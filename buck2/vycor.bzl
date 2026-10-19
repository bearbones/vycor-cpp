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

"""Buck2 rules and macros for vycor-cpp (`megascope` call-graph indexing and `check` analysis).

Architecture: Map-Reduce Call-Graph Indexing in Buck2
=====================================================
Instead of requiring a monolithic `compile_commands.json` over the whole
repository, these rules integrate `vycor-cpp` directly into Buck2's action
graph at three granularities:

1. **Per-Translation-Unit (`.vycs` per TU)**:
   Each C/C++ source file is indexed in its own cacheable, parallel Buck2
   action (`vycor-cpp megascope index --threads 1 --source <tu> --index <tu.vycs> -- <flags>`).
   Buck2 tracks the TU's source file and transitive headers as action inputs,
   so editing a file only re-indexes the affected TUs (and fetches unchanged
   TUs from Buck2's local/remote action cache).

2. **Per-Target (`.vycs` per library/binary)**:
   `vycor_target_index` merges a target's per-TU `.vycs` shards into a single
   `<target>.vycs` artifact (`vycor-cpp megascope merge --output <target.vycs> --input-list <list>`)
   and propagates a `VycorIndexTSet` (`transitive_set`) across `deps`.
   Sub-targets provide direct access to every granularity:
   - `buck2 build //pkg:lib_vycor`            -> per-target `lib_vycor.vycs`
   - `buck2 build //pkg:lib_vycor[target]`    -> per-target `lib_vycor.vycs`
   - `buck2 build //pkg:lib_vycor[tu]`        -> all per-TU `.vycs` shards of `lib`
   - `buck2 build //pkg:lib_vycor[tu/a.cpp]`  -> single per-TU `.vycs` shard for `a.cpp`
   - `buck2 build //pkg:lib_vycor[global]`    -> transitive global `.vycs` across `lib` + `deps`

3. **Global Index (`.vycs` across targets)**:
   `vycor_global_index` reduces the `VycorIndexTSet` across any set of root
   targets into a single global `.vycs` call-graph and control-flow index.
   Diamond dependencies are deduplicated both at the Buck2 `transitive_set`
   layer (for shared target shards) and inside `vycor-cpp megascope merge`
   (for shared TUs across targets).
"""

# ---------------------------------------------------------------------------
# Providers & Transitive Sets
# ---------------------------------------------------------------------------

def _project_index_path(artifact: Artifact):
    return cmd_args(artifact)

def _project_headers(headers: list):
    return cmd_args(headers)

def _project_flags(flags: list):
    return cmd_args(flags)

VycorIndexTSet = transitive_set(
    args_projections = {
        "inputs": _project_index_path,
    },
)

VycorHeadersTSet = transitive_set(
    args_projections = {
        "headers": _project_headers,
    },
)

VycorFlagsTSet = transitive_set(
    args_projections = {
        "flags": _project_flags,
    },
)

VycorIndexInfo = provider(
    doc = "Carries per-TU, per-target, and transitive .vycs call-graph artifacts and C++ header/flag metadata.",
    fields = [
        "exported_flags_tset",
        "exported_headers_tset",
        "global_index",
        "target_index",
        "target_tset",
        "tu_shards",
        "tu_tset",
    ],
)

_DepSets = record(
    target_children = list,
    tu_children = list,
    header_children = list,
    flag_children = list,
)

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _vycor_exe(ctx: AnalysisContext) -> cmd_args:
    if getattr(ctx.attrs, "vycor_bin", None) != None:
        return cmd_args(ctx.attrs.vycor_bin[RunInfo])
    exe = getattr(ctx.attrs, "vycor_executable", "vycor-cpp")
    return cmd_args(exe)

def _collect_dep_Sets(ctx: AnalysisContext, deps: list) -> _DepSets:
    target_children = []
    tu_children = []
    header_children = []
    flag_children = []
    for dep in deps:
        if VycorIndexInfo in dep:
            info = dep[VycorIndexInfo]
            if info.target_tset != None:
                target_children.append(info.target_tset)
            if info.tu_tset != None:
                tu_children.append(info.tu_tset)
            if info.exported_headers_tset != None:
                header_children.append(info.exported_headers_tset)
            if info.exported_flags_tset != None:
                flag_children.append(info.exported_flags_tset)
        elif DefaultInfo in dep and dep[DefaultInfo].default_outputs:
            # Support cross-language shard targets (e.g. pyrex .mgsc / .shard.json rules)
            for out_art in dep[DefaultInfo].default_outputs:
                node = ctx.actions.tset(VycorIndexTSet, value = out_art)
                target_children.append(node)
                tu_children.append(node)
    return _DepSets(
        target_children = target_children,
        tu_children = tu_children,
        header_children = header_children,
        flag_children = flag_children,
    )

def _index_ext(ctx: AnalysisContext) -> str:
    fmt = getattr(ctx.attrs, "index_format", "vycs")
    if fmt.startswith("."):
        fmt = fmt[1:]
    return fmt if fmt else "vycs"

def _make_list_tset(ctx: AnalysisContext, tset_type, values: list, children: list):
    vals = list(values)
    if vals and children:
        return ctx.actions.tset(tset_type, value = vals, children = children)
    elif vals:
        return ctx.actions.tset(tset_type, value = vals)
    elif children:
        return ctx.actions.tset(tset_type, children = children)
    return None

