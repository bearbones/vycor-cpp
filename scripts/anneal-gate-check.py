#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# Original author: Alex Mason
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""`vycor-cpp anneal` as a CI gate, end to end (the built binary).

Scenarios, each over a scratch project written by this script:

  parse_failure   a TU that fails to parse is never "no issues found":
                  exit 3 naming the TU, or exit 0 with a warning under
                  --allow-parse-failures; per-TU outcomes in the JSON;
  exit_codes      0 clean, 1 findings (and --fail-on), 2 usage;
  formats         text lines carry the check name; JSON and SARIF shape;
  sarif_schema    the SARIF validates against the vendored 2.1.0 schema
                  (tests/data/sarif-schema-2.1.0.json);
  modes           --checkpoint (fresh and resumed) and --isolate-workers
                  (with and without --checkpoint) print JSON byte-identical
                  to the in-process run;
  fingerprints    unchanged by unrelated lines inserted above a finding,
                  changed when the entities involved change;
  baseline        write, rerun (exit 0), a new finding (exit 1, only it
                  reported), a fixed one (reported stale);
  suppressions    `// vycor: ignore[check]` on the line or the line above,
                  `ignore[*]`, unused suppressions under -v;
  changed_lines   --patch-file and --git-base keep only findings in
                  changed hunks;
  source_list     --source-list from a file and from stdin.

Also a ctest: `ctest -R anneal_gate`.

    scripts/anneal-gate-check.py --binary build/src/vycor-cpp [-v] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SARIF_SCHEMA = REPO / "tests" / "data" / "sarif-schema-2.1.0.json"

# A fragile ADL resolution: use.cpp calls scale(v, 3.14) seeing only the
# int overload while ext.hpp (indexed through other.cpp) holds a better
# double overload.
CORE = """#pragma once
namespace M {
struct V {};
inline void scale(V, int) {}
}
"""
EXT = """#pragma once
#include "core.hpp"
namespace M {
inline void scale(V, double) {}
}
"""
USE = """#include "core.hpp"
void use() {
  M::V v;
  scale(v, 3.14);
}
"""
OTHER = """#include "ext.hpp"
void other() {}
"""
CLEAN = """int clean() { return 0; }
"""
BROKEN = """#include "no-such-header.hpp"
int broken() { return 0; }
"""


# ---------------------------------------------------------------------------
# A JSON Schema (draft-04) validator covering every keyword the SARIF 2.1.0
# schema uses. Used when the `jsonschema` package is not installed, so the
# check never needs the network or a pip install.
# ---------------------------------------------------------------------------

URI_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.\-]*:[^\s]*$")
URI_REF_RE = re.compile(r"^[^\s]*$")
DATE_TIME_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}(\.\d+)?([Zz]|[+-]\d{2}:\d{2})$")
HANDLED = {"$schema", "id", "title", "description", "default", "definitions",
           "type", "$ref", "properties", "additionalProperties", "required",
           "items", "minItems", "uniqueItems", "minimum", "maximum",
           "pattern", "format", "enum", "anyOf", "oneOf"}


def _type_ok(value, t: str) -> bool:
    if t == "object":
        return isinstance(value, dict)
    if t == "array":
        return isinstance(value, list)
    if t == "string":
        return isinstance(value, str)
    if t == "boolean":
        return isinstance(value, bool)
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if t == "null":
        return value is None
    raise ValueError(f"unhandled type {t}")


