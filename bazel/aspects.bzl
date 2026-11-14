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

"""Zero-config Bazel Aspects for vycor-cpp (`megascope` indexing & `check` analysis).

Bazel counterpart to `buck2/vycor.bxl`: attaches directly to unmodified
`cc_library`, `cc_binary`, and `cc_test` targets without editing `BUILD` files!

Examples:
  # Build per-TU shards, per-target indexes, and a merged global .vycs index
  # across any cc_binary / cc_library target graph:
  bazel build //:app \\
    --aspects=@vycor//bazel:aspects.bzl%vycor_index_aspect \\
    --output_groups=global

  # Emit language-agnostic megascope (.mgsc) snapshots:
  bazel build //:app \\
    --aspects=@vycor//bazel:aspects.bzl%vycor_index_aspect \\
    --@vycor//bazel:index_format=mgsc \\
    --output_groups=global

  # Run vycor-cpp check across all C/C++ targets in a package or repository:
  bazel build //... \\
    --aspects=@vycor//bazel:aspects.bzl%vycor_check_aspect \\
    --output_groups=report
"""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(":settings.bzl", "VycorStringFlagInfo", "VycorStringListFlagInfo")
load(":vycor.bzl", "VycorIndexInfo")

_CXX_SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".c++", ".C", ".m", ".mm")
_HEADER_EXTENSIONS = (".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp")

def _is_cxx_source(f) -> bool:
    p = f.short_path
    for ext in _CXX_SOURCE_EXTENSIONS:
        if p.endswith(ext):
            return True
    return False

def _is_header_file(f) -> bool:
    p = f.short_path
    for ext in _HEADER_EXTENSIONS:
        if p.endswith(ext):
            return True
    return False

def _normalize_ext(fmt: str) -> str:
    if fmt.startswith("."):
        fmt = fmt[1:]
    return fmt if fmt else "vycs"

def _workspace_prefix(label) -> str:
    ws = label.workspace_root
    pkg = label.package
    if ws and pkg:
        return ws + "/" + pkg
    elif ws:
        return ws
    return pkg

def _extract_rule_files(rule_attrs, attr_name: str) -> list:
    if not hasattr(rule_attrs, attr_name):
        return []
    targets = getattr(rule_attrs, attr_name)
    if type(targets) != "list":
        return []
    files = []
    for t in targets:
        if DefaultInfo in t:
            files.extend(t[DefaultInfo].files.to_list())
    return files

def _extract_rule_strings(rule_attrs, attr_name: str) -> list:
    if not hasattr(rule_attrs, attr_name):
        return []
    val = getattr(rule_attrs, attr_name)
    if type(val) != "list":
        return []
    return [str(x) for x in val if type(x) == "string"]

def _collect_aspect_dep_infos(rule_attrs) -> list:
    infos = []
    for attr_name in ("deps", "implementation_deps"):
        if hasattr(rule_attrs, attr_name):
            val = getattr(rule_attrs, attr_name)
            if type(val) == "list":
                for dep in val:
                    if VycorIndexInfo in dep:
                        infos.append(dep[VycorIndexInfo])
    return infos

def _collect_aspect_cc_contexts(target, rule_attrs) -> struct:
    header_depsets = []
    flags = []

    if CcInfo in target:
        cc_ctx = target[CcInfo].compilation_context
        header_depsets.append(cc_ctx.headers)
        for d in cc_ctx.defines.to_list():
            flags.append("-D" + d)
        for ld in cc_ctx.local_defines.to_list():
            flags.append("-D" + ld)
        for inc in cc_ctx.includes.to_list():
            flags.append("-I" + (inc if inc else "."))
        for qinc in cc_ctx.quote_includes.to_list():
            flags.append("-iquote" + (qinc if qinc else "."))
        for sinc in cc_ctx.system_includes.to_list():
            flags.append("-isystem" + (sinc if sinc else "."))
        for finc in cc_ctx.framework_includes.to_list():
            flags.append("-F" + finc)
    else:
        # cc_binary / cc_test do not always advertise CcInfo themselves; pull
        # compilation_context from their deps and implementation_deps.
        for attr_name in ("deps", "implementation_deps"):
            if hasattr(rule_attrs, attr_name):
                val = getattr(rule_attrs, attr_name)
                if type(val) == "list":
                    for dep in val:
                        if CcInfo in dep:
                            dep_cc = dep[CcInfo].compilation_context
                            header_depsets.append(dep_cc.headers)
                            for d in dep_cc.defines.to_list():
                                flags.append("-D" + d)
                            for inc in dep_cc.includes.to_list():
                                flags.append("-I" + (inc if inc else "."))
                            for qinc in dep_cc.quote_includes.to_list():
                                flags.append("-iquote" + (qinc if qinc else "."))
                            for sinc in dep_cc.system_includes.to_list():
                                flags.append("-isystem" + (sinc if sinc else "."))
                            for finc in dep_cc.framework_includes.to_list():
                                flags.append("-F" + finc)

    return struct(
        header_depsets = header_depsets,
        cc_flags = flags,
    )