def _qualify_include_dirs(pkg: str, dirs: list) -> list[str]:
    out = []
    for inc in dirs:
        if inc == "." or inc == "":
            out.append("-I" + (pkg if pkg else "."))
        elif inc.startswith("/") or inc.startswith("-"):
            out.append(inc if inc.startswith("-") else ("-I" + inc))
        else:
            rel = (pkg + "/" + inc) if pkg else inc
            out.append("-I" + rel)
    return out

def _build_compile_flags(
        ctx: AnalysisContext,
        dep_flag_tset,
        dep_header_tset,
        own_headers: list) -> cmd_args:
    flags = cmd_args()
    if getattr(ctx.attrs, "cxx_standard", ""):
        flags.add("-std=" + ctx.attrs.cxx_standard)

    # Package-relative and repo-root include directories.
    for inc_flag in _qualify_include_dirs(ctx.label.package, getattr(ctx.attrs, "include_dirs", [])):
        flags.add(inc_flag)

    for pf in getattr(ctx.attrs, "preprocessor_flags", []):
        flags.add(pf)
    if dep_flag_tset != None:
        flags.add(dep_flag_tset.project_as_args("flags"))
    for cf in getattr(ctx.attrs, "compiler_flags", []):
        flags.add(cf)

    # Attach own headers and transitive exported headers as hidden inputs so
    # Buck2's action digest changes whenever any included header is edited.
    if own_headers:
        flags.add(cmd_args(hidden = own_headers))
    if dep_header_tset != None:
        flags.add(cmd_args(hidden = [dep_header_tset.project_as_args("headers")]))
    return flags

def _append_bake_options(cmd: cmd_args, ctx: AnalysisContext):
    for pat in getattr(ctx.attrs, "collapse_paths", []):
        cmd.add("--collapse-paths", pat)
    for lt in getattr(ctx.attrs, "lock_types", []):
        cmd.add("--lock-types", lt)
    if getattr(ctx.attrs, "channel_types_json", None) != None:
        cmd.add("--channel-types-json", ctx.attrs.channel_types_json)
    if getattr(ctx.attrs, "org_config", None) != None:
        cmd.add("--org-config", ctx.attrs.org_config)
    if getattr(ctx.attrs, "sysroot", ""):
        cmd.add("--sysroot", ctx.attrs.sysroot)
    for ea in getattr(ctx.attrs, "extra_args", []):
        cmd.add("--extra-arg=" + ea)
    for ep in getattr(ctx.attrs, "entry_points", []):
        cmd.add("--entry-point", ep)

def vycor_plugin_flags(
        plugin,
        output_vycs = None,
        output_mgsc = None,
        output_shard = None,
        collapse_paths: list = [],
        lock_types: list = [],
        entry_points: list = [],
        channel_types_json = None,
        org_config = None,
        no_wait: bool = True) -> list:
    """Returns Clang flags (`-fplugin=...` and `-Xclang -plugin-arg-vycor-megascope -Xclang ...`)
    for emitting a `.vycs`, `.mgsc`, or `.shard.json` call-graph shard during normal `clang++ -c` compilation."""
    flags = [cmd_args(plugin, format = "-fplugin={}")]
    def _arg(val):
        flags.extend(["-Xclang", "-plugin-arg-vycor-megascope", "-Xclang", val])
    if output_vycs != None:
        _arg(cmd_args(output_vycs, format = "--output={}"))
    if output_mgsc != None:
        _arg(cmd_args(output_mgsc, format = "--emit-mgsc={}"))
    if output_shard != None:
        _arg(cmd_args(output_shard, format = "--emit-shard={}"))
    if no_wait:
        _arg("--no-wait")
    for pat in collapse_paths:
        _arg("--collapse-paths=" + pat)
    for lt in lock_types:
        _arg("--lock-types=" + lt)
    for ep in entry_points:
        _arg("--entry-point=" + ep)
    if channel_types_json != None:
        _arg(cmd_args(channel_types_json, format = "--channel-types-json={}"))
    if org_config != None:
        _arg(cmd_args(org_config, format = "--org-config={}"))
    return flags

def _declare_tu_bake_action(
        ctx: AnalysisContext,
        src: Artifact,
        out_name: str,
        compile_flags: cmd_args) -> Artifact:
    tu_out = ctx.actions.declare_output(out_name)
    plugin_target = getattr(ctx.attrs, "vycor_plugin", None)
    plugin_path = getattr(ctx.attrs, "vycor_plugin_path", "")
    if plugin_target != None or plugin_path != "":
        plugin_arg = plugin_target[DefaultInfo].default_outputs[0] if plugin_target != None else plugin_path
        clang_exe = getattr(ctx.attrs, "clang_executable", "clang++")
        cmd = cmd_args(clang_exe, "-fsyntax-only")
        for pf in vycor_plugin_flags(
            plugin = plugin_arg,
            output_vycs = tu_out.as_output(),
            collapse_paths = getattr(ctx.attrs, "collapse_paths", []),
            lock_types = getattr(ctx.attrs, "lock_types", []),
            entry_points = getattr(ctx.attrs, "entry_points", []),
            channel_types_json = getattr(ctx.attrs, "channel_types_json", None),
            org_config = getattr(ctx.attrs, "org_config", None),
            no_wait = True,
        ):
            cmd.add(pf)
        if getattr(ctx.attrs, "sysroot", ""):
            cmd.add("--sysroot=" + ctx.attrs.sysroot)
        cmd.add(compile_flags)
        for ea in getattr(ctx.attrs, "extra_args", []):
            cmd.add(ea)
        cmd.add("-c", src)
    else:
        cmd = cmd_args(_vycor_exe(ctx))
        cmd.add(
            "megascope",
            "index",
            "--threads",
            "1",
            "--no-wait",
            "--index",
            tu_out.as_output(),
            "--source",
            src,
        )
        _append_bake_options(cmd, ctx)
        cmd.add("--")
        cmd.add(compile_flags)

    ctx.actions.run(
        cmd,
        category = "vycor_tu_index",
        identifier = src.short_path,
    )
    return tu_out