def mini_validate(schema: dict, root: dict, value, path: str,
                  errors: list[str]) -> None:
    unknown = set(schema) - HANDLED
    if unknown:
        raise ValueError(f"validator does not handle {sorted(unknown)}")
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/definitions/"):
            raise ValueError(f"unhandled $ref {ref}")
        mini_validate(root["definitions"][ref[len("#/definitions/"):]], root,
                      value, path, errors)
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) \
            else [schema["type"]]
        if not any(_type_ok(value, t) for t in types):
            errors.append(f"{path}: expected {types}, got {value!r:.60}")
            return
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} not in {schema['enum']}")
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: missing required {key!r}")
        for key, sub in value.items():
            if key in props:
                mini_validate(props[key], root, sub, f"{path}.{key}", errors)
            else:
                extra = schema.get("additionalProperties", True)
                if extra is False:
                    errors.append(f"{path}: unexpected property {key!r}")
                elif isinstance(extra, dict):
                    mini_validate(extra, root, sub, f"{path}.{key}", errors)
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            errors.append(f"{path}: fewer than {schema['minItems']} items")
        if schema.get("uniqueItems"):
            seen = [json.dumps(v, sort_keys=True) for v in value]
            if len(seen) != len(set(seen)):
                errors.append(f"{path}: items are not unique")
        if isinstance(schema.get("items"), dict):
            for i, sub in enumerate(value):
                mini_validate(schema["items"], root, sub, f"{path}[{i}]",
                              errors)
    if _type_ok(value, "number"):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: {value} < {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: {value} > {schema['maximum']}")
    if isinstance(value, str):
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append(f"{path}: {value!r} does not match "
                          f"{schema['pattern']!r}")
        fmt = schema.get("format")
        if fmt == "uri" and not URI_RE.match(value):
            errors.append(f"{path}: {value!r} is not a URI")
        elif fmt == "uri-reference" and not URI_REF_RE.match(value):
            errors.append(f"{path}: {value!r} is not a URI reference")
        elif fmt == "date-time" and not DATE_TIME_RE.match(value):
            errors.append(f"{path}: {value!r} is not a date-time")
    for key in ("anyOf", "oneOf"):
        if key in schema:
            matches = 0
            for alt in schema[key]:
                sub: list[str] = []
                mini_validate(alt, root, value, path, sub)
                matches += not sub
            if (key == "anyOf" and matches == 0) or \
                    (key == "oneOf" and matches != 1):
                errors.append(f"{path}: {matches} {key} alternative(s) "
                              f"matched")


def validate_sarif(doc: dict) -> list[str]:
    schema = json.loads(SARIF_SCHEMA.read_text())
    errors: list[str] = []
    mini_validate(schema, schema, doc, "$", errors)
    if os.environ.get("VYCOR_SARIF_MINI_ONLY"):
        return errors
    try:
        import jsonschema  # type: ignore
    except ImportError:
        return errors
    validator = jsonschema.Draft4Validator(
        schema, format_checker=jsonschema.FormatChecker())
    errors += [f"jsonschema: {'/'.join(map(str, e.path))}: {e.message}"
               for e in validator.iter_errors(doc)]
    return errors


# ---------------------------------------------------------------------------


class Check:
    def __init__(self, binary: Path, root: Path, verbose: bool) -> None:
        self.binary = binary
        self.root = root
        self.verbose = verbose
        self.failures: list[str] = []

    def project(self, name: str, files: dict[str, str]) -> Path:
        d = self.root / name
        d.mkdir()
        entries = []
        for rel, text in files.items():
            p = d / rel
            p.write_text(text)
            if rel.endswith(".cpp"):
                entries.append({"directory": str(d), "file": str(p),
                                "arguments": ["clang++", "-std=c++17",
                                              "-c", str(p)]})
        (d / "compile_commands.json").write_text(json.dumps(entries))
        return d

    def anneal(self, d: Path, *argv: str, sources: list[str] | None = None,
               stdin: str | None = None) -> tuple[int, str, str]:
        full = [str(self.binary), "anneal", "--build-path", str(d)]
        for s in sources or []:
            full += ["--source", str(d / s)]
        full += list(argv)
        p = subprocess.run(full, capture_output=True, text=True, cwd=d,
                           input=stdin)
        if self.verbose:
            print(f"$ anneal {' '.join(full[2:])} -> {p.returncode}",
                  file=sys.stderr)
            if p.stderr:
                print(p.stderr.rstrip(), file=sys.stderr)
        return p.returncode, p.stdout, p.stderr

    def expect(self, scenario: str, cond: bool, what: str) -> None:
        if not cond:
            self.failures.append(f"{scenario}: {what}")
            print(f"FAIL {scenario}: {what}", file=sys.stderr)

    def json_run(self, scenario: str, d: Path, *argv: str,
                 sources: list[str]) -> tuple[int, dict]:
        code, out, err = self.anneal(d, "--format", "json", *argv,
                                     sources=sources)
        try:
            return code, json.loads(out)
        except json.JSONDecodeError:
            self.expect(scenario, False,
                        f"stdout is not JSON (exit {code}): {out[:200]!r} "
                        f"stderr {err[-300:]!r}")
            return code, {}

    def adl_project(self, name: str, extra: dict[str, str] | None = None
                    ) -> Path:
        files = {"core.hpp": CORE, "ext.hpp": EXT, "use.cpp": USE,
                 "other.cpp": OTHER}
        files.update(extra or {})
        return self.project(name, files)

    # ---- scenarios ---------------------------------------------------------

    def parse_failure(self) -> None:
        name = "parse_failure"
        d = self.project(name, {"clean.cpp": CLEAN, "broken.cpp": BROKEN})
        srcs = ["clean.cpp", "broken.cpp"]
        code, out, err = self.anneal(d, sources=srcs)
        self.expect(name, "no issues found" not in out + err,
                    f"'no issues found' over a TU that failed to parse: "
                    f"{out.strip()!r}")
        self.expect(name, code == 3, f"exit {code}, expected 3")
        self.expect(name, "broken.cpp" in err and "1 failed" in err,
                    f"stderr does not name the failed TU: {err[-400:]!r}")
        self.expect(name, "1 analyzed" in err,
                    f"no 'N analyzed, M failed' summary: {err[-400:]!r}")

        code, out, err = self.anneal(d, "--allow-parse-failures",
                                     sources=srcs)
        self.expect(name, code == 0,
                    f"--allow-parse-failures: exit {code}, expected 0")
        self.expect(name, "warning" in err.lower() and "broken.cpp" in err,
                    f"--allow-parse-failures: no warning naming the TU: "
                    f"{err[-400:]!r}")
        self.expect(name, "no issues found" not in out,
                    "--allow-parse-failures still printed 'no issues found'")

        code, doc = self.json_run(name, d, sources=srcs)
        tus = {Path(t["file"]).name: t["status"] for t in doc.get("tus", [])}
        self.expect(name, tus == {"clean.cpp": "analyzed",
                                  "broken.cpp": "partial"},
                    f"per-TU outcomes {tus}")
        summary = doc.get("summary", {})
        self.expect(name, summary.get("analyzed") == 1 and
                    summary.get("failed") == 1, f"summary {summary}")

        # A source that does not exist is not analyzed either.
        code, out, err = self.anneal(d, sources=["clean.cpp", "missing.cpp"])
        self.expect(name, code == 3 and "missing.cpp" in err,
                    f"a missing source: exit {code}, {err[-300:]!r}")

    def exit_codes(self) -> None:
        name = "exit_codes"
        d = self.adl_project(name, {"clean.cpp": CLEAN})
        code, out, _ = self.anneal(d, sources=["clean.cpp"])
        self.expect(name, code == 0, f"clean run: exit {code}")
        self.expect(name, "no issues found" in out,
                    f"clean run: {out.strip()!r}")
        srcs = ["use.cpp", "other.cpp"]
        code, out, _ = self.anneal(d, sources=srcs)
        self.expect(name, code == 1, f"findings: exit {code}, expected 1")
        code, _, _ = self.anneal(d, "--fail-on", "error", sources=srcs)
        self.expect(name, code == 0,
                    f"--fail-on error over warnings: exit {code}")
        code, _, _ = self.anneal(d, "--fail-on", "warning", sources=srcs)
        self.expect(name, code == 1, f"--fail-on warning: exit {code}")
        code, _, _ = self.anneal(d, "--fail-on", "none", sources=srcs)
        self.expect(name, code == 0, f"--fail-on none: exit {code}")
        for argv, what in (
                (["--format", "xml"], "unknown --format"),
                (["--no-such-flag"], "unknown flag"),
                (["--fail-on", "fatal"], "unknown --fail-on"),
                (["--checks", "no-such-check"], "unknown check"),
                (["--baseline", str(d / "missing.json")], "missing baseline"),
                (["--patch-file", "x", "--git-base", "HEAD"],
                 "--patch-file with --git-base"),
        ):
            code, _, _ = self.anneal(d, *argv, sources=srcs)
            self.expect(name, code == 2, f"{what}: exit {code}, expected 2")
        code, _, _ = self.anneal(d)
        self.expect(name, code == 2, f"no --source: exit {code}, expected 2")
        p = subprocess.run([str(self.binary), "anneal", "--source", "x.cpp"],
                           capture_output=True, text=True, cwd=d)
        self.expect(name, p.returncode == 2,
                    f"no --build-path: exit {p.returncode}, expected 2")

    def formats(self) -> None:
        name = "formats"
        d = self.adl_project(name)
        srcs = ["use.cpp", "other.cpp"]
        code, out, _ = self.anneal(d, sources=srcs)
        lines = [l for l in out.splitlines() if l.strip()]
        self.expect(name, any(re.match(r"^use\.cpp:4:3: \[adl-visibility\] "
                                       r"Fragile ADL", l) for l in lines),
                    f"text lines: {lines}")
        code, doc = self.json_run(name, d, sources=srcs)
        findings = doc.get("findings", [])
        self.expect(name, len(findings) == 1, f"findings {findings}")
        if findings:
            f = findings[0]
            for key in ("check", "kind", "severity", "file", "line",
                        "column", "message", "fingerprint"):
                self.expect(name, key in f, f"finding lacks {key!r}: {f}")
            self.expect(name, f.get("check") == "adl-visibility" and
                        f.get("file") == "use.cpp" and f.get("line") == 4,
                        f"finding {f}")
            self.expect(name, re.fullmatch(r"[0-9a-f]{16}",
                                           f.get("fingerprint", "")),
                        f"fingerprint {f.get('fingerprint')!r}")
        out_file = d / "out.json"
        code, out, _ = self.anneal(d, "--format", "json", "--output",
                                   str(out_file), sources=srcs)
        self.expect(name, code == 1 and out == "" and
                    json.loads(out_file.read_text()) == doc,
                    "--output did not write the same JSON document")

    def sarif_schema(self) -> None:
        name = "sarif_schema"
        d = self.adl_project(name, {"broken.cpp": BROKEN})
        code, out, err = self.anneal(d, "--format", "sarif",
                                     "--allow-parse-failures",
                                     sources=["use.cpp", "other.cpp",
                                              "broken.cpp"])
        try:
            doc = json.loads(out)
        except json.JSONDecodeError:
            self.expect(name, False, f"SARIF is not JSON: {out[:200]!r}")
            return
        errors = validate_sarif(doc)
        self.expect(name, not errors, "schema errors:\n  " +
                    "\n  ".join(errors[:20]))
        # The validator is not vacuous: a damaged copy must fail it.
        damaged = json.loads(out)
        del damaged["version"]
        damaged["runs"][0]["results"][0]["level"] = "fatal"
        damaged["runs"][0]["tool"]["driver"]["rules"][0]["helpUri"] = "x y"
        self.expect(name, len(validate_sarif(damaged)) >= 3,
                    f"a damaged SARIF passed: {validate_sarif(damaged)}")
        run = doc["runs"][0]
        rules = run["tool"]["driver"]["rules"]
        ids = [r["id"] for r in rules]
        self.expect(name, "adl-visibility" in ids, f"rules {ids}")
        rule = next((r for r in rules if r["id"] == "adl-visibility"), {})
        self.expect(name, rule.get("helpUri", "").endswith(
            "docs/checks/adl-visibility.md"), f"helpUri {rule}")
        results = run["results"]
        self.expect(name, len(results) == 1, f"results {results}")
        if results:
            r = results[0]
            self.expect(name, "vycorFingerprint/v1" in r.get("partialFingerprints", {}),
                        f"partialFingerprints {r.get('partialFingerprints')}")
            self.expect(name, ids[r["ruleIndex"]] == r["ruleId"],
                        f"ruleIndex {r['ruleIndex']} != {r['ruleId']}")
        inv = run["invocations"][0]
        notes = json.dumps(inv.get("toolExecutionNotifications", []))
        self.expect(name, "broken.cpp" in notes,
                    f"the failed TU is not a notification: {notes[:300]}")
        if self.verbose:
            (self.root / "sample.sarif").write_text(out)

    def modes(self) -> None:
        name = "modes"
        d = self.adl_project(name, {"broken.cpp": BROKEN, "clean.cpp": CLEAN})
        srcs = ["use.cpp", "other.cpp", "clean.cpp", "broken.cpp"]
        base = ["--format", "json", "--allow-parse-failures"]

        def run(*extra: str) -> str:
            code, out, err = self.anneal(d, *base, *extra, sources=srcs)
            self.expect(name, code == 1,
                        f"{' '.join(extra) or 'in-process'}: exit {code}: "
                        f"{err[-300:]!r}")
            return out

        reference = run("--threads", "1")
        self.expect(name, json.loads(reference)["summary"]["findings"] == 1,
                    f"reference run: {reference[:300]}")
        ckpt = d / "anneal.ckpt"
        ckpt2 = d / "anneal2.ckpt"
        variants = [
            ("threads 4", ["--threads", "4"]),
            ("checkpoint fresh", ["--checkpoint", str(ckpt)]),
            ("checkpoint resumed", ["--checkpoint", str(ckpt)]),
            ("isolate-workers", ["--isolate-workers", "--workers", "2"]),
            ("isolate-workers + checkpoint",
             ["--isolate-workers", "--workers", "2", "--checkpoint",
              str(ckpt2)]),
            ("isolate-workers + checkpoint resumed",
             ["--isolate-workers", "--workers", "2", "--checkpoint",
              str(ckpt2)]),
        ]
        for label, extra in variants:
            out = run(*extra)
            self.expect(name, out == reference,
                        f"{label}: JSON differs from the in-process run\n"
                        f"  in-process: {reference[:400]}\n"
                        f"  {label}: {out[:400]}")

    def fingerprints(self) -> None:
        name = "fingerprints"
        d = self.adl_project(name)
        srcs = ["use.cpp", "other.cpp"]

        def fps() -> list[tuple[int, str]]:
            _, doc = self.json_run(name, d, sources=srcs)
            return [(f["line"], f["fingerprint"])
                    for f in doc.get("findings", [])]

        before = fps()
        (d / "use.cpp").write_text("// a comment\n\nint unrelated = 1;\n" +
                                   USE)
        after = fps()
        self.expect(name, len(before) == 1 and len(after) == 1 and
                    before[0][0] != after[0][0] and
                    before[0][1] == after[0][1],
                    f"inserted lines changed the fingerprint: {before} -> "
                    f"{after}")
        # The involved entities change: struct V becomes W.
        for f in ("core.hpp", "ext.hpp", "use.cpp"):
            p = d / f
            p.write_text(re.sub(r"\bV\b", "W", p.read_text()))
        renamed = fps()
        self.expect(name, len(renamed) == 1 and
                    renamed[0][1] != before[0][1],
                    f"renaming the entity kept the fingerprint: {before} -> "
                    f"{renamed}")

    def baseline(self) -> None:
        name = "baseline"
        d = self.adl_project(name)
        srcs = ["use.cpp", "other.cpp"]
        bl = d / "baseline.json"
        code, _, err = self.anneal(d, "--write-baseline", str(bl),
                                   sources=srcs)
        self.expect(name, code == 0 and bl.exists(),
                    f"--write-baseline: exit {code}: {err[-300:]!r}")
        code, doc = self.json_run(name, d, "--baseline", str(bl),
                                  sources=srcs)
        self.expect(name, code == 0 and not doc.get("findings") and
                    doc.get("summary", {}).get("baselined") == 1,
                    f"rerun over the baseline: exit {code}, "
                    f"{doc.get('summary')}")
        # A new finding in a new TU.
        (d / "use2.cpp").write_text(USE.replace("use()", "use2()"))
        entries = json.loads((d / "compile_commands.json").read_text())
        p = d / "use2.cpp"
        entries.append({"directory": str(d), "file": str(p),
                        "arguments": ["clang++", "-std=c++17", "-c",
                                      str(p)]})
        (d / "compile_commands.json").write_text(json.dumps(entries))
        srcs2 = srcs + ["use2.cpp"]
        code, doc = self.json_run(name, d, "--baseline", str(bl),
                                  sources=srcs2)
        files = [f["file"] for f in doc.get("findings", [])]
        self.expect(name, code == 1 and files == ["use2.cpp"],
                    f"new finding: exit {code}, reported {files}")
        # Fix the baselined one.
        (d / "use.cpp").write_text(USE.replace("3.14", "3"))
        code, doc = self.json_run(name, d, "--baseline", str(bl),
                                  sources=srcs2)
        stale = doc.get("staleBaseline", [])
        self.expect(name, code == 1 and len(stale) == 1 and
                    doc.get("summary", {}).get("staleBaseline") == 1,
                    f"fixed finding: exit {code}, stale {stale}")
        code, out, err = self.anneal(d, "--baseline", str(bl), sources=srcs2)
        self.expect(name, "1 baseline entr" in err and "stale" in err,
                    f"text mode does not report the stale entry: "
                    f"{err[-300:]!r}")

    def suppressions(self) -> None:
        name = "suppressions"
        variants = {
            "line_above": USE.replace("  scale(v, 3.14);",
                                      "  // vycor: ignore[adl-visibility]\n"
                                      "  scale(v, 3.14);"),
            "same_line": USE.replace("scale(v, 3.14);",
                                     "scale(v, 3.14); "
                                     "// vycor: ignore[adl-visibility]"),
            "star": USE.replace("  scale(v, 3.14);",
                                "  // vycor: ignore[*]\n  scale(v, 3.14);"),
            "list": USE.replace("  scale(v, 3.14);",
                                "  // vycor: ignore[dead-code, "
                                "adl-visibility]\n  scale(v, 3.14);"),
        }
        for label, text in variants.items():
            d = self.adl_project(f"{name}_{label}", {"use.cpp": text})
            code, doc = self.json_run(name, d, sources=["use.cpp",
                                                        "other.cpp"])
            self.expect(name, code == 0 and not doc.get("findings") and
                        doc.get("summary", {}).get("suppressed") == 1,
                        f"{label}: exit {code}, {doc.get('summary')}")
        # Wrong check name, or two lines above: not suppressed.
        for label, text in {
            "other_check": USE.replace("  scale(v, 3.14);",
                                       "  // vycor: ignore[dead-code]\n"
                                       "  scale(v, 3.14);"),
            "too_far": USE.replace("  scale(v, 3.14);",
                                   "  // vycor: ignore[adl-visibility]\n\n"
                                   "  scale(v, 3.14);"),
        }.items():
            d = self.adl_project(f"{name}_{label}", {"use.cpp": text})
            code, out, err = self.anneal(d, "-v",
                                         sources=["use.cpp", "other.cpp"])
            self.expect(name, code == 1, f"{label}: exit {code}")
            self.expect(name, "unused suppression" in err,
                        f"{label}: -v does not report the unused "
                        f"suppression: {err[-300:]!r}")
            code, out, err = self.anneal(d, sources=["use.cpp", "other.cpp"])
            self.expect(name, "unused suppression" not in err,
                        f"{label}: unused suppression reported without -v")

    def changed_lines(self) -> None:
        name = "changed_lines"
        d = self.adl_project(name, {"use2.cpp": USE.replace("use()",
                                                            "use2()")})
        srcs = ["use.cpp", "use2.cpp", "other.cpp"]
        code, doc = self.json_run(name, d, sources=srcs)
        self.expect(name, len(doc.get("findings", [])) == 2,
                    f"unfiltered: {doc.get('findings')}")
        patch = d / "change.patch"
        patch.write_text(
            "diff --git a/use2.cpp b/use2.cpp\n"
            "--- a/use2.cpp\n+++ b/use2.cpp\n"
            "@@ -4,1 +4,1 @@\n-  scale(v, 3.0);\n+  scale(v, 3.14);\n")
        code, doc = self.json_run(name, d, "--patch-file", str(patch),
                                  sources=srcs)
        files = [f["file"] for f in doc.get("findings", [])]
        self.expect(name, code == 1 and files == ["use2.cpp"] and
                    doc.get("summary", {}).get("outsideChanges") == 1,
                    f"--patch-file: exit {code}, {files}, "
                    f"{doc.get('summary')}")
        patch.write_text(
            "diff --git a/use2.cpp b/use2.cpp\n"
            "--- a/use2.cpp\n+++ b/use2.cpp\n"
            "@@ -1,1 +1,1 @@\n-// x\n+#include \"core.hpp\"\n")
        code, doc = self.json_run(name, d, "--patch-file", str(patch),
                                  sources=srcs)
        self.expect(name, code == 0 and not doc.get("findings"),
                    f"a change away from every finding: exit {code}")

        git = shutil.which("git")
        if not git:
            print(f"skip {name} --git-base: no git", file=sys.stderr)
            return
        env = {**os.environ, "GIT_AUTHOR_NAME": "t",
               "GIT_AUTHOR_EMAIL": "t@example.com", "GIT_COMMITTER_NAME": "t",
               "GIT_COMMITTER_EMAIL": "t@example.com"}

        def g(*argv: str) -> None:
            subprocess.run([git, *argv], cwd=d, env=env, check=True,
                           capture_output=True)

        (d / "use2.cpp").write_text(USE.replace("use()", "use2()")
                                    .replace("3.14", "3"))
        g("init", "-q")
        g("add", "-A")
        g("commit", "-q", "-m", "base")
        (d / "use2.cpp").write_text(USE.replace("use()", "use2()"))
        code, doc = self.json_run(name, d, "--git-base", "HEAD",
                                  sources=srcs)
        files = [f["file"] for f in doc.get("findings", [])]
        self.expect(name, code == 1 and files == ["use2.cpp"],
                    f"--git-base HEAD: exit {code}, {files}")

    def source_list(self) -> None:
        name = "source_list"
        d = self.adl_project(name)
        lst = d / "sources.txt"
        lst.write_text("# changed files\nuse.cpp\nother.cpp\n")
        code, doc = self.json_run(name, d, "--source-list", str(lst),
                                  sources=[])
        self.expect(name, code == 1 and len(doc.get("findings", [])) == 1
                    and len(doc.get("tus", [])) == 2,
                    f"--source-list file: exit {code}, {doc.get('summary')}")
        code, out, err = self.anneal(d, "--format", "json", "--source-list",
                                     "-", stdin="use.cpp\nother.cpp\n")
        self.expect(name, code == 1 and
                    len(json.loads(out or "{}").get("tus", [])) == 2,
                    f"--source-list -: exit {code}: {err[-300:]!r}")
        code, _, _ = self.anneal(d, "--source-list", str(d / "missing.txt"))
        self.expect(name, code == 2, f"unreadable list: exit {code}")


SCENARIOS = ["parse_failure", "exit_codes", "formats", "sarif_schema",
             "modes", "fingerprints", "baseline", "suppressions",
             "changed_lines", "source_list"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--only", action="append", choices=SCENARIOS)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch directory")
    args = ap.parse_args()
    root = Path(tempfile.mkdtemp(prefix="vycor-anneal-gate-"))
    check = Check(args.binary.resolve(), root, args.verbose)
    try:
        for scenario in args.only or SCENARIOS:
            before = len(check.failures)
            getattr(check, scenario)()
            print(f"{'ok  ' if len(check.failures) == before else 'FAIL'} "
                  f"{scenario}")
    finally:
        if args.keep:
            print(f"scratch kept at {root}", file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)
    if check.failures:
        print(f"\n{len(check.failures)} failure(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