def _build_target_compile_flags(target, ctx) -> struct:
    rule_attrs = ctx.rule.attr
    raw_srcs = _extract_rule_files(rule_attrs, "srcs")
    cxx_srcs = [f for f in raw_srcs if _is_cxx_source(f)]
    priv_hdrs = [f for f in raw_srcs if _is_header_file(f)]
    pub_hdrs = _extract_rule_files(rule_attrs, "hdrs")
    textual_hdrs = _extract_rule_files(rule_attrs, "textual_hdrs")

    cc_data = _collect_aspect_cc_contexts(target, rule_attrs)
    all_headers_depset = depset(
        direct = priv_hdrs + pub_hdrs + textual_hdrs,
        transitive = cc_data.header_depsets,
    )

    copts = _extract_rule_strings(rule_attrs, "copts")
    has_std_flag = any([c.startswith("-std=") for c in copts])

    flags = []
    cxx_std = ctx.attr._cxx_standard[VycorStringFlagInfo].value
    if cxx_std and not has_std_flag:
        flags.append("-std=" + cxx_std)

    base = _workspace_prefix(target.label)
    bin_root = ctx.bin_dir.path
    flags.append("-I.")
    flags.append("-I" + bin_root)
    if base:
        flags.append("-I" + base)
        flags.append("-I" + bin_root + "/" + base)

    for inc in _extract_rule_strings(rule_attrs, "includes"):
        if inc == "." or inc == "":
            continue
        rel = (base + "/" + inc) if base else inc
        flags.append("-I" + rel)
        flags.append("-I" + bin_root + "/" + rel)

    for d in _extract_rule_strings(rule_attrs, "defines"):
        flags.append("-D" + d)
    for ld in _extract_rule_strings(rule_attrs, "local_defines"):
        flags.append("-D" + ld)

    flags.extend(cc_data.cc_flags)
    flags.extend(copts)

    # Deduplicate flags while preserving order.
    dedup_flags = []
    seen = {}
    for f in flags:
        if f not in seen:
            seen[f] = True
            dedup_flags.append(f)

    return struct(
        cxx_srcs = cxx_srcs,
        headers_depset = all_headers_depset,
        compile_flags = dedup_flags,
    )

def _resolve_aspect_vycor_tool(ctx) -> struct:
    bin_target = ctx.attr._vycor_bin
    if bin_target != None:
        if bin_target[DefaultInfo].files_to_run.executable != None:
            return struct(
                files_to_run = bin_target[DefaultInfo].files_to_run,
                file = None,
                exe_str = bin_target[DefaultInfo].files_to_run.executable.path,
            )
        files = bin_target[DefaultInfo].files.to_list()
        if files:
            return struct(
                files_to_run = None,
                file = files[0],
                exe_str = files[0].path,
            )
    return struct(
        files_to_run = None,
        file = None,
        exe_str = ctx.attr._vycor_executable[VycorStringFlagInfo].value,
    )

def _resolve_aspect_plugin(ctx) -> struct:
    plugin_target = ctx.attr._vycor_plugin
    if plugin_target != None:
        files = plugin_target[DefaultInfo].files.to_list()
        if files:
            return struct(file = files[0], path = files[0].path)
    return struct(
        file = None,
        path = ctx.attr._vycor_plugin_path[VycorStringFlagInfo].value,
    )

