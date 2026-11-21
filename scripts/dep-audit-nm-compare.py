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
"""Compare the dependency audit against the object files of a built tree
(docs/dependency-audit.md, "Checking the audit against the objects").

For every pair of targets the script works out two answers to "which
symbols of the provider does the consumer need":

  objects  the undefined symbols of the consumer's object files that the
           provider's object files define (`llvm-nm`). What the linker
           resolves; the reference.
  index    the seeds `megascope explain-dependency` reports for the pair.

and says where they differ and why:

  objects only   variables; vtables and typeinfo; functions, split by what
                 the index holds about the function (a call from one of the
                 consumer's sources whose callee the audit could not place
                 in the provider; calls only from header bodies; no call
                 edge at all; no such function).
  index only     split by how the audit knew the symbol (defined, reached
                 through an inline body, presumed owner) and by what the
                 objects say (the provider defines it but the consumer has
                 no undefined reference; the consumer emits its own copy;
                 neither side has such a symbol).

Functions are compared by qualified name (`llvm-cxxfilt --no-params`,
template arguments dropped), because the index identifies functions by
USR and the objects by mangled name; overloads therefore count once.

    scripts/dep-audit-nm-compare.py --binary build/src/vycor-cpp \\
        --index build/.vycor/megascope.vycs --target-map targets.json \\
        [--out report.json]

The target map must name each target's object files (`objects`) or its
static archive (`artifact`), as scripts/cmake-target-map.py writes them.
Targets whose objects are not on disk are left out and counted.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPORT_VERSION = 1
CHUNK = 400  # object files per llvm-nm invocation
CALLER_LIMIT = 20000

VTABLE_PREFIXES = ("_ZTV", "_ZTT", "_ZTC")
TYPEINFO_PREFIXES = ("_ZTI", "_ZTS")
FUNCTION_TYPES = set("TtWwi")
KEPT_OPERATOR_WORDS = ("new", "delete", "new[]", "delete[]", "co_await")


def fail(message: str, code: int = 2) -> "NoReturn":  # noqa: F821
    print(f"dep-audit-nm-compare: {message}", file=sys.stderr)
    sys.exit(code)


# --- Object side ---------------------------------------------------------


def nm_files(nm: str, files: list[str], strip_underscore: bool = False):
    """Yield (name, type letter) for every symbol of the given files.

    Mach-O prefixes every C-level symbol with one underscore (`__ZTV1S`
    for the Itanium name `_ZTV1S`); `strip_underscore` removes it so the
    names compare and classify as on ELF.
    """
    for i in range(0, len(files), CHUNK):
        out = subprocess.run([nm, "-P", "--no-sort", *files[i:i + CHUNK]],
                             check=True, capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            # "<name> <type> [value size]"; file and member headers end in ':'
            if len(parts) < 2 or len(parts[1]) != 1 or line.endswith(":"):
                continue
            name = parts[0]
            if strip_underscore and name.startswith("_"):
                name = name[1:]
            yield name, parts[1]


class TargetObjects:
    """What one target's objects need and provide, by mangled name."""

    def __init__(self):
        self.undefined: set[str] = set()
        self.defined: dict[str, str] = {}  # global definitions -> type
        self.local: set[str] = set()       # internal-linkage definitions

    def finish(self):
        self.undefined -= self.defined.keys()


def read_target(nm: str, files: list[str], strip_underscore: bool = False) -> TargetObjects:
    t = TargetObjects()
    for name, kind in nm_files(nm, files, strip_underscore):
        if kind in "Uwv":
            t.undefined.add(name)
        elif kind.isupper() or kind == "u":
            t.defined[name] = kind
        else:
            t.local.add(name)
    t.finish()
    return t


def symbol_class(name: str, kind: str) -> str:
    if name.startswith(VTABLE_PREFIXES):
        return "vtable"
    if name.startswith(TYPEINFO_PREFIXES):
        return "typeinfo"
    return "function" if kind in FUNCTION_TYPES else "variable"


# --- Names ---------------------------------------------------------------

_ABI_TAG = re.compile(r"\[abi:[^\]]*\]")