def _declare_merge_action(
        ctx: AnalysisContext,
        out_name: str,
        list_name: str,
        input_args,
        category: str,
        identifier: str,
        allow_mismatch: bool = False,
        entry_points: list = []) -> Artifact:
    merged_out = ctx.actions.declare_output(out_name)
    list_file = ctx.actions.write(list_name, input_args)
    cmd = cmd_args(_vycor_exe(ctx))
    cmd.add(
        "megascope",
        "merge",
        "--no-wait",
        "--output",
        merged_out.as_output(),
        "--input-list",
        list_file,
    )
    if allow_mismatch:
        cmd.add("--allow-mismatch")
    for ep in entry_points:
        cmd.add("--entry-point", ep)
    # Ensure the underlying .vycs shard artifacts are materialized before
    # megascope merge reads their paths from `list_file`.
    cmd.add(cmd_args(hidden = [input_args]))

    ctx.actions.run(
        cmd,
        category = category,
        identifier = identifier,
    )
    return merged_out

# ---------------------------------------------------------------------------
# Rule 1: vycor_tu_index (per-TU .vycs / .mgsc / .shard.json artifact)
# ---------------------------------------------------------------------------

def _vycor_tu_index_impl(ctx: AnalysisContext) -> list[Provider]:
    dep_sets = _collect_dep_Sets(ctx, ctx.attrs.deps)
    own_headers = list(ctx.attrs.headers) + list(ctx.attrs.exported_headers)
    exported_flags = (
        _qualify_include_dirs(ctx.label.package, getattr(ctx.attrs, "exported_include_dirs", [])) +
        list(ctx.attrs.exported_preprocessor_flags)
    )
    dep_header_tset = _make_list_tset(
        ctx,
        VycorHeadersTSet,
        ctx.attrs.exported_headers,
        dep_sets.header_children,
    )
    dep_flag_tset = _make_list_tset(
        ctx,
        VycorFlagsTSet,
        exported_flags,
        dep_sets.flag_children,
    )

    compile_flags = _build_compile_flags(ctx, dep_flag_tset, dep_header_tset, own_headers)
    ext = _index_ext(ctx)
    out_name = ctx.attrs.out if ctx.attrs.out else (ctx.label.name + "." + ext)
    tu_shard = _declare_tu_bake_action(ctx, ctx.attrs.src, out_name, compile_flags)

    tu_tset = ctx.actions.tset(
        VycorIndexTSet,
        value = tu_shard,
        children = dep_sets.tu_children,
    )
    target_tset = ctx.actions.tset(
        VycorIndexTSet,
        value = tu_shard,
        children = dep_sets.target_children,
    )

    return [
        DefaultInfo(default_output = tu_shard),
        VycorIndexInfo(
            tu_shards = {ctx.attrs.src.short_path: tu_shard},
            target_index = tu_shard,
            global_index = tu_shard,
            target_tset = target_tset,
            tu_tset = tu_tset,
            exported_headers_tset = dep_header_tset,
            exported_flags_tset = dep_flag_tset,
        ),
    ]

_COMMON_BAKE_ATTRS = {
    "channel_types_json": attrs.option(attrs.source(), default = None, doc = "Optional channel-types JSON config."),
    "clang_executable": attrs.string(default = "clang++", doc = "Clang++ executable when using vycor_plugin / vycor_plugin_path."),
    "collapse_paths": attrs.list(attrs.string(), default = [], doc = "Path patterns to collapse in the call graph."),
    "compiler_flags": attrs.list(attrs.string(), default = [], doc = "Target-private compiler flags."),
    "cxx_standard": attrs.string(default = "c++17", doc = "C++ standard flag (e.g. c++17, c++20)."),
    "deps": attrs.list(attrs.dep(), default = [], doc = "Dependencies providing VycorIndexInfo or shard outputs."),
    "entry_points": attrs.list(attrs.string(), default = [], doc = "Entry-point function names recorded in the index."),
    "exported_headers": attrs.list(attrs.source(), default = [], doc = "Public headers propagated to dependents."),
    "exported_include_dirs": attrs.list(attrs.string(), default = [], doc = "Package-relative public include directories propagated to dependents."),
    "exported_preprocessor_flags": attrs.list(attrs.string(), default = [], doc = "Public preprocessor/include flags propagated to dependents."),
    "extra_args": attrs.list(attrs.string(), default = [], doc = "Extra arguments forwarded via --extra-arg."),
    "headers": attrs.list(attrs.source(), default = [], doc = "Private headers of this target."),
    "include_dirs": attrs.list(attrs.string(), default = ["."], doc = "Package-relative include directories."),
    "index_format": attrs.string(default = "vycs", doc = "Output shard/snapshot format extension: 'vycs', 'mgsc', or 'shard.json'."),
    "lock_types": attrs.list(attrs.string(), default = [], doc = "Additional qualified lock type names."),
    "org_config": attrs.option(attrs.source(), default = None, doc = "Optional organization config JSON."),
    "preprocessor_flags": attrs.list(attrs.string(), default = [], doc = "Target-private preprocessor flags (-D, -I)."),
    "sysroot": attrs.string(default = "", doc = "Optional sysroot path."),
    "vycor_bin": attrs.option(attrs.exec_dep(providers = [RunInfo]), default = None, doc = "Optional hermetic vycor-cpp executable target."),
    "vycor_executable": attrs.string(default = "vycor-cpp", doc = "Path or binary name of vycor-cpp when vycor_bin is not set."),
    "vycor_plugin": attrs.option(attrs.dep(), default = None, doc = "Optional libvycor_megascope_plugin.so target for Clang plugin indexing."),
    "vycor_plugin_path": attrs.string(default = "", doc = "Optional path to libvycor_megascope_plugin.so for Clang plugin indexing."),
}