def _run_aspect_vycor(ctx, tool_info, arg_builder, direct_inputs: list, transitive_depsets: list, outputs: list, mnemonic: str, progress_message: str):
    if tool_info.files_to_run != None:
        ctx.actions.run(
            executable = tool_info.files_to_run,
            arguments = [arg_builder],
            inputs = depset(direct = direct_inputs, transitive = transitive_depsets),
            outputs = outputs,
            mnemonic = mnemonic,
            progress_message = progress_message,
            use_default_shell_env = True,
        )
    else:
        inputs = list(direct_inputs)
        if tool_info.file != None:
            inputs.append(tool_info.file)
        shell_args = ctx.actions.args()
        shell_args.add(tool_info.exe_str)
        ctx.actions.run_shell(
            command = 'exec "$@"',
            arguments = [shell_args, arg_builder],
            inputs = depset(direct = inputs, transitive = transitive_depsets),
            outputs = outputs,
            mnemonic = mnemonic,
            progress_message = progress_message,
            use_default_shell_env = True,
        )

def _aspect_merge(ctx, tool_info, out_name: str, list_name: str, shards_depset, entry_points: list, mnemonic: str):
    merged_out = ctx.actions.declare_file(out_name)
    list_file = ctx.actions.declare_file(list_name)

    list_args = ctx.actions.args()
    list_args.add_all(shards_depset)
    ctx.actions.write(output = list_file, content = list_args)

    cmd = ctx.actions.args()
    cmd.add("megascope")
    cmd.add("merge")
    cmd.add("--no-wait")
    cmd.add("--output")
    cmd.add(merged_out.path)
    cmd.add("--input-list")
    cmd.add(list_file.path)
    for ep in entry_points:
        cmd.add("--entry-point")
        cmd.add(ep)

    _run_aspect_vycor(
        ctx = ctx,
        tool_info = tool_info,
        arg_builder = cmd,
        direct_inputs = [list_file],
        transitive_depsets = [shards_depset],
        outputs = [merged_out],
        mnemonic = mnemonic,
        progress_message = "Merging vycor aspect index " + merged_out.short_path,
    )
    return merged_out

# ---------------------------------------------------------------------------
# Aspect 1: vycor_index_aspect
# ---------------------------------------------------------------------------