_OPERATOR = re.compile(
    r"operator(?:<=>|<<=|>>=|->\*|<<|>>|<=|>=|->|\(\)|\[\]|==|!=|\+=|-=|\*=|/=|%=|\^=|&=|\|=|&&|\|\||\+\+|--"
    r"|[<>=!+\-*/%^&|~,])?")


def strip_templates(name: str) -> str:
    """Drop template argument lists; keep the angle brackets of operators."""
    out = []
    depth = 0
    i = 0
    n = len(name)
    while i < n:
        c = name[i]
        if depth == 0 and c == "o" and name.startswith("operator", i) and (
                i == 0 or not (name[i - 1].isalnum() or name[i - 1] == "_")):
            token = _OPERATOR.match(name, i).group(0)
            out.append(token)
            i += len(token)
            continue
        if c == "<":
            depth += 1
        elif c == ">" and depth > 0:
            depth -= 1
        elif depth == 0:
            out.append(c)
        i += 1
    return "".join(out)


def strip_params(name: str) -> str:
    """Cut a demangled signature at its parameter list (fallback path)."""
    depth = 0
    i = 0
    n = len(name)
    while i < n:
        c = name[i]
        if c == "<":
            depth += 1
        elif c == ">" and depth > 0:
            depth -= 1
        elif c == "(" and depth == 0:
            if name.startswith("(anonymous namespace)", i):
                i += len("(anonymous namespace)")
                continue
            if name[:i].endswith("operator"):
                i += 2
                continue
            return name[:i]
        i += 1
    return name


def normalize(name: str) -> str:
    name = _ABI_TAG.sub("", name)
    name = strip_templates(name)
    at = name.rfind("operator ")
    if at >= 0 and (at == 0 or name[at - 1] == ":"):
        word = name[at + len("operator "):]
        if word not in KEPT_OPERATOR_WORDS:
            name = name[:at] + "operator <conversion>"
    return name.strip()


def demangle(cxxfilt: str, names: list[str]) -> dict[str, str]:
    """Mangled name -> normalized qualified name."""
    if not names:
        return {}
    text = "\n".join(names) + "\n"
    out = subprocess.run([cxxfilt, "--no-params"], input=text, check=True,
                         capture_output=True, text=True).stdout.splitlines()
    if len(out) != len(names):
        fail(f"{cxxfilt} answered {len(out)} lines for {len(names)} names")
    # --no-params gives up on a few manglings (conversion operators): those
    # are demangled in full and cut at the parameter list.
    retry = [i for i, (m, d) in enumerate(zip(names, out)) if d == m and m.startswith("_Z")]
    if retry:
        full = subprocess.run([cxxfilt], input="\n".join(names[i] for i in retry) + "\n",
                              check=True, capture_output=True, text=True).stdout.splitlines()
        if len(full) != len(retry):
            fail(f"{cxxfilt} answered {len(full)} lines for {len(retry)} names")
        for i, d in zip(retry, full):
            out[i] = strip_params(d)
    result = {}
    for mangled, plain in zip(names, out):
        for prefix in ("non-virtual thunk to ", "virtual thunk to "):
            if plain.startswith(prefix):
                plain = plain[len(prefix):]
        result[mangled] = normalize(plain)
    return result


# --- Index side ----------------------------------------------------------