vycor_tu_index = rule(
    impl = _vycor_tu_index_impl,
    doc = "Indexes a single C/C++ translation unit into a hermetic .vycs, .mgsc, or .shard.json call-graph shard.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        out = attrs.string(default = "", doc = "Optional output filename (defaults to <name>.<index_format>)."),
        src = attrs.source(doc = "The C/C++ translation unit source file to index."),
    ),
)

# ---------------------------------------------------------------------------
# Rule 2: vycor_target_index (per-TU shards + per-target .vycs/.mgsc + [global])
# ---------------------------------------------------------------------------

def _vycor_target_index_impl(ctx: AnalysisContext) -> list[Provider]:
    dep_sets = _collect_dep_Sets(ctx, ctx.attrs.deps)
    own_headers = list(ctx.attrs.headers) + list(ctx.attrs.exported_headers)
    exported_flags = (
        _qualify_include_dirs(ctx.label.package, getattr(ctx.attrs, "exported_include_dirs", [])) +
        list(ctx.attrs.exported_preprocessor_flags)
    )

    exported_headers_tset = _make_list_tset(
        ctx,
        VycorHeadersTSet,
        ctx.attrs.exported_headers,
        dep_sets.header_children,
    )

    exported_flags_tset = _make_list_tset(
        ctx,
        VycorFlagsTSet,
        exported_flags,
        dep_sets.flag_children,
    )

    compile_flags = _build_compile_flags(
        ctx,
        exported_flags_tset,
        exported_headers_tset,
        own_headers,
    )

    ext = _index_ext(ctx)

    # 1. Per-TU .vycs / .mgsc / .shard.json actions (one cacheable action per source file).
    tu_shards = {}
    tu_shard_list = []
    sub_targets = {}
    for src in ctx.attrs.srcs:
        rel = src.short_path
        shard_out_name = "tu/" + ctx.label.name + "/" + rel + "." + ext
        shard = _declare_tu_bake_action(ctx, src, shard_out_name, compile_flags)
        tu_shards[rel] = shard
        tu_shard_list.append(shard)
        sub_targets["tu/" + rel] = [DefaultInfo(default_output = shard)]

    sub_targets["tu"] = [DefaultInfo(default_outputs = tu_shard_list)]

    # 2. Per-target .vycs / .mgsc artifact:
    #    - Header-only target (0 TUs): no per-target shard; forwards transitive
    #      headers, flags, and dep index tsets to downstream dependents.
    #    - Single-TU target: reuse the TU shard directly (entry_points were
    #      already baked into the TU shard).
    #    - Multi-TU target: merge all per-TU shards into <name>.<ext>.
    target_out_name = ctx.attrs.out if ctx.attrs.out else (ctx.label.name + "." + ext)
    if len(tu_shard_list) == 0:
        target_index = None
        tu_tset = ctx.actions.tset(VycorIndexTSet, children = dep_sets.tu_children) if dep_sets.tu_children else None
        target_tset = ctx.actions.tset(VycorIndexTSet, children = dep_sets.target_children) if dep_sets.target_children else None
    elif len(tu_shard_list) == 1:
        target_index = ctx.actions.copy_file(target_out_name, tu_shard_list[0])
        tu_nodes = [ctx.actions.tset(VycorIndexTSet, value = tu_shard_list[0])]
        tu_tset = ctx.actions.tset(
            VycorIndexTSet,
            children = tu_nodes + dep_sets.tu_children,
        )
        target_tset = ctx.actions.tset(
            VycorIndexTSet,
            value = target_index,
            children = dep_sets.target_children,
        )
    else:
        target_index = _declare_merge_action(
            ctx = ctx,
            out_name = target_out_name,
            list_name = ctx.label.name + ".tu_shards.list",
            input_args = cmd_args(tu_shard_list),
            category = "vycor_target_merge",
            identifier = ctx.label.name,
            allow_mismatch = ctx.attrs.allow_mismatch,
            entry_points = ctx.attrs.entry_points,
        )
        tu_nodes = [
            ctx.actions.tset(VycorIndexTSet, value = s)
            for s in tu_shard_list
        ]
        tu_tset = ctx.actions.tset(
            VycorIndexTSet,
            children = tu_nodes + dep_sets.tu_children,
        )
        target_tset = ctx.actions.tset(
            VycorIndexTSet,
            value = target_index,
            children = dep_sets.target_children,
        )

    if target_index != None:
        sub_targets["target"] = [DefaultInfo(default_output = target_index)]

    # 3. On-demand [global] sub-target merging this target + all transitive deps.
    global_ext = "mgsc" if ext == "shard.json" else ext
    if target_tset == None:
        global_index = None
    elif target_index != None and not dep_sets.target_children and ext != "shard.json":
        global_index = target_index
    else:
        global_index = _declare_merge_action(
            ctx = ctx,
            out_name = ctx.label.name + ".global." + global_ext,
            list_name = ctx.label.name + ".global_shards.list",
            input_args = target_tset.project_as_args("inputs"),
            category = "vycor_global_merge",
            identifier = ctx.label.name,
            allow_mismatch = ctx.attrs.allow_mismatch,
            entry_points = ctx.attrs.entry_points,
        )

    if global_index != None:
        sub_targets["global"] = [DefaultInfo(default_output = global_index)]

    return [
        DefaultInfo(
            default_outputs = [target_index] if target_index != None else [],
            sub_targets = sub_targets,
        ),
        VycorIndexInfo(
            tu_shards = tu_shards,
            target_index = target_index,
            global_index = global_index,
            target_tset = target_tset,
            tu_tset = tu_tset,
            exported_headers_tset = exported_headers_tset,
            exported_flags_tset = exported_flags_tset,
        ),
    ]