def _vycor_index_aspect_impl(target, ctx):
    # If the target already produces VycorIndexInfo (e.g. explicit vycor_target_index),
    # pass it through without re-indexing.
    if VycorIndexInfo in target:
        return []

    dep_infos = _collect_aspect_dep_infos(ctx.rule.attr)
    dep_target_depsets = [i.target_depset for i in dep_infos]
    dep_tu_depsets = [i.tu_depset for i in dep_infos]
    has_dep_targets = any([i.has_targets for i in dep_infos])
    has_dep_tus = any([i.has_tus for i in dep_infos])

    parsed = _build_target_compile_flags(target, ctx)
    cxx_srcs = parsed.cxx_srcs
    if not cxx_srcs and not dep_infos:
        return []

    tool_info = _resolve_aspect_vycor_tool(ctx)
    plugin_info = _resolve_aspect_plugin(ctx)
    clang_exe = ctx.attr._clang_executable[VycorStringFlagInfo].value
    ext = _normalize_ext(ctx.attr._index_format[VycorStringFlagInfo].value)
    merged_ext = "mgsc" if ext == "shard.json" else ext
    collapse_paths = ctx.attr._collapse_paths[VycorStringListFlagInfo].values
    lock_types = ctx.attr._lock_types[VycorStringListFlagInfo].values
    entry_points = ctx.attr._entry_points[VycorStringListFlagInfo].values

    tu_shards = {}
    tu_shard_list = []
    for src in cxx_srcs:
        rel = src.short_path
        tu_out = ctx.actions.declare_file(
            "vycor_aspect/tu/" + target.label.name + "/" + rel + "." + ext,
        )
        if plugin_info.path:
            cmd = ctx.actions.args()
            cmd.add(clang_exe)
            cmd.add("-fsyntax-only")
            cmd.add("-fplugin=" + plugin_info.path)

            def _parg(val):
                cmd.add("-Xclang")
                cmd.add("-plugin-arg-vycor-megascope")
                cmd.add("-Xclang")
                cmd.add(val)

            if ext == "mgsc":
                _parg("--emit-mgsc=" + tu_out.path)
            elif ext == "shard.json":
                _parg("--emit-shard=" + tu_out.path)
            else:
                _parg("--output=" + tu_out.path)
            _parg("--no-wait")
            for pat in collapse_paths:
                _parg("--collapse-paths=" + pat)
            for lt in lock_types:
                _parg("--lock-types=" + lt)
            for ep in entry_points:
                _parg("--entry-point=" + ep)
            cmd.add_all(parsed.compile_flags)
            cmd.add(src.path)
            plugin_inputs = [src] + ([plugin_info.file] if plugin_info.file != None else [])
            ctx.actions.run_shell(
                command = 'exec "$@"',
                arguments = [cmd],
                inputs = depset(direct = plugin_inputs, transitive = [parsed.headers_depset]),
                outputs = [tu_out],
                mnemonic = "VycorAspectTuPluginIndex",
                progress_message = "Indexing C++ TU (aspect plugin) " + rel,
                use_default_shell_env = True,
            )
        else:
            cmd = ctx.actions.args()
            cmd.add("megascope")
            cmd.add("index")
            cmd.add("--threads")
            cmd.add("1")
            cmd.add("--no-wait")
            cmd.add("--index")
            cmd.add(tu_out.path)
            cmd.add("--source")
            cmd.add(src.path)
            for pat in collapse_paths:
                cmd.add("--collapse-paths")
                cmd.add(pat)
            for lt in lock_types:
                cmd.add("--lock-types")
                cmd.add(lt)
            for ep in entry_points:
                cmd.add("--entry-point")
                cmd.add(ep)
            cmd.add("--")
            cmd.add_all(parsed.compile_flags)
            _run_aspect_vycor(
                ctx = ctx,
                tool_info = tool_info,
                arg_builder = cmd,
                direct_inputs = [src],
                transitive_depsets = [parsed.headers_depset],
                outputs = [tu_out],
                mnemonic = "VycorAspectTuIndex",
                progress_message = "Indexing C++ TU (aspect) " + rel,
            )
        tu_shards[rel] = tu_out
        tu_shard_list.append(tu_out)

    # Per-target index
    target_out_prefix = "vycor_aspect/targets/" + target.label.name
    if len(tu_shard_list) == 0:
        target_index = None
        tu_depset = depset(transitive = dep_tu_depsets)
        target_depset = depset(transitive = dep_target_depsets)
        has_targets = has_dep_targets
        has_tus = has_dep_tus
    elif len(tu_shard_list) == 1 and ext == merged_ext:
        target_index = ctx.actions.declare_file(target_out_prefix + "." + merged_ext)
        ctx.actions.run_shell(
            command = 'cp -f "$1" "$2"',
            arguments = [tu_shard_list[0].path, target_index.path],
            inputs = depset([tu_shard_list[0]]),
            outputs = [target_index],
            mnemonic = "VycorAspectCopyIndex",
            progress_message = "Copying aspect target index " + target_index.short_path,
            use_default_shell_env = False,
        )
        tu_depset = depset(direct = tu_shard_list, transitive = dep_tu_depsets)
        target_depset = depset(direct = [target_index], transitive = dep_target_depsets)
        has_targets = True
        has_tus = True
    else:
        target_index = _aspect_merge(
            ctx = ctx,
            tool_info = tool_info,
            out_name = target_out_prefix + "." + merged_ext,
            list_name = target_out_prefix + ".tu.list",
            shards_depset = depset(tu_shard_list),
            entry_points = entry_points,
            mnemonic = "VycorAspectTargetMerge",
        )
        tu_depset = depset(direct = tu_shard_list, transitive = dep_tu_depsets)
        target_depset = depset(direct = [target_index], transitive = dep_target_depsets)
        has_targets = True
        has_tus = True

    if target_index == None and not has_dep_targets:
        global_index = None
    elif target_index != None and not has_dep_targets:
        global_index = target_index
    else:
        global_prefix = "vycor_aspect/global/" + target.label.name
        global_index = _aspect_merge(
            ctx = ctx,
            tool_info = tool_info,
            out_name = global_prefix + ".global." + merged_ext,
            list_name = global_prefix + ".targets.list",
            shards_depset = target_depset,
            entry_points = entry_points,
            mnemonic = "VycorAspectGlobalMerge",
        )

    output_groups = {
        "tu": depset(tu_shard_list),
        "tu_all": tu_depset,
        "target_all": target_depset,
    }
    if target_index != None:
        output_groups["target"] = depset([target_index])
    if global_index != None:
        output_groups["global"] = depset([global_index])

    return [
        OutputGroupInfo(**output_groups),
        VycorIndexInfo(
            tu_shards = tu_shards,
            target_index = target_index,
            global_index = global_index,
            target_depset = target_depset,
            tu_depset = tu_depset,
            has_targets = has_targets,
            has_tus = has_tus,
            headers_depset = parsed.headers_depset,
            flags_depset = depset(parsed.compile_flags),
        ),
    ]