def megascope(binary: str, index: str, argv: list[str]) -> dict:
    proc = subprocess.run([binary, "megascope", *argv, "--index", index],
                          capture_output=True, text=True)
    if not proc.stdout.strip():
        fail(f"`megascope {' '.join(argv)}` failed: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def batch(binary: str, index: str, requests: list[dict]):
    """Run requests through `megascope batch`; the results, in order.

    Fails when the answers do not match the requests one for one: a batch
    that died part-way would otherwise silently drop the tail.
    """
    if not requests:
        return []
    proc = subprocess.Popen([binary, "megascope", "batch", "--index", index],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def feed():
        for r in requests:
            proc.stdin.write(json.dumps(r) + "\n")
        proc.stdin.close()

    import threading
    writer = threading.Thread(target=feed, daemon=True)
    writer.start()
    results = [json.loads(line).get("result", {}) for line in proc.stdout]
    writer.join()
    proc.wait()
    if len(results) != len(requests):
        fail(f"`megascope batch` answered {len(results)} of {len(requests)} requests "
             f"(exit code {proc.returncode})")
    return results


def site_file(site: str) -> str:
    return site.rsplit(":", 2)[0]


# --- Comparison ----------------------------------------------------------

INDEX_HOW = ("defined", "viaInline", "presumed")
OBJECT_SAYS = ("noUndefinedReference", "consumerEmitsItsOwn", "noSuchSymbol")
MISSED_WHY = ("calleeNotPlaced", "headerBodyOnly", "noCallEdge", "notInIndex")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True, help="the built vycor-cpp")
    ap.add_argument("--index", required=True, help="the megascope index of the tree")
    ap.add_argument("--target-map", required=True, type=Path)
    ap.add_argument("--nm", default=shutil.which("llvm-nm") or "nm")
    ap.add_argument("--cxxfilt", default=shutil.which("llvm-cxxfilt") or "c++filt")
    ap.add_argument("--strip-underscore", choices=("auto", "yes", "no"), default="auto",
                    help="drop the leading underscore Mach-O puts on every symbol "
                         "(auto: yes on macOS)")
    ap.add_argument("--out", type=Path, help="write the full JSON report here")
    ap.add_argument("--top", type=int, default=15, help="rows per table in the text summary")
    args = ap.parse_args()

    tmap = json.loads(args.target_map.read_text())
    root = tmap.get("root") or str(args.target_map.resolve().parent)

    def resolve(p: str) -> str:
        return os.path.normpath(p if os.path.isabs(p) else os.path.join(root, p))

    strip_underscore = args.strip_underscore == "yes" or (
        args.strip_underscore == "auto" and sys.platform == "darwin")

    # 1. Objects.
    objects: dict[str, TargetObjects] = {}
    sources: dict[str, set[str]] = {}
    not_built = []
    for t in tmap["targets"]:
        sources[t["name"]] = {resolve(s) for s in t.get("sources", [])}
        files = [f for f in t.get("objects", []) if os.path.isfile(f)]
        if t.get("objects") and len(files) != len(t["objects"]):
            files = []  # a partly built target would under-report its needs
        artifact = t.get("artifact", "")
        if not files and artifact.endswith((".a", ".lib")) and os.path.isfile(artifact):
            files = [artifact]
        if not files:
            if t.get("sources"):
                not_built.append(t["name"])
            continue
        objects[t["name"]] = read_target(args.nm, files, strip_underscore)
    if len(objects) < 2:
        fail("fewer than two targets of the map have object files on disk; "
             "build the tree and regenerate the map (see scripts/cmake-target-map.py)")
    all_sources = set().union(*sources.values())

    mangled = set()
    for t in objects.values():
        mangled |= t.undefined | t.defined.keys() | t.local
    ordered = sorted(mangled)
    plain = demangle(args.cxxfilt, ordered)

    providers_of: dict[str, list[str]] = defaultdict(list)
    for name, t in objects.items():
        for sym in t.defined:
            providers_of[sym].append(name)

    # truth[(consumer, provider)] -> {mangled: class}
    truth: dict[tuple[str, str], dict[str, str]] = defaultdict(dict)
    for consumer, t in objects.items():
        for sym in t.undefined:
            for provider in providers_of.get(sym, ()):
                if provider != consumer:
                    truth[(consumer, provider)][sym] = symbol_class(sym, objects[provider].defined[sym])

    defined_names = {n: {plain[s] for s in t.defined} for n, t in objects.items()}
    own_names = {n: {plain[s] for s in t.defined} | {plain[s] for s in t.local} for n, t in objects.items()}
    undefined_names = {n: {plain[s] for s in t.undefined} for n, t in objects.items()}

    # 2. Index.
    audit = megascope(args.binary, args.index,
                      ["audit-dependencies", "--target-map", str(args.target_map), "--limit", "10000000"])
    if audit.get("status") != "ok":
        fail(f"audit-dependencies answered {audit.get('status')}: {audit.get('error', '')}")
    rows = {(r["consumer"], r["provider"]): r for r in audit["edges"]}
    covered: dict[str, bool] = {}
    for r in rows.values():
        covered[r["consumer"]] = r["consumerCovered"]
        covered[r["provider"]] = r["providerCovered"]

    seeded = [k for k, r in rows.items()
              if (r["symbols"] or r["uncertainSymbols"]) and k[0] in objects and k[1] in objects]
    requests = [{"tool": "explain_dependency",
                 "args": {"target_map": str(args.target_map), "consumer": c, "provider": p,
                          "closure_limit": 0, "limit": 10000000}} for c, p in seeded]
    # index_names[(consumer, provider)] -> {name: how}
    index_names: dict[tuple[str, str], dict[str, str]] = {}
    for key, result in zip(seeded, batch(args.binary, args.index, requests)):
        names: dict[str, str] = {}
        for seed in result.get("seeds", []):
            name = normalize(seed["symbol"]["name"])
            how = "presumed" if seed["owner"] == "presumed" else ("viaInline" if seed["via"] != "direct" else "defined")
            if INDEX_HOW.index(how) < INDEX_HOW.index(names.get(name, "presumed")) or name not in names:
                names[name] = how
        index_names[key] = names

    # 3. Per pair.
    pairs = sorted(set(truth) | {k for k in rows if k[0] in objects and k[1] in objects})
    edges = []
    missed: dict[tuple[str, str], set[str]] = {}
    for key in pairs:
        consumer, provider = key
        need = truth.get(key, {})
        by_class = Counter(need.values())
        need_functions = {plain[s] for s, c in need.items() if c == "function"}
        idx = index_names.get(key, {})
        both = need_functions & idx.keys()
        only_objects = need_functions - idx.keys()
        only_index = idx.keys() - need_functions
        matrix = {h: {o: 0 for o in OBJECT_SAYS} for h in INDEX_HOW}
        for name in only_index:
            if name in defined_names[provider]:
                says = "noUndefinedReference"
            elif name in own_names[consumer]:
                says = "consumerEmitsItsOwn"
            else:
                says = "noSuchSymbol"
            matrix[idx[name]][says] += 1
        row = rows.get(key)
        missed[key] = only_objects
        edges.append({
            "consumer": consumer, "provider": provider,
            "declared": row["declared"] if row else "none",
            "covered": bool(covered.get(consumer)) and bool(covered.get(provider)),
            "findings": [f["id"] for f in row["findings"]] if row else [],
            "index": {"symbols": row["symbols"] if row else 0,
                      "uncertainSymbols": row["uncertainSymbols"] if row else 0,
                      "names": len(idx)},
            "objects": {"symbols": len(need), "functions": by_class["function"],
                        "functionNames": len(need_functions), "variables": by_class["variable"],
                        "vtables": by_class["vtable"], "typeinfo": by_class["typeinfo"]},
            "both": len(both), "objectsOnly": len(only_objects), "indexOnly": len(only_index),
            "indexOnlyBy": matrix,
        })

    # 4. Why the index missed a function the objects need.
    wanted = sorted(set().union(*missed.values())) if missed else []
    callers: dict[str, set[str] | None] = {}
    truncated_callers = 0

    def collect(result: dict) -> set[str]:
        nonlocal truncated_callers
        if result.get("truncated"):
            truncated_callers += 1
        return {site_file(c["callSite"]) for c in result.get("callers", [])}

    first = [{"tool": "get_callers", "args": {"name": n, "limit": CALLER_LIMIT}} for n in wanted]
    follow: list[tuple[str, str]] = []
    for name, result in zip(wanted, batch(args.binary, args.index, first)):
        status = result.get("status")
        if status == "ok":
            callers[name] = collect(result)
        elif status == "ambiguous":
            callers[name] = set()
            follow += [(name, c["usr"]) for c in result.get("candidates", []) if c.get("usr")]
        else:
            callers[name] = None
    second = [{"tool": "get_callers", "args": {"usr": usr, "limit": CALLER_LIMIT}} for _, usr in follow]
    for (name, _), result in zip(follow, batch(args.binary, args.index, second)):
        if result.get("status") == "ok":
            callers[name] |= collect(result)

    for edge in edges:
        why = Counter()
        for name in missed[(edge["consumer"], edge["provider"])]:
            files = callers.get(name)
            if files is None:
                why["notInIndex"] += 1
            elif files & sources[edge["consumer"]]:
                why["calleeNotPlaced"] += 1
            elif files - all_sources:
                why["headerBodyOnly"] += 1
            else:
                why["noCallEdge"] += 1
        edge["objectsOnlyBy"] = {w: why[w] for w in MISSED_WHY}

    # 5. Totals.
    def totals(selected: list[dict]) -> dict:
        t = {
            "pairs": len(selected),
            "pairsObjectsSee": sum(1 for e in selected if e["objects"]["symbols"]),
            "pairsIndexSees": sum(1 for e in selected if e["index"]["names"]),
            "pairsBothSee": sum(1 for e in selected if e["objects"]["symbols"] and e["index"]["names"]),
            "pairsOnlyObjectsSee": sum(1 for e in selected if e["objects"]["symbols"] and not e["index"]["names"]),
            "pairsOnlyDataSymbols": sum(1 for e in selected
                                        if e["objects"]["symbols"] and not e["objects"]["functions"]),
            "pairsOnlyIndexSees": sum(1 for e in selected if not e["objects"]["symbols"] and e["index"]["names"]),
            "objectSymbols": {k: sum(e["objects"][k] for e in selected)
                              for k in ("symbols", "functions", "functionNames", "variables", "vtables", "typeinfo")},
            "functionNames": {
                "both": sum(e["both"] for e in selected),
                "objectsOnly": sum(e["objectsOnly"] for e in selected),
                "indexOnly": sum(e["indexOnly"] for e in selected),
            },
            "objectsOnlyBy": {w: sum(e["objectsOnlyBy"][w] for e in selected) for w in MISSED_WHY},
            "indexOnlyBy": {h: {o: sum(e["indexOnlyBy"][h][o] for e in selected) for o in OBJECT_SAYS}
                            for h in INDEX_HOW},
        }
        declared = [e for e in selected if e["declared"] in ("direct", "closure")]
        flagged = [e for e in declared if "unused-link-dependency" in e["findings"]]
        t["unused"] = {
            "declaredPairs": len(declared),
            "objectsUnused": sum(1 for e in declared if not e["objects"]["symbols"]),
            "flagged": len(flagged),
            "flaggedConfirmed": sum(1 for e in flagged if not e["objects"]["symbols"]),
            "flaggedButFunctionsNeeded": sum(1 for e in flagged if e["objects"]["functions"]),
            "flaggedButOnlyDataNeeded": sum(1 for e in flagged
                                            if e["objects"]["symbols"] and not e["objects"]["functions"]),
            "objectsUnusedNotFlagged": sum(1 for e in declared if not e["objects"]["symbols"]
                                           and "unused-link-dependency" not in e["findings"]),
        }
        thin = [e for e in selected if "thin-dependency" in e["findings"]]
        t["thin"] = {
            "flagged": len(thin),
            "objectsAgreeAtMost5": sum(1 for e in thin if e["objects"]["symbols"] <= 5),
            "objectsNeedMoreThan5": sum(1 for e in thin if e["objects"]["symbols"] > 5),
            "objectsNeedNone": sum(1 for e in thin if not e["objects"]["symbols"]),
        }
        undeclared = [e for e in selected if "undeclared-link-dependency" in e["findings"]]
        t["undeclared"] = {
            "flagged": len(undeclared),
            "objectsConfirm": sum(1 for e in undeclared if e["objects"]["symbols"]),
        }
        return t

    report = {
        "reportVersion": REPORT_VERSION,
        "targetMap": str(args.target_map),
        "index": args.index,
        "generator": tmap.get("generator", ""),
        "depsForm": tmap.get("depsForm", "direct"),
        "completeness": audit.get("completeness"),
        "targets": {"inMap": len(tmap["targets"]), "withObjects": len(objects),
                    "notBuilt": sorted(not_built)},
        "callerListsTruncated": truncated_callers,
        "all": totals(edges),
        "covered": totals([e for e in edges if e["covered"]]),
        "edges": edges,
    }
    if args.out:
        args.out.write_text(json.dumps(report, indent=1) + "\n")

    # 6. Text summary.
    def show(title: str, t: dict):
        f = t["functionNames"]
        o = t["objectSymbols"]
        print(f"\n{title}: {t['pairs']} target pairs")
        print(f"  pairs the objects connect {t['pairsObjectsSee']}, the index connects {t['pairsIndexSees']}, "
              f"both {t['pairsBothSee']}, objects only {t['pairsOnlyObjectsSee']} "
              f"(of all object pairs, {t['pairsOnlyDataSymbols']} carry data symbols only), "
              f"index only {t['pairsOnlyIndexSees']}")
        print(f"  object symbols {o['symbols']}: functions {o['functions']} ({o['functionNames']} names), "
              f"variables {o['variables']}, vtables {o['vtables']}, typeinfo {o['typeinfo']}")
        print(f"  function names: both {f['both']}, objects only {f['objectsOnly']}, index only {f['indexOnly']}")
        print("  objects only, by what the index holds: " +
              ", ".join(f"{w} {t['objectsOnlyBy'][w]}" for w in MISSED_WHY))
        for h in INDEX_HOW:
            print(f"  index only, {h}: " + ", ".join(f"{s} {t['indexOnlyBy'][h][s]}" for s in OBJECT_SAYS))
        u = t["unused"]
        print(f"  unused: {u['declaredPairs']} declared pairs, objects need nothing in {u['objectsUnused']}; "
              f"flagged {u['flagged']} (confirmed {u['flaggedConfirmed']}, functions needed "
              f"{u['flaggedButFunctionsNeeded']}, only data needed {u['flaggedButOnlyDataNeeded']}); "
              f"unused per objects but not flagged {u['objectsUnusedNotFlagged']}")
        th = t["thin"]
        print(f"  thin: flagged {th['flagged']}; objects need at most 5 symbols in {th['objectsAgreeAtMost5']} "
              f"(none in {th['objectsNeedNone']}), more in {th['objectsNeedMoreThan5']}")
        un = t["undeclared"]
        print(f"  undeclared: flagged {un['flagged']}, objects confirm {un['objectsConfirm']}")

    print(f"targets: {report['targets']['inMap']} in the map, {len(objects)} with objects, "
          f"{len(not_built)} with sources but not built")
    show("all pairs with objects", report["all"])
    show("pairs whose targets are both cleanly indexed", report["covered"])

    def table(title: str, selected: list[dict]):
        if not selected:
            return
        print(f"\n{title}")
        print("  consumer -> provider | declared | index sym+uncertain | objects fn/var/vtbl/ti | both/objOnly/idxOnly")
        for e in selected[: args.top]:
            o = e["objects"]
            print(f"  {e['consumer']} -> {e['provider']} | {e['declared']} | "
                  f"{e['index']['symbols']}+{e['index']['uncertainSymbols']} | "
                  f"{o['functions']}/{o['variables']}/{o['vtables']}/{o['typeinfo']} | "
                  f"{e['both']}/{e['objectsOnly']}/{e['indexOnly']}")

    table("thin-dependency findings against the objects",
          sorted((e for e in edges if "thin-dependency" in e["findings"]),
                 key=lambda e: (e["objects"]["symbols"], e["consumer"], e["provider"])))
    table("unused-link-dependency findings the objects contradict",
          sorted((e for e in edges if "unused-link-dependency" in e["findings"] and e["objects"]["symbols"]),
                 key=lambda e: (-e["objects"]["symbols"], e["consumer"], e["provider"])))
    table("largest disagreements",
          sorted(edges, key=lambda e: (-(e["objectsOnly"] + e["indexOnly"]), e["consumer"], e["provider"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