vycor_target_index = rule(
    impl = _vycor_target_index_impl,
    doc = "Produces per-TU .vycs/.mgsc/.shard.json shards, a per-target merged index, and a [global] transitive index.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        allow_mismatch = attrs.bool(default = False, doc = "Allow merging shards with differing build configs."),
        out = attrs.string(default = "", doc = "Optional output filename (defaults to <name>.<index_format>)."),
        srcs = attrs.list(attrs.source(), default = [], doc = "C/C++ source files in this target (empty for header-only libraries)."),
    ),
)

# ---------------------------------------------------------------------------
# Rule 3: vycor_global_index (global .vycs / .mgsc across target graph)
# ---------------------------------------------------------------------------

def _vycor_global_index_impl(ctx: AnalysisContext) -> list[Provider]:
    if not ctx.attrs.deps and not ctx.attrs.extra_shards:
        fail("vycor_global_index: 'deps' or 'extra_shards' must not be empty")
    dep_sets = _collect_dep_Sets(ctx, ctx.attrs.deps)
    extra_nodes = [
        ctx.actions.tset(VycorIndexTSet, value = s)
        for s in ctx.attrs.extra_shards
    ]
    granularity = ctx.attrs.merge_granularity
    if granularity not in ("target", "tu"):
        fail("vycor_global_index: merge_granularity must be 'target' or 'tu', got '" + granularity + "'")

    if granularity == "tu":
        children = dep_sets.tu_children + extra_nodes
        if not children:
            fail("vycor_global_index: no translation units or extra_shards found")
        merged_tset = ctx.actions.tset(VycorIndexTSet, children = children)
    else:
        children = dep_sets.target_children + extra_nodes
        if not children:
            fail("vycor_global_index: no indexed targets or extra_shards found")
        merged_tset = ctx.actions.tset(VycorIndexTSet, children = children)

    ext = _index_ext(ctx)
    out_name = ctx.attrs.out if ctx.attrs.out else (ctx.label.name + "." + ext)
    global_index = _declare_merge_action(
        ctx = ctx,
        out_name = out_name,
        list_name = ctx.label.name + ".inputs.list",
        input_args = merged_tset.project_as_args("inputs"),
        category = "vycor_global_merge",
        identifier = ctx.label.name,
        allow_mismatch = ctx.attrs.allow_mismatch,
        entry_points = ctx.attrs.entry_points,
    )

    return [
        DefaultInfo(default_output = global_index),
        VycorIndexInfo(
            tu_shards = {},
            target_index = global_index,
            global_index = global_index,
            target_tset = ctx.actions.tset(VycorIndexTSet, value = global_index),
            tu_tset = ctx.actions.tset(VycorIndexTSet, children = dep_sets.tu_children + extra_nodes) if (dep_sets.tu_children or extra_nodes) else None,
        ),
    ]

vycor_global_index = rule(
    impl = _vycor_global_index_impl,
    doc = "Merges transitive per-target or per-TU .vycs/.mgsc/.shard.json indexes (including cross-language shards) into a single global call-graph index.",
    attrs = {
        "allow_mismatch": attrs.bool(default = False, doc = "Allow merging shards whose build configuration or environment differs."),
        "deps": attrs.list(attrs.dep(), default = [], doc = "Root targets whose transitive .vycs/.mgsc/.shard.json indexes will be merged."),
        "entry_points": attrs.list(attrs.string(), default = [], doc = "Override entry-point functions in the global index."),
        "extra_shards": attrs.list(attrs.source(), default = [], doc = "Additional .mgsc, .vycs, .pyrx, or .shard.json shard artifacts (e.g. from pyrex Python targets) to merge."),
        "index_format": attrs.string(default = "vycs", doc = "Output snapshot format extension ('vycs' or 'mgsc') when 'out' is not set."),
        "merge_granularity": attrs.string(default = "target", doc = "'target' merges per-target artifacts; 'tu' merges leaf per-TU shards directly."),
        "out": attrs.string(default = "", doc = "Output filename (defaults to <name>.<index_format>)."),
        "vycor_bin": attrs.option(attrs.exec_dep(providers = [RunInfo]), default = None, doc = "Optional hermetic vycor-cpp executable target."),
        "vycor_executable": attrs.string(default = "vycor-cpp", doc = "Path or binary name of vycor-cpp when vycor_bin is not set."),
    },
)