vycor_index_aspect = aspect(
    implementation = _vycor_index_aspect_impl,
    attr_aspects = ["deps", "implementation_deps"],
    doc = "Builds per-TU shards (`tu`, `tu_all`), per-target indexes (`target`, `target_all`), and a transitive merged index (`global`) over existing C/C++ targets.",
    attrs = {
        "_clang_executable": attr.label(default = Label("//bazel:clang_executable")),
        "_collapse_paths": attr.label(default = Label("//bazel:collapse_paths")),
        "_cxx_standard": attr.label(default = Label("//bazel:cxx_standard")),
        "_entry_points": attr.label(default = Label("//bazel:entry_points")),
        "_index_format": attr.label(default = Label("//bazel:index_format")),
        "_lock_types": attr.label(default = Label("//bazel:lock_types")),
        "_vycor_bin": attr.label(cfg = "exec", default = Label("//bazel:vycor_bin")),
        "_vycor_executable": attr.label(default = Label("//bazel:vycor_executable")),
        "_vycor_plugin": attr.label(cfg = "exec", default = Label("//bazel:vycor_plugin")),
        "_vycor_plugin_path": attr.label(default = Label("//bazel:vycor_plugin_path")),
    },
)

# ---------------------------------------------------------------------------
# Aspect 2: vycor_check_aspect
# ---------------------------------------------------------------------------

def _vycor_check_aspect_impl(target, ctx):
    parsed = _build_target_compile_flags(target, ctx)
    cxx_srcs = parsed.cxx_srcs
    if not cxx_srcs:
        return []

    tool_info = _resolve_aspect_vycor_tool(ctx)
    fmt = ctx.attr._check_format[VycorStringFlagInfo].value
    checks = ctx.attr._checks[VycorStringFlagInfo].value
    fail_on = ctx.attr._fail_on[VycorStringFlagInfo].value
    entry_points = ctx.attr._entry_points[VycorStringListFlagInfo].values

    ext = "sarif" if fmt == "sarif" else ("json" if fmt == "json" else "txt")
    prefix = "vycor_aspect/check/" + target.label.name
    report_out = ctx.actions.declare_file(prefix + ".vycor_check." + ext)
    src_list = ctx.actions.declare_file(prefix + ".sources.list")

    src_list_args = ctx.actions.args()
    src_list_args.add_all(cxx_srcs)
    ctx.actions.write(output = src_list, content = src_list_args)

    cmd = ctx.actions.args()
    cmd.add("check")
    cmd.add("--threads")
    cmd.add("1")
    cmd.add("--format")
    cmd.add(fmt)
    cmd.add("--fail-on")
    cmd.add(fail_on)
    cmd.add("--output")
    cmd.add(report_out.path)
    cmd.add("--source-list")
    cmd.add(src_list.path)
    if checks:
        cmd.add("--checks=" + checks)
    for ep in entry_points:
        cmd.add("--entry-point")
        cmd.add(ep)
    cmd.add("--")
    cmd.add_all(parsed.compile_flags)

    _run_aspect_vycor(
        ctx = ctx,
        tool_info = tool_info,
        arg_builder = cmd,
        direct_inputs = [src_list] + cxx_srcs,
        transitive_depsets = [parsed.headers_depset],
        outputs = [report_out],
        mnemonic = "VycorAspectCheck",
        progress_message = "Running vycor check (aspect) -> " + report_out.short_path,
    )

    return [
        OutputGroupInfo(report = depset([report_out])),
    ]

vycor_check_aspect = aspect(
    implementation = _vycor_check_aspect_impl,
    attr_aspects = ["deps", "implementation_deps"],
    doc = "Runs `vycor-cpp check` over existing C/C++ targets and emits SARIF/JSON/text reports under `--output_groups=report`.",
    attrs = {
        "_check_format": attr.label(default = Label("//bazel:check_format")),
        "_checks": attr.label(default = Label("//bazel:checks")),
        "_cxx_standard": attr.label(default = Label("//bazel:cxx_standard")),
        "_entry_points": attr.label(default = Label("//bazel:entry_points")),
        "_fail_on": attr.label(default = Label("//bazel:fail_on")),
        "_vycor_bin": attr.label(cfg = "exec", default = Label("//bazel:vycor_bin")),
        "_vycor_executable": attr.label(default = Label("//bazel:vycor_executable")),
    },
)
