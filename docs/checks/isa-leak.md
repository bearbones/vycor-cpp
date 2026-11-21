# isa-leak

**Default:** off · **Severity:** warning · **Group:** compute-heavy ·
**Diagnostics:** `IsaLeak_Divergent` (warning), `IsaLeak_Inlined` (note)

Reports vague-linkage functions (inline functions, in-class method bodies,
implicit template instantiations) emitted by translation units built with
different code-generation flags. The linker keeps one COMDAT copy of such a
function for every caller. If one TU compiled it with `-mavx2` and another
without, every caller gets whichever copy the linker kept, including callers
on the baseline path of a runtime CPU dispatch.

```cpp
// dot.h
inline float dot(const float *a, const float *b, std::size_t n) {
  float s = 0;
  for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

// fast_path.cpp — built with -mavx2, called only after a CPUID check
float dot_avx2(const float *a, const float *b, std::size_t n) { return dot(a, b, n); }

// generic.cpp — built for the baseline, the fallback on older CPUs
float dot_generic(const float *a, const float *b, std::size_t n) { return dot(a, b, n); }
```

When the compiler does not inline `dot`, both objects carry a weak copy of
`_Z3dotPKfS0_m`. Which copy survives depends on link order. With
`fast_path.o` first, `dot_generic` calls VEX-encoded AVX code and dies with
SIGILL on a CPU without AVX. Nothing in the source of `generic.cpp` explains
the crash.

```sh
vycor-cpp check --build-path build --source fast_path.cpp --source generic.cpp \
  --checks=-all,isa-leak
```

```text
dot.h:2: [isa-leak] 'dot(const float *, const float *, std::size_t)' has vague
linkage and is emitted by TUs built with different code generation (isa):
[1] fast_path.cpp (target features +avx,+avx2,...); [2] generic.cpp (no extra
target features). The linker keeps one copy of '_Z3dotPKfS0_m' for every
caller, picked by link order; ...
```

You get one finding per function and target triple. It names one witness TU
per distinct group of settings, with a count of the other TUs in that group,
and says which settings differ.

The diagnostic kind says where the sensitivity comes from:

- **`IsaLeak_Divergent` (warning).** The function's own body has what the
  differing settings compile differently, such as a loop, floating-point code
  or a `try` block. Its out-of-line copies differ at any optimization level.
- **`IsaLeak_Inlined` (note).** The sensitivity comes only from bodies the
  function can inline, for example a helper's loop or a `std::string`
  constructor's `memcpy`. The copies differ only in an optimizing build that
  emits this function out of line and still inlines into it. A `-O0` copy is
  the same in every TU. Treat these as leads.

## Facets and the flags that count

A *profile* is computed once per compile command from the parsed invocation,
not from the command-line text. That way, spellings that mean the same thing
compare equal, such as `-march=haswell` and its `-m<feature>` expansion, or
`-mavx2` and `-mavx -mavx2`. `--isa-leak-facets` picks which parts of the
profile are compared. The default is `isa,fp,eh`; `all` adds `overflow`.

| Facet | Compared | Changes code for a body that has |
|-------|----------|----------------------------------|
| `isa` | The effective target features (`-march`, `-mcpu`, `-m<feature>`, `-mno-<feature>`) | See the feature classes below |
| `fp` | `-ffp-contract`, `-fassociative-math`, `-f[no-]honor-nans`/`-infinities`, `-f[no-]signed-zeros`, `-freciprocal-math`, `-frounding-math`, `-ffp-exception-behavior`, `-ffp-eval-method`, `-mfpmath` (all set by `-ffast-math` and friends) | Floating-point arithmetic, comparisons, conversions or vector types (not values that are only loaded, passed or returned) |
| `fp` | `-f[no-]math-errno`, `-fapprox-func` | Calls to math builtins (`sqrt`, `exp`, ...) |
| `eh` | `-fexceptions` vs `-fno-exceptions` | A `try` block, or a destructor-bearing local or temporary around a call not declared non-throwing |
| `overflow` | `-fwrapv`, `-ftrapv` | Signed integer `+ - * <<`, increments and negation |

The `isa` facet splits target features into classes. It compares only the
classes the body can be affected by:

- **Vector and floating-point units** (`sse*`, `avx*`, `fma`, `f16c`, AArch64
  `neon`/`sve*`, PowerPC `altivec`/`vsx`, WebAssembly `simd128`, RISC-V `v`/`zv*`,
  ...) are compared for bodies with a loop (a vectorization candidate),
  floating-point values (even ones only loaded, passed or returned: under
  `-mavx` every scalar floating-point move is VEX-encoded), vector types or
  `memcpy`/`memset`.
- **Bit manipulation** (`popcnt`, `lzcnt`, `bmi`, `bmi2`, `movbe`, RISC-V
  `zb*`, ...) is compared for bodies with a loop or a bit builtin
  (`__builtin_popcount`, `clz`, `ctz`, `ffs`, `parity`, `clrsb`, `bswap`,
  rotates, and the generic `popcountg`/`clzg`/`ctzg` where Clang has them).