# ---------------------------------------------------------------------------
# Rule 4: vycor_query (hermetic megascope query action against a .vycs/.mgsc index)
# ---------------------------------------------------------------------------

def _vycor_query_impl(ctx: AnalysisContext) -> list[Provider]:
    index_artifact = ctx.attrs.index[VycorIndexInfo].global_index if VycorIndexInfo in ctx.attrs.index else (
        ctx.attrs.index[DefaultInfo].default_outputs[0] if ctx.attrs.index[DefaultInfo].default_outputs else None
    )
    if index_artifact == None:
        fail("vycor_query: 'index' target does not produce a .vycs or .mgsc index artifact")
    ext = "ndjson" if ctx.attrs.format == "ndjson" else ("tsv" if ctx.attrs.format == "tsv" else "json")
    out_name = ctx.attrs.out if ctx.attrs.out else (ctx.label.name + "." + ext)
    report_out = ctx.actions.declare_output(out_name)

    inner = cmd_args(_vycor_exe(ctx))
    inner.add("megascope", ctx.attrs.tool, "--index", index_artifact, "--format", ctx.attrs.format)
    if ctx.attrs.pretty and ctx.attrs.format == "json":
        inner.add("--pretty")
    if ctx.attrs.args_json:
        inner.add("--args", ctx.attrs.args_json)
    for ep in ctx.attrs.entry_points:
        inner.add("--entry-point", ep)
    for flag in ctx.attrs.tool_flags:
        inner.add(flag)

    # Redirect stdout payload to report_out; allow exit code 0 (results) or
    # optionally exit code 1 (empty result set, e.g. 0 dead functions).
    allow_empty = "1" if ctx.attrs.allow_empty else "0"
    script = (
        'set -e; out="$1"; allow_empty="$2"; shift 2; ' +
        '"$@" > "$out" || { rc=$?; if [ "$rc" -eq 1 ] && [ "$allow_empty" = "1" ]; then exit 0; fi; exit "$rc"; }'
    )
    cmd = cmd_args(["/bin/sh", "-c", script, "--", report_out.as_output(), allow_empty])
    cmd.add(inner)

    ctx.actions.run(
        cmd,
        category = "vycor_query",
        identifier = ctx.label.name,
    )
    return [DefaultInfo(default_output = report_out)]

vycor_query = rule(
    impl = _vycor_query_impl,
    doc = "Runs a `vycor-cpp megascope <tool>` query against a .vycs or .mgsc index and writes the JSON/NDJSON/TSV output artifact.",
    attrs = {
        "allow_empty": attrs.bool(default = True, doc = "Treat exit code 1 (empty results, e.g. zero dead-code findings) as success."),
        "args_json": attrs.string(default = "", doc = "Optional JSON object passed via --args."),
        "entry_points": attrs.list(attrs.string(), default = [], doc = "Optional --entry-point overrides."),
        "format": attrs.string(default = "json", doc = "Output format: json, ndjson, or tsv."),
        "index": attrs.dep(doc = "Target providing VycorIndexInfo or a .vycs/.mgsc default output."),
        "out": attrs.string(default = "", doc = "Optional output filename."),
        "pretty": attrs.bool(default = True, doc = "Indent JSON output."),
        "tool": attrs.string(doc = "Megascope verb or tool name (e.g. 'graph-summary', 'analyze-dead-code', 'info', 'dump')."),
        "tool_flags": attrs.list(attrs.string(), default = [], doc = "Additional flags passed to the tool (e.g. ['--name', 'Foo::bar'])."),
        "vycor_bin": attrs.option(attrs.exec_dep(providers = [RunInfo]), default = None, doc = "Optional hermetic vycor-cpp executable target."),
        "vycor_executable": attrs.string(default = "vycor-cpp", doc = "Path or binary name of vycor-cpp when vycor_bin is not set."),
    },
)

# ---------------------------------------------------------------------------
# Rule 5: vycor_check (hermetic check static analysis report)
# ---------------------------------------------------------------------------

