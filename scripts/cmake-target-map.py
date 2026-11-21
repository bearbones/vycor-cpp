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
"""CMake adapter for the dependency audit's target map
(docs/dependency-audit.md).

Reads a CMake File API codemodel-v2 reply from a configured build
directory and writes the build-system-neutral target map that
`megascope audit-dependencies --target-map` takes: every link target,
the sources compiled into it, the headers it lists, and the targets it
links.

    scripts/cmake-target-map.py --build-dir build --output targets.json
    scripts/cmake-target-map.py --build-dir build --configure   # ask cmake first

CMake only writes the reply when a query file exists at configure time.
Without a reply this script writes the query file
(<build>/.cmake/api/v1/query/client-vycor/query.json) and stops; re-run
`cmake <build>` and call it again, or pass --configure to have it run
cmake itself.

Which dependencies it can state depends on the CMake version:

  codemodel >= 2.9 (CMake 4.2)  `linkLibraries` and `objectDependencies`
        per target: what each target's own build file names. The map says
        "depsForm": "direct", and INTERFACE libraries are listed. What a
        target only forwards to its consumers (`interfaceLinkLibraries`
        it does not link itself) goes under `interfaceDeps`.
  older                          only `dependencies`, the transitive
        build-order closure. The map says "depsForm": "closure"; the audit
        then cannot tell a direct dependency from an inherited one.

Beyond the documented schema each target carries two members the audit
ignores and scripts/dep-audit-nm-compare.py reads: `artifact` (the
linked file) and `objects` (the object files of its compiled sources,
taken from the `output` member of compile_commands.json when the build
directory has one).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

KINDS = {
    "EXECUTABLE": "executable",
    "STATIC_LIBRARY": "static_library",
    "SHARED_LIBRARY": "shared_library",
    "MODULE_LIBRARY": "module_library",
    "OBJECT_LIBRARY": "object_library",
    "INTERFACE_LIBRARY": "interface_library",
}

# Languages megascope indexes. Sources in any other language (assembly,
# resource files) are reported under `otherSources`: the index never holds
# them, so listing them as sources would mark the target as not covered.
INDEXED_LANGUAGES = {"C", "CXX", "OBJC", "OBJCXX"}

HEADER_SUFFIXES = {
    "", ".h", ".hh", ".hpp", ".hxx", ".h++", ".inc", ".inl", ".ipp", ".tcc",
    ".def", ".tpp",
}

QUERY = {"requests": [{"kind": "codemodel", "version": 2}]}


def fail(message: str, code: int = 1) -> "NoReturn":  # noqa: F821
    print(f"cmake-target-map: {message}", file=sys.stderr)
    sys.exit(code)


def write_query(build: Path) -> Path:
    query_dir = build / ".cmake" / "api" / "v1" / "query" / "client-vycor"
    query_dir.mkdir(parents=True, exist_ok=True)
    query = query_dir / "query.json"
    query.write_text(json.dumps(QUERY) + "\n")
    return query


def load_reply(build: Path):
    """The newest index file and its codemodel-v2 object, or (None, None)."""
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        return None, None
    index = json.loads(indexes[-1].read_text())
    for obj in index.get("objects", []):
        if obj.get("kind") == "codemodel" and obj.get("version", {}).get("major") == 2:
            return index, json.loads((reply / obj["jsonFile"]).read_text())
    return index, None


def resolve(path: str, base: str) -> str:
    return os.path.normpath(path if os.path.isabs(path) else os.path.join(base, path))


def object_outputs(build: Path) -> dict[tuple[str, str], str]:
    """(target name, source path) -> object file, from compile_commands.json.

    CMake's Ninja and Makefile generators put a target's objects under
    <dir>/CMakeFiles/<target>.dir/, which is how an entry is matched to
    one target when a source is compiled into several.
    """
    path = build / "compile_commands.json"
    if not path.is_file():
        return {}
    outputs: dict[tuple[str, str], str] = {}
    for entry in json.loads(path.read_text()):
        output = entry.get("output")
        if not output:
            continue
        directory = entry.get("directory", str(build))
        output = resolve(output, directory)
        source = resolve(entry["file"], directory)
        parts = Path(output).parts
        for i, part in enumerate(parts):
            if part == "CMakeFiles" and i + 1 < len(parts) and parts[i + 1].endswith(".dir"):
                outputs[(parts[i + 1][: -len(".dir")], source)] = output
                break
    return outputs


def build_map(build: Path, config: str | None) -> dict:
    index, model = load_reply(build)
    if model is None:
        return {}
    version = model.get("version", {})
    minor = (version.get("major", 2), version.get("minor", 0))
    configurations = model["configurations"]
    chosen = configurations[0]
    if config is not None:
        named = [c for c in configurations if c.get("name") == config]
        if not named:
            names = ", ".join(repr(c.get("name", "")) for c in configurations)
            fail(f"no configuration {config!r} in the reply (it has {names})", 2)
        chosen = named[0]

    source_dir = resolve(model["paths"]["source"], str(build))
    build_dir = resolve(model["paths"]["build"], str(build))
    reply = build / ".cmake" / "api" / "v1" / "reply"
    outputs = object_outputs(build)

    raw = []
    for ref in list(chosen.get("targets", [])) + list(chosen.get("abstractTargets", [])):
        raw.append(json.loads((reply / ref["jsonFile"]).read_text()))

    # A target states what it links when the codemodel carries
    # `linkLibraries` (2.9+). One target with the member is enough to know
    # the reply is of that generation: a target that links nothing omits it.
    direct = minor >= (2, 9)
    kept = {}
    for t in raw:
        # An imported target is external whatever its type (an import of
        # unknown type is UNKNOWN_LIBRARY).
        kind = "external" if t.get("imported") else KINDS.get(t.get("type", ""))
        if kind is None:  # UTILITY and anything newer CMake invents
            continue
        kept[t["id"]] = (t, kind)

    # Imported targets are scoped to a directory, so two directories may
    # each import one of the same name (and CMake 4.2+ reports both). They
    # are one target of the map; anything else sharing a name is an error.
    names: dict[str, str] = {}
    seen: dict[str, str] = {}
    for tid, (t, kind) in kept.items():
        name = t["name"]
        if name in seen and not (kind == "external" and seen[name] == "external"):
            fail(f"two targets are named {name!r}; the target map needs unique names")
        seen[name] = kind
        names[tid] = name

    def named(t: dict, members: tuple[str, ...]) -> set[str]:
        ids = {e["id"] for m in members for e in t.get(m, []) if "id" in e}
        return {names[i] for i in ids if i in names} - {t["name"]}

    by_name: dict[str, dict] = {}
    other_sources = 0
    for tid, (t, kind) in kept.items():
        groups = t.get("compileGroups", [])
        sources, headers, others = [], [], []
        for s in t.get("sources", []):
            path = resolve(s["path"], source_dir)
            group = s.get("compileGroupIndex")
            if group is not None:
                if groups[group].get("language") in INDEXED_LANGUAGES:
                    sources.append(path)
                else:
                    others.append(path)
            elif Path(path).suffix.lower() in HEADER_SUFFIXES and not s.get("isGenerated"):
                headers.append(path)
        other_sources += len(others)

        # What the target links itself, and what it only tells its
        # consumers to link (`target_link_libraries(t INTERFACE ...)`, the
        # whole of a header-only library's dependencies).
        if direct:
            deps = named(t, ("linkLibraries", "objectDependencies"))
            forwarded = named(t, ("interfaceLinkLibraries",)) - deps
        else:
            deps = named(t, ("dependencies",))
            forwarded = set()

        previous = by_name.get(t["name"])
        if previous is not None:  # a second import of the same name
            previous["deps"] = sorted(set(previous["deps"]) | deps)
            merged = (set(previous.get("interfaceDeps", [])) | forwarded) - set(previous["deps"])
            if merged:
                previous["interfaceDeps"] = sorted(merged)
            continue

        entry = {"name": t["name"], "kind": kind, "sources": sorted(set(sources))}
        if headers:
            entry["headers"] = sorted(set(headers))
        entry["deps"] = sorted(deps)
        if forwarded:
            entry["interfaceDeps"] = sorted(forwarded)
        if others:
            entry["otherSources"] = sorted(set(others))
        artifacts = [resolve(a["path"], build_dir) for a in t.get("artifacts", [])]
        if kind == "object_library":
            objects = artifacts
        else:
            objects = [outputs[(t["name"], s)] for s in entry["sources"] if (t["name"], s) in outputs]
            if artifacts:
                entry["artifact"] = artifacts[0]
        if objects:
            entry["objects"] = sorted(objects)
        by_name[t["name"]] = entry

    targets = sorted(by_name.values(), key=lambda e: e["name"])
    cmake_version = index.get("cmake", {}).get("version", {}).get("string", "?")
    result = {
        "schema": 1,
        "generator": f"cmake-file-api {cmake_version} (codemodel {minor[0]}.{minor[1]})",
        "depsForm": "direct" if direct else "closure",
        "root": source_dir,
        "roots": sorted({source_dir, build_dir}),
        "targets": targets,
    }
    summary = {
        "targets": len(targets),
        "depsForm": result["depsForm"],
        "sources": sum(len(t["sources"]) for t in targets),
        "otherSources": other_sources,
        "withObjects": sum(1 for t in targets if t.get("objects")),
    }
    return {"map": result, "summary": summary}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, type=Path, help="a configured CMake build directory")
    ap.add_argument("--output", "-o", type=Path, help="where to write the target map (default: stdout)")
    ap.add_argument("--config", help="configuration name, for multi-config generators (default: the first)")
    ap.add_argument("--configure", action="store_true",
                    help="write the File API query and run `cmake <build-dir>` when there is no reply yet")
    ap.add_argument("--cmake", default="cmake", help="cmake executable for --configure")
    args = ap.parse_args()

    build = args.build_dir.resolve()
    if not (build / "CMakeCache.txt").is_file():
        fail(f"{build} is not a configured CMake build directory (no CMakeCache.txt)", 2)

    built = build_map(build, args.config)
    if not built:
        query = write_query(build)
        if not args.configure:
            fail(f"no File API codemodel reply in {build}. Wrote the query {query}; "
                 f"re-run `cmake {build}` and call this script again (or pass --configure).", 3)
        subprocess.run([args.cmake, str(build)], check=True, stdout=sys.stderr)
        built = build_map(build, args.config)
        if not built:
            fail("cmake ran but wrote no codemodel-v2 reply (File API needs CMake 3.14 or newer)", 3)

    text = json.dumps(built["map"], indent=1) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        sys.stdout.write(text)
    print("cmake-target-map: " + json.dumps(built["summary"]), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