- **Every other feature** (AES, CRC32, SHA, ...) is compared only for bodies
  that call a target-specific builtin, which also makes every other class
  relevant.

The *body* of a function includes every body it can inline from the same TU,
transitively, including system-header code. A function with no loop of its
own that calls one that has a loop is sensitive to vector features, and is
reported as `IsaLeak_Inlined`. A `do { ... } while (0)` statement macro, or
any loop whose condition is the constant `false`, is not a loop.

Optimization level, debug info, warnings, defines and include paths are not
code-generation facets. `-O0` against `-O2` is not reported.

## What is reported

A function is a candidate in a TU when that TU *emits* it. Emission follows
how code generation decides what to emit: start from what the TU must emit,
then follow every function a walked body references. A TU must emit:

- its strong definitions;
- `used`, `constructor` and `destructor` functions;
- the dynamic initialization and destruction of global variables, as Clang
  decides (`ASTContext::DeclMustBeEmitted`): every variable another TU can
  need, and an inline or template variable whose destructor is non-trivial
  or whose initializer has side effects and is not a constant. Any other
  variable is emitted only where emitted code references it. A constant
  initializer runs nothing, so a `constexpr` constructor it names is not
  emitted for it;
- the vtables it homes, meaning those of classes whose key function it
  defines, and the vtables of dynamic classes without a key function that it
  constructs. A vtable emits every function in its slots, the final
  overriders inherited from bases included.

An inline function referenced only by another inline function that nothing
calls is not emitted. `Decl::isUsed` is not consulted. A virtual call does
not emit its callee: the callee is reached through a vtable or not at all.
The exception is a call on a complete object (a variable of class type or a
temporary), which Clang makes a direct call.
Code the compiler evaluates or discards emits nothing: unevaluated operands
(`sizeof`, `noexcept`, `decltype`, a non-polymorphic `typeid`),
initializers of `constexpr` locals, `case` labels and other constant
expressions, and the discarded branch of `if constexpr`. By-value
parameters are destroyed by the caller in the Itanium ABI and by the callee
only where the target's C++ ABI says so (Microsoft).

These are excluded:

- implicit and defaulted-on-first-declaration special members (their effect
  is folded into the functions that use them);
- lambdas' call operators and members of function-local classes (folded into
  their enclosing function);
- `consteval` and `always_inline` functions;
- functions with their own `target`, `target_clones`, `target_version`,
  `cpu_specific` or `cpu_dispatch` attribute, from every facet (the
  attribute pins their target features; their bits are also not folded into
  callers);
- definitions in system headers;
- non-C++ TUs.

A finding needs at least two distinct projected profiles for the same
function on the same target triple. Different triples are different link
units and are never compared.

## What the check cannot see

- **Inlining.** A function that every caller inlines has no out-of-line copy
  for the linker to pick, so the hazard does not exist. The check reasons
  about bodies, not about the optimizer's decisions, so it reports these
  functions too. At `-O2`/`-O3` small helpers often inline everywhere. At
  `-O0`, and wherever the inliner declines (size, recursion, an address taken,
  `noinline`), the copy is real.
- **Which copy the linker keeps.** That depends on link order and on the
  linker. The check reports that the choice exists, not what it was.
- **Link units.** The selected TUs are treated as one program. Select TUs that
  are linked together. A shared library resolves COMDATs among its own
  objects, and the check cannot infer the link graph.
- **System headers.** Standard-library templates such as `std::vector<float>`
  members and `<algorithm>` loops are a common real source of this leak, but
  they are excluded by default.
- **Integer straight-line code.** Code without loops, floating point or bulk
  memory operations is treated as ISA-insensitive. The compiler may still pick
  different encodings for it, for example a VEX-encoded struct copy.
- **Code-generation state outside the profile.** Denormal flushing (the
  `crtfastmath.o` startup object), `#pragma float_control` or
  `#pragma clang fp` inside a body, and GCC-only `optimize` attributes (the
  parse is done by Clang) are not modeled.

## Remediation

- Build shared headers' consumers with one set of flags.
- Give the copies in specially built TUs internal linkage, for example by
  wrapping the SIMD code in a per-ISA namespace, as OpenCV's
  `CV_CPU_OPTIMIZATION_NAMESPACE` does, or in an anonymous namespace.
- For target features, mark the function with a `target` attribute so every
  copy is compiled the same way.

## Freshness and persistence

Profiles come from the compile command, so `--checkpoint` reuse is turned off
when this check is enabled. The stderr output says so. The legacy journal does
not revalidate compile-command changes (issue
[#84](https://github.com/bearbones/vycor-cpp/issues/84)). Profiles and emitted
functions travel in isolated-worker shards (journal/shard format version 15)
as a profile table, a TU table, a function table and flat id triples; a
sighting that names no table entry fails the shard.
They are left out of the phase-2 handoff, because the analysis runs only in
the parent. `--isa-leak-facets` is applied there too, so workers never see it.