def _vycor_check_impl(ctx: AnalysisContext) -> list[Provider]:
    dep_sets = _collect_dep_Sets(ctx, ctx.attrs.deps)
    own_headers = list(ctx.attrs.headers) + list(ctx.attrs.exported_headers)
    exported_flags = (
        _qualify_include_dirs(ctx.label.package, getattr(ctx.attrs, "exported_include_dirs", [])) +
        list(ctx.attrs.exported_preprocessor_flags)
    )
    dep_header_tset = _make_list_tset(
        ctx,
        VycorHeadersTSet,
        ctx.attrs.exported_headers,
        dep_sets.header_children,
    )
    dep_flag_tset = _make_list_tset(
        ctx,
        VycorFlagsTSet,
        exported_flags,
        dep_sets.flag_children,
    )

    compile_flags = _build_compile_flags(ctx, dep_flag_tset, dep_header_tset, own_headers)

    ext = "sarif" if ctx.attrs.format == "sarif" else ("json" if ctx.attrs.format == "json" else "txt")
    out_name = ctx.attrs.out if ctx.attrs.out else (ctx.label.name + "." + ext)
    report_out = ctx.actions.declare_output(out_name)
    src_list = ctx.actions.write(ctx.label.name + ".sources.list", ctx.attrs.srcs)

    cmd = cmd_args(_vycor_exe(ctx))
    cmd.add(
        "check",
        "--threads",
        str(ctx.attrs.threads),
        "--format",
        ctx.attrs.format,
        "--fail-on",
        ctx.attrs.fail_on,
        "--output",
        report_out.as_output(),
        "--source-list",
        src_list,
    )
    if ctx.attrs.checks:
        cmd.add("--checks=" + ctx.attrs.checks)
    if ctx.attrs.baseline != None:
        cmd.add("--baseline", ctx.attrs.baseline)
    if ctx.attrs.org_config != None:
        cmd.add("--org-config", ctx.attrs.org_config)
    for ep in ctx.attrs.entry_points:
        cmd.add("--entry-point", ep)
    for ea in ctx.attrs.extra_args:
        cmd.add("--extra-arg=" + ea)
    cmd.add(cmd_args(hidden = ctx.attrs.srcs))
    cmd.add("--")
    cmd.add(compile_flags)

    ctx.actions.run(
        cmd,
        category = "vycor_check",
        identifier = ctx.label.name,
    )
    return [DefaultInfo(default_output = report_out)]

vycor_check = rule(
    impl = _vycor_check_impl,
    doc = "Runs `vycor-cpp check` over a target's sources and produces a SARIF, JSON, or text report artifact.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        baseline = attrs.option(attrs.source(), default = None, doc = "Optional check baseline file."),
        checks = attrs.string(default = "", doc = "Comma-separated check specification passed to --checks."),
        fail_on = attrs.string(default = "none", doc = "Severity threshold that fails the build action: note, warning, error, or none."),
        format = attrs.string(default = "sarif", doc = "Report format: sarif, json, or text."),
        out = attrs.string(default = "", doc = "Optional output report filename."),
        srcs = attrs.list(attrs.source(), doc = "C/C++ source files to analyze."),
        threads = attrs.int(default = 1, doc = "Thread count for the analysis action."),
    ),
)

# ---------------------------------------------------------------------------
# Drop-in Wrapper Macros: vycor_cxx_library and vycor_cxx_binary
# ---------------------------------------------------------------------------

def _to_vycor_dep(d: str) -> str:
    if d.endswith("_vycor"):
        return d
    if ":" not in d and ("//" in d or "@" in d):
        last = d.rsplit("/", 1)[-1]
        return d + ":" + last + "_vycor"
    return d + "_vycor"

def vycor_cxx_library(
        name: str,
        srcs: list = [],
        headers: list = [],
        exported_headers: list = [],
        deps: list = [],
        include_dirs: list = ["."],
        exported_include_dirs: list = [],
        compiler_flags: list = [],
        preprocessor_flags: list = [],
        exported_preprocessor_flags: list = [],
        cxx_standard: str = "c++17",
        vycor_bin = None,
        vycor_executable: str = "vycor-cpp",
        vycor_plugin = None,
        vycor_plugin_path: str = "",
        clang_executable: str = "clang++",
        inject_plugin_into_cxx: bool = False,
        collapse_paths: list = [],
        lock_types: list = [],
        enable_check: bool = True,
        checks: str = "",
        check_fail_on: str = "none",
        index_format: str = "vycs",
        native_cxx_library = None,
        **kwargs):
    """Defines a C++ library alongside its per-TU and per-target megascope `.vycs`/`.mgsc` index targets.

    Supports both standalone `vycor-cpp megascope index` actions and Clang
    frontend plugin (`vycor_plugin` / `vycor_plugin_path`) per-TU `.vycs`/`.mgsc`/`.shard.json` emission.

    Generated targets:
      - `:<name>` (when `native_cxx_library` is provided or `native.cxx_library` exists)
      - `:<name>_vycor`: `vycor_target_index` producing `<name>_vycor.<ext>`, plus
        sub-targets `[tu]`, `[tu/<src>]`, `[target]`, and `[global]`.
      - `:<name>_check`: `vycor_check` producing `<name>_check.sarif`.
    """
    cxx_rule = native_cxx_library if native_cxx_library != None else getattr(native, "cxx_library", None)
    if cxx_rule != None:
        cxx_flags = list(compiler_flags)
        if inject_plugin_into_cxx and (vycor_plugin != None or vycor_plugin_path != ""):
            cxx_flags = cxx_flags + vycor_plugin_flags(
                plugin = vycor_plugin if vycor_plugin != None else vycor_plugin_path,
                collapse_paths = collapse_paths,
                lock_types = lock_types,
            )
        cxx_rule(
            name = name,
            srcs = srcs,
            headers = headers,
            exported_headers = exported_headers,
            deps = deps,
            compiler_flags = cxx_flags,
            preprocessor_flags = preprocessor_flags,
            exported_preprocessor_flags = exported_preprocessor_flags,
            **kwargs
        )

    vycor_deps = [
        _to_vycor_dep(d)
        for d in deps
    ]

    ext = _index_ext(index_format)
    merged_ext = ".mgsc" if ext == ".shard.json" else ext

    vycor_target_index(
        name = name + "_vycor",
        srcs = srcs,
        headers = headers,
        exported_headers = exported_headers,
        deps = vycor_deps,
        include_dirs = include_dirs,
        exported_include_dirs = exported_include_dirs,
        compiler_flags = compiler_flags,
        preprocessor_flags = preprocessor_flags,
        exported_preprocessor_flags = exported_preprocessor_flags,
        cxx_standard = cxx_standard,
        collapse_paths = collapse_paths,
        lock_types = lock_types,
        vycor_bin = vycor_bin,
        vycor_executable = vycor_executable,
        vycor_plugin = vycor_plugin,
        vycor_plugin_path = vycor_plugin_path,
        clang_executable = clang_executable,
        index_format = index_format,
        out = name + merged_ext,
    )

    if enable_check and srcs:
        vycor_check(
            name = name + "_check",
            srcs = srcs,
            headers = headers,
            exported_headers = exported_headers,
            deps = vycor_deps,
            include_dirs = include_dirs,
            exported_include_dirs = exported_include_dirs,
            compiler_flags = compiler_flags,
            preprocessor_flags = preprocessor_flags,
            exported_preprocessor_flags = exported_preprocessor_flags,
            cxx_standard = cxx_standard,
            checks = checks,
            fail_on = check_fail_on,
            vycor_bin = vycor_bin,
            vycor_executable = vycor_executable,
        )

