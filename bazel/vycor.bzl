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

"""Bazel rules, providers, and wrapper macros for vycor-cpp (`megascope` & `check`).

Architecture (mirrors `buck2/vycor.bzl` hierarchical map-reduce indexing):
  1. Per-TU artifacts (`.vycs` / `.mgsc` / `.shard.json` per translation unit):
     Each C/C++ source file is indexed in its own hermetic, remotely cacheable
     Bazel action via either:
       (a) `vycor-cpp megascope index --threads 1 --no-wait --index <tu.vycs> --source <tu.cpp> -- <flags>`, or
       (b) `clang++ -fsyntax-only -fplugin=<libvycor_megascope_plugin.so> ... -c <tu.cpp>`
     requiring NO `compile_commands.json` file.
  2. Per-library / per-binary target artifacts (`.vycs` / `.mgsc` per target):
     `vycor_target_index` merges a target's per-TU shards via `vycor-cpp megascope merge`,
     emits `OutputGroupInfo` groups (`tu`, `tu_<rel_src>`, `target`, `global`),
     and propagates O(1) Bazel `depset`s (`VycorIndexInfo`) to downstream dependents.
  3. Global index artifact (`.vycs` / `.mgsc` across targets):
     `vycor_global_index` projects the transitive `VycorIndexInfo` depset into a
     newline-delimited input list without analysis-time flattening and runs
     `vycor-cpp megascope merge` with automatic diamond-dependency deduplication.
  4. Hermetic query and check report rules (`vycor_query` and `vycor_check`)
     plus drop-in wrapper macros (`vycor_cc_library` / `vycor_cc_binary` and
     Buck2-compatible aliases `vycor_cxx_library` / `vycor_cxx_binary`).

For zero-config indexing and static analysis over unmodified `cc_library` /
`cc_binary` / `cc_test` targets, see `@vycor//bazel:aspects.bzl`.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(":settings.bzl", "VycorStringFlagInfo")

# ---------------------------------------------------------------------------
# Providers
# ---------------------------------------------------------------------------

VycorIndexInfo = provider(
    doc = "Carries per-TU, per-target, and transitive call-graph index artifacts and compile context.",
    fields = {
        "flags_depset": "depset[str] of transitive exported preprocessor/include flags.",
        "global_index": "File | None: merged transitive .vycs/.mgsc index covering this target and all deps.",
        "has_targets": "bool: True if target_depset is non-empty (O(1) check without depset flattening).",
        "has_tus": "bool: True if tu_depset is non-empty (O(1) check without depset flattening).",
        "headers_depset": "depset[File] of transitive public/exported headers required by downstream TUs.",
        "target_depset": "depset[File] of per-target .vycs/.mgsc/.shard.json artifacts in the dependency Closure.",
        "target_index": "File | None: merged .vycs/.mgsc index for this target's own TUs (None for header-only targets).",
        "tu_depset": "depset[File] of leaf per-TU .vycs/.mgsc/.shard.json shards in the dependency closure.",
        "tu_shards": "dict[str, File] mapping target-relative source path -> per-TU shard File.",
    },
)

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

_SHARD_EXTENSIONS = (".vycs", ".mgsc", ".pyrx", ".shard.json")

def _is_shard_file(f) -> bool:
    p = f.short_path
    for ext in _SHARD_EXTENSIONS:
        if p.endswith(ext):
            return True
    return False

def _normalize_ext(fmt: str) -> str:
    if fmt.startswith("."):
        fmt = fmt[1:]
    return fmt if fmt else "vycs"

def _resolve_index_ext(ctx) -> str:
    raw = getattr(ctx.attr, "index_format", "")
    if not raw and hasattr(ctx.attr, "_default_index_format"):
        raw = ctx.attr._default_index_format[VycorStringFlagInfo].value
    return _normalize_ext(raw)

def _resolve_vycor_exe_str(ctx) -> str:
    explicit = getattr(ctx.attr, "vycor_executable", "")
    if explicit:
        return explicit
    if hasattr(ctx.attr, "_default_vycor_executable"):
        return ctx.attr._default_vycor_executable[VycorStringFlagInfo].value
    return "vycor-cpp"

def _resolve_clang_exe_str(ctx) -> str:
    explicit = getattr(ctx.attr, "clang_executable", "")
    if explicit:
        return explicit
    if hasattr(ctx.attr, "_default_clang_executable"):
        return ctx.attr._default_clang_executable[VycorStringFlagInfo].value
    return "clang++"

def _resolve_plugin_path_str(ctx) -> str:
    explicit = getattr(ctx.attr, "vycor_plugin_path", "")
    if explicit:
        return explicit
    if hasattr(ctx.attr, "_default_vycor_plugin_path"):
        return ctx.attr._default_vycor_plugin_path[VycorStringFlagInfo].value
    return ""

def _workspace_prefix(label) -> str:
    ws = label.workspace_root
    pkg = label.package
    if ws and pkg:
        return ws + "/" + pkg
    elif ws:
        return ws
    return pkg

def _qualify_include_dirs(ctx, dirs: list) -> list:
    base = _workspace_prefix(ctx.label)
    bin_root = ctx.bin_dir.path
    bin_base = (bin_root + "/" + base) if base else bin_root
    out = []
    for inc in dirs:
        if inc == "." or inc == "":
            out.append("-I" + (base if base else "."))
            out.append("-I" + bin_base)
        elif inc.startswith("-"):
            out.append(inc)
        elif inc.startswith("/"):
            out.append("-I" + inc)
        else:
            rel = (base + "/" + inc) if base else inc
            out.append("-I" + rel)
            out.append("-I" + bin_root + "/" + rel)
    return out

def _cc_info_flags(cc_info) -> list:
    cc_ctx = cc_info.compilation_context
    flags = []
    for d in cc_ctx.defines.to_list():
        flags.append("-D" + d)
    for inc in cc_ctx.includes.to_list():
        flags.append("-I" + (inc if inc else "."))
    for qinc in cc_ctx.quote_includes.to_list():
        flags.append("-iquote" + (qinc if qinc else "."))
    for sinc in cc_ctx.system_includes.to_list():
        flags.append("-isystem" + (sinc if sinc else "."))
    for finc in cc_ctx.framework_includes.to_list():
        flags.append("-F" + finc)
    return flags

def _resolve_vycor_bin_target(ctx):
    explicit = getattr(ctx.attr, "vycor_bin", None)
    if explicit != None:
        return explicit
    default_bin = getattr(ctx.attr, "_default_vycor_bin", None)
    if default_bin != None:
        if default_bin[DefaultInfo].files_to_run.executable != None:
            return default_bin
        if default_bin[DefaultInfo].files.to_list():
            return default_bin
    return None

def _resolve_plugin_target(ctx):
    explicit = getattr(ctx.attr, "vycor_plugin", None)
    if explicit != None:
        return explicit
    default_plugin = getattr(ctx.attr, "_default_vycor_plugin", None)
    if default_plugin != None and default_plugin[DefaultInfo].files.to_list():
        return default_plugin
    return None

def _collect_dep_sets(deps: list) -> struct:
    target_transitive = []
    target_direct = []
    tu_transitive = []
    tu_direct = []
    header_transitive = []
    flag_transitive = []
    flag_direct = []
    has_targets = False
    has_tus = False

    for dep in deps:
        if VycorIndexInfo in dep:
            info = dep[VycorIndexInfo]
            target_transitive.append(info.target_depset)
            tu_transitive.append(info.tu_depset)
            header_transitive.append(info.headers_depset)
            flag_transitive.append(info.flags_depset)
            if info.has_targets:
                has_targets = True
            if info.has_tus:
                has_tus = True
        if CcInfo in dep:
            cc_info = dep[CcInfo]
            header_transitive.append(cc_info.compilation_context.headers)
            flag_direct.extend(_cc_info_flags(cc_info))
        if VycorIndexInfo not in dep and CcInfo not in dep and DefaultInfo in dep:
            # Cross-language shard targets (e.g. pyrex .mgsc / .pyrx / .shard.json rules).
            for f in dep[DefaultInfo].files.to_list():
                if _is_shard_file(f):
                    target_direct.append(f)
                    tu_direct.append(f)
                    has_targets = True
                    has_tus = True

    return struct(
        target_depset = depset(direct = target_direct, transitive = target_transitive),
        tu_depset = depset(direct = tu_direct, transitive = tu_transitive),
        has_targets = has_targets,
        has_tus = has_tus,
        headers_depset = depset(transitive = header_transitive),
        flags_depset = depset(direct = flag_direct, transitive = flag_transitive),
    )

def _run_tool_action(
        ctx,
        vycor_bin,
        exe_str: str,
        arg_builder,
        inputs_depset,
        outputs: list,
        mnemonic: str,
        progress_message: str,
        extra_tools: list = []):
    if vycor_bin != None and vycor_bin[DefaultInfo].files_to_run.executable != None:
        ctx.actions.run(
            executable = vycor_bin[DefaultInfo].files_to_run,
            arguments = [arg_builder],
            inputs = inputs_depset,
            outputs = outputs,
            tools = extra_tools,
            mnemonic = mnemonic,
            progress_message = progress_message,
            use_default_shell_env = True,
        )
    elif vycor_bin != None and vycor_bin[DefaultInfo].files.to_list():
        bin_file = vycor_bin[DefaultInfo].files.to_list()[0]
        shell_args = ctx.actions.args()
        shell_args.add(bin_file.path)
        ctx.actions.run_shell(
            command = 'exec "$@"',
            arguments = [shell_args, arg_builder],
            inputs = depset(direct = [bin_file], transitive = [inputs_depset]),
            outputs = outputs,
            tools = extra_tools,
            mnemonic = mnemonic,
            progress_message = progress_message,
            use_default_shell_env = True,
        )
    else:
        shell_args = ctx.actions.args()
        shell_args.add(exe_str)
        ctx.actions.run_shell(
            command = 'exec "$@"',
            arguments = [shell_args, arg_builder],
            inputs = inputs_depset,
            outputs = outputs,
            tools = extra_tools,
            mnemonic = mnemonic,
            progress_message = progress_message,
            use_default_shell_env = True,
        )

def vycor_plugin_copts(
        plugin,
        output_vycs: str = "",
        output_mgsc: str = "",
        output_shard: str = "",
        collapse_paths: list = [],
        lock_types: list = [],
        entry_points: list = [],
        channel_types_json: str = "",
        org_config: str = "",
        no_wait: bool = True) -> list:
    """Returns Clang `copts` (`-fplugin=...` and `-Xclang -plugin-arg-vycor-megascope -Xclang ...`)
    for emitting a `.vycs`, `.mgsc`, or `.shard.json` call-graph shard during `clang++ -c` compilation.

    When `plugin` is a Bazel target label (e.g. `"//tools:libvycor_megascope_plugin.so"`), pass it via
    `$(location ...)` in `copts` + `additional_compiler_inputs = [plugin]` on `cc_library` / `cc_binary`.
    """
    plugin_str = plugin if type(plugin) == "string" else str(plugin)
    flags = ["-fplugin=" + plugin_str]

    def _arg(val: str):
        flags.extend(["-Xclang", "-plugin-arg-vycor-megascope", "-Xclang", val])

    if output_vycs:
        _arg("--output=" + output_vycs)
    if output_mgsc:
        _arg("--emit-mgsc=" + output_mgsc)
    if output_shard:
        _arg("--emit-shard=" + output_shard)
    if no_wait:
        _arg("--no-wait")
    for pat in collapse_paths:
        _arg("--collapse-paths=" + pat)
    for lt in lock_types:
        _arg("--lock-types=" + lt)
    for ep in entry_points:
        _arg("--entry-point=" + ep)
    if channel_types_json:
        _arg("--channel-types-json=" + channel_types_json)
    if org_config:
        _arg("--org-config=" + org_config)
    return flags

# Alias matching `buck2/vycor.bzl` naming.
vycor_plugin_flags = vycor_plugin_copts

def _append_compile_flags(args, ctx, flags_depset):
    if getattr(ctx.attr, "cxx_standard", ""):
        args.add("-std=" + ctx.attr.cxx_standard)
    for inc_flag in _qualify_include_dirs(ctx, getattr(ctx.attr, "include_dirs", [])):
        args.add(inc_flag)
    for pf in getattr(ctx.attr, "preprocessor_flags", []):
        args.add(pf)
    args.add_all(flags_depset)
    for cf in getattr(ctx.attr, "compiler_flags", []):
        args.add(cf)

def _declare_tu_bake_action(
        ctx,
        src,
        out_name: str,
        ext: str,
        headers_depset,
        flags_depset):
    tu_out = ctx.actions.declare_file(out_name)
    plugin_target = _resolve_plugin_target(ctx)
    plugin_path = _resolve_plugin_path_str(ctx)

    extra_inputs = [src]
    if getattr(ctx.file, "channel_types_json", None) != None:
        extra_inputs.append(ctx.file.channel_types_json)
    if getattr(ctx.file, "org_config", None) != None:
        extra_inputs.append(ctx.file.org_config)

    if plugin_target != None or plugin_path != "":
        plugin_files = plugin_target[DefaultInfo].files.to_list() if plugin_target != None else []
        if plugin_target != None and not plugin_files:
            fail("vycor_plugin target provided no output files")
        plugin_arg = plugin_files[0].path if plugin_files else plugin_path
        extra_inputs.extend(plugin_files)

        args = ctx.actions.args()
        args.add("-fsyntax-only")
        args.add("-fplugin=" + plugin_arg)

        def _parg(val):
            args.add("-Xclang")
            args.add("-plugin-arg-vycor-megascope")
            args.add("-Xclang")
            args.add(val)

        if ext == "mgsc":
            _parg("--emit-mgsc=" + tu_out.path)
        elif ext == "shard.json":
            _parg("--emit-shard=" + tu_out.path)
        else:
            _parg("--output=" + tu_out.path)
        _parg("--no-wait")
        for pat in getattr(ctx.attr, "collapse_paths", []):
            _parg("--collapse-paths=" + pat)
        for lt in getattr(ctx.attr, "lock_types", []):
            _parg("--lock-types=" + lt)
        for ep in getattr(ctx.attr, "entry_points", []):
            _parg("--entry-point=" + ep)
        if getattr(ctx.file, "channel_types_json", None) != None:
            _parg("--channel-types-json=" + ctx.file.channel_types_json.path)
        if getattr(ctx.file, "org_config", None) != None:
            _parg("--org-config=" + ctx.file.org_config.path)
        if getattr(ctx.attr, "sysroot", ""):
            args.add("--sysroot=" + ctx.attr.sysroot)
        _append_compile_flags(args, ctx, flags_depset)
        for ea in getattr(ctx.attr, "extra_args", []):
            args.add(ea)
        args.add(src.path)

        inputs_depset = depset(direct = extra_inputs, transitive = [headers_depset])
        _run_tool_action(
            ctx = ctx,
            vycor_bin = None,
            exe_str = _resolve_clang_exe_str(ctx),
            arg_builder = args,
            inputs_depset = inputs_depset,
            outputs = [tu_out],
            mnemonic = "VycorTuPluginIndex",
            progress_message = "Indexing C++ TU (plugin) " + src.short_path,
        )
    else:
        args = ctx.actions.args()
        args.add("megascope")
        args.add("index")
        args.add("--threads")
        args.add("1")
        args.add("--no-wait")
        args.add("--index")
        args.add(tu_out.path)
        args.add("--source")
        args.add(src.path)
        for pat in getattr(ctx.attr, "collapse_paths", []):
            args.add("--collapse-paths")
            args.add(pat)
        for lt in getattr(ctx.attr, "lock_types", []):
            args.add("--lock-types")
            args.add(lt)
        if getattr(ctx.file, "channel_types_json", None) != None:
            args.add("--channel-types-json")
            args.add(ctx.file.channel_types_json.path)
        if getattr(ctx.file, "org_config", None) != None:
            args.add("--org-config")
            args.add(ctx.file.org_config.path)
        if getattr(ctx.attr, "sysroot", ""):
            args.add("--sysroot")
            args.add(ctx.attr.sysroot)
        for ea in getattr(ctx.attr, "extra_args", []):
            args.add("--extra-arg=" + ea)
        for ep in getattr(ctx.attr, "entry_points", []):
            args.add("--entry-point")
            args.add(ep)
        args.add("--")
        _append_compile_flags(args, ctx, flags_depset)

        inputs_depset = depset(direct = extra_inputs, transitive = [headers_depset])
        _run_tool_action(
            ctx = ctx,
            vycor_bin = _resolve_vycor_bin_target(ctx),
            exe_str = _resolve_vycor_exe_str(ctx),
            arg_builder = args,
            inputs_depset = inputs_depset,
            outputs = [tu_out],
            mnemonic = "VycorTuIndex",
            progress_message = "Indexing C++ TU " + src.short_path,
        )

    return tu_out

def _declare_copy_action(ctx, dst_name: str, src_file):
    dst = ctx.actions.declare_file(dst_name)
    ctx.actions.run_shell(
        command = 'cp -f "$1" "$2"',
        arguments = [src_file.path, dst.path],
        inputs = depset([src_file]),
        outputs = [dst],
        mnemonic = "VycorCopyIndex",
        progress_message = "Copying index " + dst.short_path,
        use_default_shell_env = False,
    )
    return dst

def _declare_merge_action(
        ctx,
        out_name: str,
        list_name: str,
        shards_depset,
        mnemonic: str,
        allow_mismatch: bool = False,
        entry_points: list = []):
    merged_out = ctx.actions.declare_file(out_name)
    list_file = ctx.actions.declare_file(list_name)

    list_args = ctx.actions.args()
    list_args.add_all(shards_depset)
    ctx.actions.write(
        output = list_file,
        content = list_args,
    )

    args = ctx.actions.args()
    args.add("megascope")
    args.add("merge")
    args.add("--no-wait")
    args.add("--output")
    args.add(merged_out.path)
    args.add("--input-list")
    args.add(list_file.path)
    if allow_mismatch:
        args.add("--allow-mismatch")
    for ep in entry_points:
        args.add("--entry-point")
        args.add(ep)

    inputs_depset = depset(direct = [list_file], transitive = [shards_depset])
    _run_tool_action(
        ctx = ctx,
        vycor_bin = _resolve_vycor_bin_target(ctx),
        exe_str = _resolve_vycor_exe_str(ctx),
        arg_builder = args,
        inputs_depset = inputs_depset,
        outputs = [merged_out],
        mnemonic = mnemonic,
        progress_message = "Merging vycor index " + merged_out.short_path,
    )
    return merged_out

def _sanitize_output_group(rel_path: str) -> str:
    out = rel_path
    for ch in ("/", ".", "-"):
        out = out.replace(ch, "_")
    return out

# ---------------------------------------------------------------------------
# Shared attributes
# ---------------------------------------------------------------------------

_COMMON_BAKE_ATTRS = {
    "channel_types_json": attr.label(
        allow_single_file = [".json"],
        doc = "Optional channel-types JSON config.",
    ),
    "clang_executable": attr.string(
        default = "",
        doc = "Clang++ executable when using vycor_plugin / vycor_plugin_path (defaults to @vycor//bazel:clang_executable).",
    ),
    "collapse_paths": attr.string_list(
        default = [],
        doc = "Path patterns to collapse in the call graph.",
    ),
    "compiler_flags": attr.string_list(
        default = [],
        doc = "Target-private compiler flags.",
    ),
    "cxx_standard": attr.string(
        default = "c++17",
        doc = "C++ standard flag (e.g. c++17, c++20).",
    ),
    "deps": attr.label_list(
        default = [],
        doc = "Dependencies providing VycorIndexInfo, CcInfo, or cross-language shard outputs.",
    ),
    "entry_points": attr.string_list(
        default = [],
        doc = "Entry-point function names recorded in the index.",
    ),
    "exported_headers": attr.label_list(
        allow_files = True,
        default = [],
        doc = "Public headers propagated to dependents.",
    ),
    "exported_include_dirs": attr.string_list(
        default = [],
        doc = "Package-relative public include directories propagated to dependents.",
    ),
    "exported_preprocessor_flags": attr.string_list(
        default = [],
        doc = "Public preprocessor/include flags propagated to dependents.",
    ),
    "extra_args": attr.string_list(
        default = [],
        doc = "Extra arguments forwarded via --extra-arg.",
    ),
    "headers": attr.label_list(
        allow_files = True,
        default = [],
        doc = "Private headers of this target.",
    ),
    "include_dirs": attr.string_list(
        default = ["."],
        doc = "Package-relative include directories.",
    ),
    "index_format": attr.string(
        default = "",
        doc = "Output shard/snapshot format extension: 'vycs', 'mgsc', or 'shard.json' (defaults to @vycor//bazel:index_format).",
    ),
    "lock_types": attr.string_list(
        default = [],
        doc = "Additional qualified lock type names.",
    ),
    "org_config": attr.label(
        allow_single_file = [".json"],
        doc = "Optional organization config JSON.",
    ),
    "preprocessor_flags": attr.string_list(
        default = [],
        doc = "Target-private preprocessor flags (-D, -I).",
    ),
    "sysroot": attr.string(
        default = "",
        doc = "Optional sysroot path.",
    ),
    "vycor_bin": attr.label(
        cfg = "exec",
        executable = True,
        doc = "Optional hermetic vycor-cpp executable target.",
    ),
    "vycor_executable": attr.string(
        default = "",
        doc = "Path or binary name of vycor-cpp when vycor_bin is not set (defaults to @vycor//bazel:vycor_executable).",
    ),
    "vycor_plugin": attr.label(
        allow_single_file = [".so", ".dylib", ".dll"],
        doc = "Optional libvycor_megascope_plugin.so target for Clang plugin indexing.",
    ),
    "vycor_plugin_path": attr.string(
        default = "",
        doc = "Optional path to libvycor_megascope_plugin.so for Clang plugin indexing.",
    ),
    "_default_clang_executable": attr.label(
        default = Label("//bazel:clang_executable"),
    ),
    "_default_index_format": attr.label(
        default = Label("//bazel:index_format"),
    ),
    "_default_vycor_bin": attr.label(
        cfg = "exec",
        default = Label("//bazel:vycor_bin"),
    ),
    "_default_vycor_executable": attr.label(
        default = Label("//bazel:vycor_executable"),
    ),
    "_default_vycor_plugin": attr.label(
        cfg = "exec",
        default = Label("//bazel:vycor_plugin"),
    ),
    "_default_vycor_plugin_path": attr.label(
        default = Label("//bazel:vycor_plugin_path"),
    ),
}

# ---------------------------------------------------------------------------
# Rule 1: vycor_tu_index (per-TU .vycs / .mgsc / .shard.json artifact)
# ---------------------------------------------------------------------------

def _vycor_tu_index_impl(ctx):
    dep_sets = _collect_dep_sets(ctx.attr.deps)
    own_headers = ctx.files.headers + ctx.files.exported_headers
    exported_flags = (
        _qualify_include_dirs(ctx, ctx.attr.exported_include_dirs) +
        list(ctx.attr.exported_preprocessor_flags)
    )

    exported_headers_depset = depset(
        direct = ctx.files.exported_headers,
        transitive = [dep_sets.headers_depset],
    )
    all_headers_depset = depset(
        direct = own_headers,
        transitive = [dep_sets.headers_depset],
    )
    exported_flags_depset = depset(
        direct = exported_flags,
        transitive = [dep_sets.flags_depset],
    )

    ext = _resolve_index_ext(ctx)
    out_name = ctx.attr.out if ctx.attr.out else (ctx.label.name + "." + ext)
    tu_shard = _declare_tu_bake_action(
        ctx = ctx,
        src = ctx.file.src,
        out_name = out_name,
        ext = ext,
        headers_depset = all_headers_depset,
        flags_depset = exported_flags_depset,
    )

    tu_depset = depset(direct = [tu_shard], transitive = [dep_sets.tu_depset])
    target_depset = depset(direct = [tu_shard], transitive = [dep_sets.target_depset])

    return [
        DefaultInfo(files = depset([tu_shard])),
        OutputGroupInfo(
            tu = depset([tu_shard]),
            target = depset([tu_shard]),
        ),
        VycorIndexInfo(
            tu_shards = {ctx.file.src.short_path: tu_shard},
            target_index = tu_shard,
            global_index = tu_shard,
            target_depset = target_depset,
            tu_depset = tu_depset,
            has_targets = True,
            has_tus = True,
            headers_depset = exported_headers_depset,
            flags_depset = exported_flags_depset,
        ),
    ]

vycor_tu_index = rule(
    implementation = _vycor_tu_index_impl,
    doc = "Indexes a single C/C++ translation unit into a hermetic .vycs, .mgsc, or .shard.json call-graph shard.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        out = attr.string(
            default = "",
            doc = "Optional output filename (defaults to <name>.<index_format>).",
        ),
        src = attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "The C/C++ translation unit source file to index.",
        ),
    ),
)

# ---------------------------------------------------------------------------
# Rule 2: vycor_target_index (per-TU shards + per-target .vycs/.mgsc + global)
# ---------------------------------------------------------------------------

def _vycor_target_index_impl(ctx):
    dep_sets = _collect_dep_sets(ctx.attr.deps)
    own_headers = ctx.files.headers + ctx.files.exported_headers
    exported_flags = (
        _qualify_include_dirs(ctx, ctx.attr.exported_include_dirs) +
        list(ctx.attr.exported_preprocessor_flags)
    )

    exported_headers_depset = depset(
        direct = ctx.files.exported_headers,
        transitive = [dep_sets.headers_depset],
    )
    all_headers_depset = depset(
        direct = own_headers,
        transitive = [dep_sets.headers_depset],
    )
    exported_flags_depset = depset(
        direct = exported_flags,
        transitive = [dep_sets.flags_depset],
    )

    ext = _resolve_index_ext(ctx)
    merged_ext = "mgsc" if ext == "shard.json" else ext

    # 1. Per-TU .vycs / .mgsc / .shard.json actions (one cacheable action per source file).
    tu_shards = {}
    tu_shard_list = []
    output_groups = {}
    for src in ctx.files.srcs:
        rel = src.short_path
        shard_out_name = "tu/" + ctx.label.name + "/" + rel + "." + ext
        shard = _declare_tu_bake_action(
            ctx = ctx,
            src = src,
            out_name = shard_out_name,
            ext = ext,
            headers_depset = all_headers_depset,
            flags_depset = exported_flags_depset,
        )
        tu_shards[rel] = shard
        tu_shard_list.append(shard)
        shard_set = depset([shard])
        output_groups["tu_" + rel] = shard_set
        output_groups["tu_" + _sanitize_output_group(rel)] = shard_set

    output_groups["tu"] = depset(tu_shard_list)

    # 2. Per-target .vycs / .mgsc artifact:
    #    - Header-only target (0 TUs): no per-target shard; forwards transitive
    #      headers, flags, and dep index depsets to downstream dependents.
    #    - Single-TU target (when ext != shard.json): copy the TU shard directly.
    #    - Multi-TU target (or shard.json -> mgsc): merge all per-TU shards.
    target_out_name = ctx.attr.out if ctx.attr.out else (ctx.label.name + "." + merged_ext)
    has_dep_targets = dep_sets.has_targets

    if len(tu_shard_list) == 0:
        target_index = None
        tu_depset = dep_sets.tu_depset
        target_depset = dep_sets.target_depset
        has_targets = dep_sets.has_targets
        has_tus = dep_sets.has_tus
    elif len(tu_shard_list) == 1 and ext == merged_ext:
        target_index = _declare_copy_action(ctx, target_out_name, tu_shard_list[0])
        tu_depset = depset(direct = tu_shard_list, transitive = [dep_sets.tu_depset])
        target_depset = depset(direct = [target_index], transitive = [dep_sets.target_depset])
        has_targets = True
        has_tus = True
    else:
        target_index = _declare_merge_action(
            ctx = ctx,
            out_name = target_out_name,
            list_name = ctx.label.name + ".tu_shards.list",
            shards_depset = depset(tu_shard_list),
            mnemonic = "VycorTargetMerge",
            allow_mismatch = ctx.attr.allow_mismatch,
            entry_points = ctx.attr.entry_points,
        )
        tu_depset = depset(direct = tu_shard_list, transitive = [dep_sets.tu_depset])
        target_depset = depset(direct = [target_index], transitive = [dep_sets.target_depset])
        has_targets = True
        has_tus = True

    if target_index != None:
        output_groups["target"] = depset([target_index])

    # 3. On-demand `global` output group merging this target + all transitive deps.
    if target_index == None and not has_dep_targets:
        global_index = None
    elif target_index != None and not has_dep_targets:
        global_index = target_index
    else:
        global_index = _declare_merge_action(
            ctx = ctx,
            out_name = ctx.label.name + ".global." + merged_ext,
            list_name = ctx.label.name + ".global_shards.list",
            shards_depset = target_depset,
            mnemonic = "VycorGlobalMerge",
            allow_mismatch = ctx.attr.allow_mismatch,
            entry_points = ctx.attr.entry_points,
        )

    if global_index != None:
        output_groups["global"] = depset([global_index])

    return [
        DefaultInfo(
            files = depset([target_index] if target_index != None else []),
        ),
        OutputGroupInfo(**output_groups),
        VycorIndexInfo(
            tu_shards = tu_shards,
            target_index = target_index,
            global_index = global_index,
            target_depset = target_depset,
            tu_depset = tu_depset,
            has_targets = has_targets,
            has_tus = has_tus,
            headers_depset = exported_headers_depset,
            flags_depset = exported_flags_depset,
        ),
    ]

vycor_target_index = rule(
    implementation = _vycor_target_index_impl,
    doc = "Produces per-TU .vycs/.mgsc/.shard.json shards, a per-target merged index, and a `global` output group.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        allow_mismatch = attr.bool(
            default = False,
            doc = "Allow merging shards with differing build configs.",
        ),
        out = attr.string(
            default = "",
            doc = "Optional output filename (defaults to <name>.<index_format>).",
        ),
        srcs = attr.label_list(
            allow_files = True,
            default = [],
            doc = "C/C++ source files in this target (empty for header-only libraries).",
        ),
    ),
)

# ---------------------------------------------------------------------------
# Rule 3: vycor_global_index (global .vycs / .mgsc across target graph)
# ---------------------------------------------------------------------------

def _vycor_global_index_impl(ctx):
    if not ctx.attr.deps and not ctx.files.extra_shards:
        fail("vycor_global_index: 'deps' or 'extra_shards' must not be empty")
    dep_sets = _collect_dep_sets(ctx.attr.deps)
    granularity = ctx.attr.merge_granularity
    if granularity not in ("target", "tu"):
        fail("vycor_global_index: merge_granularity must be 'target' or 'tu', got '" + granularity + "'")

    if granularity == "tu":
        merged_shards = depset(
            direct = ctx.files.extra_shards,
            transitive = [dep_sets.tu_depset],
        )
        has_any_shards = bool(ctx.files.extra_shards) or dep_sets.has_tus
    else:
        merged_shards = depset(
            direct = ctx.files.extra_shards,
            transitive = [dep_sets.target_depset],
        )
        has_any_shards = bool(ctx.files.extra_shards) or dep_sets.has_targets

    if not has_any_shards:
        fail("vycor_global_index: no indexed targets, TUs, or extra_shards found")

    ext = _resolve_index_ext(ctx)
    merged_ext = "mgsc" if ext == "shard.json" else ext
    out_name = ctx.attr.out if ctx.attr.out else (ctx.label.name + "." + merged_ext)
    global_index = _declare_merge_action(
        ctx = ctx,
        out_name = out_name,
        list_name = ctx.label.name + ".inputs.list",
        shards_depset = merged_shards,
        mnemonic = "VycorGlobalMerge",
        allow_mismatch = ctx.attr.allow_mismatch,
        entry_points = ctx.attr.entry_points,
    )

    all_tus = depset(
        direct = ctx.files.extra_shards,
        transitive = [dep_sets.tu_depset],
    )
    return [
        DefaultInfo(files = depset([global_index])),
        OutputGroupInfo(global_index = depset([global_index])),
        VycorIndexInfo(
            tu_shards = {},
            target_index = global_index,
            global_index = global_index,
            target_depset = depset([global_index]),
            tu_depset = all_tus,
            has_targets = True,
            has_tus = bool(ctx.files.extra_shards) or dep_sets.has_tus,
            headers_depset = dep_sets.headers_depset,
            flags_depset = dep_sets.flags_depset,
        ),
    ]

vycor_global_index = rule(
    implementation = _vycor_global_index_impl,
    doc = "Merges transitive per-target or per-TU .vycs/.mgsc/.shard.json indexes (including cross-language shards) into a single global call-graph index.",
    attrs = {
        "allow_mismatch": attr.bool(
            default = False,
            doc = "Allow merging shards whose build configuration or environment differs.",
        ),
        "deps": attr.label_list(
            default = [],
            doc = "Root targets whose transitive .vycs/.mgsc/.shard.json indexes will be merged.",
        ),
        "entry_points": attr.string_list(
            default = [],
            doc = "Override entry-point functions in the global index.",
        ),
        "extra_shards": attr.label_list(
            allow_files = True,
            default = [],
            doc = "Additional .mgsc, .vycs, .pyrx, or .shard.json shard artifacts (e.g. from pyrex Python targets) to merge.",
        ),
        "index_format": attr.string(
            default = "",
            doc = "Output snapshot format extension ('vycs' or 'mgsc') when 'out' is not set.",
        ),
        "merge_granularity": attr.string(
            default = "target",
            doc = "'target' merges per-target artifacts; 'tu' merges leaf per-TU shards directly.",
        ),
        "out": attr.string(
            default = "",
            doc = "Output filename (defaults to <name>.<index_format>).",
        ),
        "vycor_bin": attr.label(
            cfg = "exec",
            executable = True,
            doc = "Optional hermetic vycor-cpp executable target.",
        ),
        "vycor_executable": attr.string(
            default = "",
            doc = "Path or binary name of vycor-cpp when vycor_bin is not set.",
        ),
        "_default_index_format": attr.label(
            default = Label("//bazel:index_format"),
        ),
        "_default_vycor_bin": attr.label(
            cfg = "exec",
            default = Label("//bazel:vycor_bin"),
        ),
        "_default_vycor_executable": attr.label(
            default = Label("//bazel:vycor_executable"),
        ),
    },
)

# ---------------------------------------------------------------------------
# Rule 4: vycor_query (hermetic megascope query action against a .vycs/.mgsc index)
# ---------------------------------------------------------------------------

def _vycor_query_impl(ctx):
    index_target = ctx.attr.index
    index_artifact = None
    if VycorIndexInfo in index_target and index_target[VycorIndexInfo].global_index != None:
        index_artifact = index_target[VycorIndexInfo].global_index
    else:
        default_files = index_target[DefaultInfo].files.to_list()
        if default_files:
            index_artifact = default_files[0]
    if index_artifact == None:
        fail("vycor_query: 'index' target does not produce a .vycs or .mgsc index artifact")

    ext = "ndjson" if ctx.attr.format == "ndjson" else ("tsv" if ctx.attr.format == "tsv" else "json")
    out_name = ctx.attr.out if ctx.attr.out else (ctx.label.name + "." + ext)
    report_out = ctx.actions.declare_file(out_name)

    vycor_bin = _resolve_vycor_bin_target(ctx)
    extra_inputs = [index_artifact]
    tools = []
    if vycor_bin != None and vycor_bin[DefaultInfo].files_to_run.executable != None:
        exe_path = vycor_bin[DefaultInfo].files_to_run.executable.path
        tools = [vycor_bin[DefaultInfo].files_to_run]
    elif vycor_bin != None and vycor_bin[DefaultInfo].files.to_list():
        bin_file = vycor_bin[DefaultInfo].files.to_list()[0]
        exe_path = bin_file.path
        extra_inputs.append(bin_file)
    else:
        exe_path = _resolve_vycor_exe_str(ctx)

    args = ctx.actions.args()
    args.add(report_out.path)
    args.add("1" if ctx.attr.allow_empty else "0")
    args.add(exe_path)
    args.add("megascope")
    args.add(ctx.attr.tool)
    args.add("--index")
    args.add(index_artifact.path)
    args.add("--format")
    args.add(ctx.attr.format)
    if ctx.attr.pretty and ctx.attr.format == "json":
        args.add("--pretty")
    if ctx.attr.args_json:
        args.add("--args")
        args.add(ctx.attr.args_json)
    for ep in ctx.attr.entry_points:
        args.add("--entry-point")
        args.add(ep)
    for flag in ctx.attr.tool_flags:
        args.add(flag)

    script = (
        'set -e; out="$1"; allow_empty="$2"; shift 2; ' +
        '"$@" > "$out" || { rc=$?; if [ "$rc" -eq 1 ] && [ "$allow_empty" = "1" ]; then exit 0; fi; exit "$rc"; }'
    )
    ctx.actions.run_shell(
        command = script,
        arguments = [args],
        inputs = depset(extra_inputs),
        outputs = [report_out],
        tools = tools,
        mnemonic = "VycorQuery",
        progress_message = "Running vycor megascope " + ctx.attr.tool + " -> " + report_out.short_path,
        use_default_shell_env = True,
    )
    return [DefaultInfo(files = depset([report_out]))]

vycor_query = rule(
    implementation = _vycor_query_impl,
    doc = "Runs a `vycor-cpp megascope <tool>` query against a .vycs or .mgsc index and writes the JSON/NDJSON/TSV output artifact.",
    attrs = {
        "allow_empty": attr.bool(
            default = True,
            doc = "Treat exit code 1 (empty results, e.g. zero dead-code findings) as success.",
        ),
        "args_json": attr.string(
            default = "",
            doc = "Optional JSON object passed via --args.",
        ),
        "entry_points": attr.string_list(
            default = [],
            doc = "Optional --entry-point overrides.",
        ),
        "format": attr.string(
            default = "json",
            doc = "Output format: json, ndjson, or tsv.",
        ),
        "index": attr.label(
            mandatory = True,
            doc = "Target providing VycorIndexInfo or a .vycs/.mgsc default output.",
        ),
        "out": attr.string(
            default = "",
            doc = "Optional output filename.",
        ),
        "pretty": attr.bool(
            default = True,
            doc = "Indent JSON output.",
        ),
        "tool": attr.string(
            mandatory = True,
            doc = "Megascope verb or tool name (e.g. 'graph-summary', 'analyze-dead-code', 'info', 'dump').",
        ),
        "tool_flags": attr.string_list(
            default = [],
            doc = "Additional flags passed to the tool (e.g. ['--name', 'Foo::bar']).",
        ),
        "vycor_bin": attr.label(
            cfg = "exec",
            executable = True,
            doc = "Optional hermetic vycor-cpp executable target.",
        ),
        "vycor_executable": attr.string(
            default = "",
            doc = "Path or binary name of vycor-cpp when vycor_bin is not set.",
        ),
        "_default_vycor_bin": attr.label(
            cfg = "exec",
            default = Label("//bazel:vycor_bin"),
        ),
        "_default_vycor_executable": attr.label(
            default = Label("//bazel:vycor_executable"),
        ),
    },
)

# ---------------------------------------------------------------------------
# Rule 5: vycor_check (hermetic check static analysis report)
# ---------------------------------------------------------------------------

def _vycor_check_impl(ctx):
    dep_sets = _collect_dep_sets(ctx.attr.deps)
    own_headers = ctx.files.headers + ctx.files.exported_headers
    exported_flags = (
        _qualify_include_dirs(ctx, ctx.attr.exported_include_dirs) +
        list(ctx.attr.exported_preprocessor_flags)
    )

    all_headers_depset = depset(
        direct = own_headers,
        transitive = [dep_sets.headers_depset],
    )
    exported_flags_depset = depset(
        direct = exported_flags,
        transitive = [dep_sets.flags_depset],
    )

    ext = "sarif" if ctx.attr.format == "sarif" else ("json" if ctx.attr.format == "json" else "txt")
    out_name = ctx.attr.out if ctx.attr.out else (ctx.label.name + "." + ext)
    report_out = ctx.actions.declare_file(out_name)

    src_list = ctx.actions.declare_file(ctx.label.name + ".sources.list")
    src_list_args = ctx.actions.args()
    src_list_args.add_all(ctx.files.srcs)
    ctx.actions.write(
        output = src_list,
        content = src_list_args,
    )

    args = ctx.actions.args()
    args.add("check")
    args.add("--threads")
    args.add(str(ctx.attr.threads))
    args.add("--format")
    args.add(ctx.attr.format)
    args.add("--fail-on")
    args.add(ctx.attr.fail_on)
    args.add("--output")
    args.add(report_out.path)
    args.add("--source-list")
    args.add(src_list.path)
    if ctx.attr.checks:
        args.add("--checks=" + ctx.attr.checks)
    if ctx.file.baseline != None:
        args.add("--baseline")
        args.add(ctx.file.baseline.path)
    if ctx.file.org_config != None:
        args.add("--org-config")
        args.add(ctx.file.org_config.path)
    for ep in ctx.attr.entry_points:
        args.add("--entry-point")
        args.add(ep)
    for ea in ctx.attr.extra_args:
        args.add("--extra-arg=" + ea)
    args.add("--")
    _append_compile_flags(args, ctx, exported_flags_depset)

    direct_inputs = [src_list] + list(ctx.files.srcs)
    if ctx.file.baseline != None:
        direct_inputs.append(ctx.file.baseline)
    if ctx.file.org_config != None:
        direct_inputs.append(ctx.file.org_config)

    inputs_depset = depset(direct = direct_inputs, transitive = [all_headers_depset])
    _run_tool_action(
        ctx = ctx,
        vycor_bin = _resolve_vycor_bin_target(ctx),
        exe_str = _resolve_vycor_exe_str(ctx),
        arg_builder = args,
        inputs_depset = inputs_depset,
        outputs = [report_out],
        mnemonic = "VycorCheck",
        progress_message = "Running vycor check -> " + report_out.short_path,
    )
    return [
        DefaultInfo(files = depset([report_out])),
        OutputGroupInfo(report = depset([report_out])),
    ]

vycor_check = rule(
    implementation = _vycor_check_impl,
    doc = "Runs `vycor-cpp check` over a target's sources and produces a SARIF, JSON, or text report artifact.",
    attrs = dict(
        _COMMON_BAKE_ATTRS,
        baseline = attr.label(
            allow_single_file = True,
            doc = "Optional check baseline file.",
        ),
        checks = attr.string(
            default = "",
            doc = "Comma-separated check specification passed to --checks.",
        ),
        fail_on = attr.string(
            default = "none",
            doc = "Severity threshold that fails the build action: note, warning, error, or none.",
        ),
        format = attr.string(
            default = "sarif",
            doc = "Report format: sarif, json, or text.",
        ),
        out = attr.string(
            default = "",
            doc = "Optional output report filename.",
        ),
        srcs = attr.label_list(
            allow_files = True,
            mandatory = True,
            doc = "C/C++ source files to analyze.",
        ),
        threads = attr.int(
            default = 1,
            doc = "Thread count for the analysis action.",
        ),
    ),
)

# ---------------------------------------------------------------------------
# Drop-in Wrapper Macros: vycor_cc_library / vycor_cc_binary
# (plus Buck2-compatible aliases vycor_cxx_library / vycor_cxx_binary)
# ---------------------------------------------------------------------------

def _format_ext(index_format: str) -> str:
    if not index_format:
        return ""
    fmt = index_format[1:] if index_format.startswith(".") else index_format
    return "." + fmt if fmt else ""

def _to_vycor_dep(d: str) -> str:
    if d.endswith("_vycor"):
        return d
    if ":" not in d and ("//" in d or "@" in d):
        last = d.rsplit("/", 1)[-1]
        return d + ":" + last + "_vycor"
    return d + "_vycor"

def _defines_to_flags(defines: list) -> list:
    out = []
    for d in defines:
        out.append(d if d.startswith("-D") else ("-D" + d))
    return out

def vycor_cc_library(
        name: str,
        srcs: list = [],
        hdrs: list = [],
        textual_hdrs: list = [],
        deps: list = [],
        includes: list = [],
        copts: list = [],
        defines: list = [],
        local_defines: list = [],
        cxx_standard: str = "c++17",
        vycor_bin = None,
        vycor_executable: str = "",
        vycor_plugin = None,
        vycor_plugin_path: str = "",
        clang_executable: str = "",
        inject_plugin_into_cc: bool = False,
        collapse_paths: list = [],
        lock_types: list = [],
        enable_check: bool = True,
        checks: str = "",
        check_fail_on: str = "none",
        index_format: str = "",
        visibility = None,
        **kwargs):
    """Defines a Bazel `cc_library` alongside companion `:<name>_vycor` and `:<name>_check` targets.

    Generated targets:
      - `:<name>`: standard `@rules_cc//cc:defs.bzl` `cc_library`.
      - `:<name>_vycor`: `vycor_target_index` producing per-TU, per-target, and `global` output groups.
      - `:<name>_check`: `vycor_check` producing `<name>_check.sarif` (when `enable_check` and `srcs` are non-empty).
    """
    cc_copts = list(copts)
    extra_compiler_inputs = list(kwargs.pop("additional_compiler_inputs", []))
    if inject_plugin_into_cc and (vycor_plugin != None or vycor_plugin_path != ""):
        if vycor_plugin != None:
            extra_compiler_inputs.append(vycor_plugin)
            plugin_copt_ref = "$(location " + str(vycor_plugin) + ")"
        else:
            plugin_copt_ref = vycor_plugin_path
        cc_copts = cc_copts + vycor_plugin_copts(
            plugin = plugin_copt_ref,
            collapse_paths = collapse_paths,
            lock_types = lock_types,
        )

    cc_kwargs = dict(kwargs)
    if extra_compiler_inputs:
        cc_kwargs["additional_compiler_inputs"] = extra_compiler_inputs

    cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        textual_hdrs = textual_hdrs,
        deps = deps,
        includes = includes,
        copts = cc_copts,
        defines = defines,
        local_defines = local_defines,
        visibility = visibility,
        **cc_kwargs
    )

    vycor_deps = [":" + name] + [_to_vycor_dep(d) for d in deps]
    inc_dirs = list(includes) if includes else ["."]
    ext = _format_ext(index_format)
    merged_ext = ".mgsc" if ext == ".shard.json" else ext
    explicit_out = (name + merged_ext) if merged_ext else ""

    vycor_target_index(
        name = name + "_vycor",
        srcs = srcs,
        headers = textual_hdrs,
        exported_headers = hdrs,
        deps = vycor_deps,
        include_dirs = inc_dirs,
        exported_include_dirs = list(includes),
        compiler_flags = copts,
        preprocessor_flags = _defines_to_flags(local_defines),
        exported_preprocessor_flags = _defines_to_flags(defines),
        cxx_standard = cxx_standard,
        collapse_paths = collapse_paths,
        lock_types = lock_types,
        vycor_bin = vycor_bin,
        vycor_executable = vycor_executable,
        vycor_plugin = vycor_plugin,
        vycor_plugin_path = vycor_plugin_path,
        clang_executable = clang_executable,
        index_format = index_format,
        out = explicit_out,
        visibility = visibility,
    )

    if enable_check and srcs:
        vycor_check(
            name = name + "_check",
            srcs = srcs,
            headers = textual_hdrs,
            exported_headers = hdrs,
            deps = vycor_deps,
            include_dirs = inc_dirs,
            exported_include_dirs = list(includes),
            compiler_flags = copts,
            preprocessor_flags = _defines_to_flags(local_defines),
            exported_preprocessor_flags = _defines_to_flags(defines),
            cxx_standard = cxx_standard,
            checks = checks,
            fail_on = check_fail_on,
            vycor_bin = vycor_bin,
            vycor_executable = vycor_executable,
            visibility = visibility,
        )

def vycor_cc_binary(
        name: str,
        srcs: list = [],
        deps: list = [],
        includes: list = [],
        copts: list = [],
        defines: list = [],
        local_defines: list = [],
        cxx_standard: str = "c++17",
        entry_points: list = ["main"],
        vycor_bin = None,
        vycor_executable: str = "",
        vycor_plugin = None,
        vycor_plugin_path: str = "",
        clang_executable: str = "",
        inject_plugin_into_cc: bool = False,
        collapse_paths: list = [],
        lock_types: list = [],
        enable_check: bool = True,
        checks: str = "",
        check_fail_on: str = "none",
        index_format: str = "",
        visibility = None,
        **kwargs):
    """Defines a Bazel `cc_binary` alongside `:<name>_vycor`, `:<name>_vycor_global`, and `:<name>_check`.

    Generated targets:
      - `:<name>`: standard `@rules_cc//cc:defs.bzl` `cc_binary`.
      - `:<name>_vycor`: `vycor_target_index` for this binary's own TUs.
      - `:<name>_vycor_global`: `vycor_global_index` merging this binary and all transitive `deps`.
      - `:<name>_check`: `vycor_check` producing `<name>_check.sarif`.
    """
    cc_copts = list(copts)
    extra_compiler_inputs = list(kwargs.pop("additional_compiler_inputs", []))
    if inject_plugin_into_cc and (vycor_plugin != None or vycor_plugin_path != ""):
        if vycor_plugin != None:
            extra_compiler_inputs.append(vycor_plugin)
            plugin_copt_ref = "$(location " + str(vycor_plugin) + ")"
        else:
            plugin_copt_ref = vycor_plugin_path
        cc_copts = cc_copts + vycor_plugin_copts(
            plugin = plugin_copt_ref,
            collapse_paths = collapse_paths,
            lock_types = lock_types,
            entry_points = entry_points,
        )

    cc_kwargs = dict(kwargs)
    if extra_compiler_inputs:
        cc_kwargs["additional_compiler_inputs"] = extra_compiler_inputs

    # Separate headers passed in `srcs` (Bazel convention for cc_binary) from .cpp TUs.
    hdr_exts = (".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp")
    tu_srcs = [s for s in srcs if not any([s.endswith(e) for e in hdr_exts])]
    priv_hdrs = [s for s in srcs if any([s.endswith(e) for e in hdr_exts])]

    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps,
        includes = includes,
        copts = cc_copts,
        defines = defines,
        local_defines = local_defines,
        visibility = visibility,
        **cc_kwargs
    )

    vycor_deps = [_to_vycor_dep(d) for d in deps] + list(deps)
    inc_dirs = list(includes) if includes else ["."]
    ext = _format_ext(index_format)
    merged_ext = ".mgsc" if ext == ".shard.json" else ext
    target_out = (name + ".target" + merged_ext) if merged_ext else ""
    global_out = (name + ".global" + merged_ext) if merged_ext else ""

    vycor_target_index(
        name = name + "_vycor",
        srcs = tu_srcs,
        headers = priv_hdrs,
        deps = vycor_deps,
        include_dirs = inc_dirs,
        compiler_flags = copts,
        preprocessor_flags = _defines_to_flags(local_defines) + _defines_to_flags(defines),
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
        out = target_out,
        visibility = visibility,
    )

    vycor_global_index(
        name = name + "_vycor_global",
        deps = [":" + name + "_vycor"],
        entry_points = entry_points,
        vycor_bin = vycor_bin,
        vycor_executable = vycor_executable,
        index_format = index_format,
        out = global_out,
        visibility = visibility,
    )

    if enable_check and tu_srcs:
        vycor_check(
            name = name + "_check",
            srcs = tu_srcs,
            headers = priv_hdrs,
            deps = vycor_deps,
            include_dirs = inc_dirs,
            compiler_flags = copts,
            preprocessor_flags = _defines_to_flags(local_defines) + _defines_to_flags(defines),
            cxx_standard = cxx_standard,
            entry_points = entry_points,
            checks = checks,
            fail_on = check_fail_on,
            vycor_bin = vycor_bin,
            vycor_executable = vycor_executable,
            visibility = visibility,
        )

# Buck2-symmetric aliases
vycor_cxx_library = vycor_cc_library
vycor_cxx_binary = vycor_cc_binary