def vycor_cxx_binary(
        name: str,
        srcs: list = [],
        headers: list = [],
        deps: list = [],
        include_dirs: list = ["."],
        compiler_flags: list = [],
        preprocessor_flags: list = [],
        cxx_standard: str = "c++17",
        entry_points: list = ["main"],
        vycor_bin = None,
        vycor_executable: str = "vycor-cpp",
        vycor_plugin = None,
        vycor_plugin_path: str = "",
        clang_executable: str = "clang++",
        inject_plugin_into_cxx: bool = False,
        collapse_paths: list = [],
        lock_types: list = [],
        enable_check: bool = True,
        checks: str = "",
        check_fail_on: str = "none",
        index_format: str = "vycs",
        native_cxx_binary = None,
        **kwargs):
    """Defines a C++ binary alongside its per-TU, per-target, and global megascope `.vycs`/`.mgsc` index targets.

    Supports both standalone `vycor-cpp megascope index` actions and Clang
    frontend plugin (`vycor_plugin` / `vycor_plugin_path`) per-TU `.vycs`/`.mgsc`/`.shard.json` emission.

    Generated targets:
      - `:<name>` (when `native_cxx_binary` is provided or `native.cxx_binary` exists)
      - `:<name>_vycor`: `vycor_target_index` for this binary's own TUs (with `[tu]`, `[target]`, `[global]`).
      - `:<name>_vycor_global`: `vycor_global_index` merging this binary and all transitive `deps` into `<name>.global.<ext>`.
      - `:<name>_check`: `vycor_check` producing `<name>_check.sarif`.
    """
    cxx_rule = native_cxx_binary if native_cxx_binary != None else getattr(native, "cxx_binary", None)
    if cxx_rule != None:
        cxx_flags = list(compiler_flags)
        if inject_plugin_into_cxx and (vycor_plugin != None or vycor_plugin_path != ""):
            cxx_flags = cxx_flags + vycor_plugin_flags(
                plugin = vycor_plugin if vycor_plugin != None else vycor_plugin_path,
                collapse_paths = collapse_paths,
                lock_types = lock_types,
                entry_points = entry_points,
            )
        cxx_rule(
            name = name,
            srcs = srcs,
            headers = headers,
            deps = deps,
            compiler_flags = cxx_flags,
            preprocessor_flags = preprocessor_flags,
            **kwargs
        )

    vycor_deps = [
        _to_vycor_dep(d)
        for d in deps
    ]

    ext = _index_ext(index_format)
    merged_ext = ".mgsc" if ext == ".shard.json" else ext

    vycor_target_index(
        name = name + "_vycor",
        srcs = srcs,
        headers = headers,
        deps = vycor_deps,
        include_dirs = include_dirs,
        compiler_flags = compiler_flags,
        preprocessor_flags = preprocessor_flags,
        cxx_standard = cxx_standard,
        entry_points = entry_points,
        collapse_paths = collapse_paths,
        lock_types = lock_types,
        vycor_bin = vycor_bin,
        vycor_executable = vycor_executable,
        vycor_plugin = vycor_plugin,
        vycor_plugin_path = vycor_plugin_path,
        clang_executable = clang_executable,
        index_format = index_format,
        out = name + ".target" + merged_ext,
    )

    vycor_global_index(
        name = name + "_vycor_global",
        deps = [":" + name + "_vycor"],
        entry_points = entry_points,
        vycor_bin = vycor_bin,
        vycor_executable = vycor_executable,
        index_format = "mgsc" if ext == ".shard.json" else index_format,
        out = name + ".global" + merged_ext,
    )

    if enable_check and srcs:
        vycor_check(
            name = name + "_check",
            srcs = srcs,
            headers = headers,
            deps = vycor_deps,
            include_dirs = include_dirs,
            compiler_flags = compiler_flags,
            preprocessor_flags = preprocessor_flags,
            cxx_standard = cxx_standard,
            entry_points = entry_points,
            checks = checks,
            fail_on = check_fail_on,
            vycor_bin = vycor_bin,
            vycor_executable = vycor_executable,
        )
