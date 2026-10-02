# anneal checks

Every anneal analysis is a **named check** with its own documentation page,
selectable through a clang-tidy-style specification. All of them share one
property: they compare information **across translation units**, which is
exactly what single-TU tools (clang-tidy, compiler warnings) structurally
cannot do.

Run `vycor-cpp anneal --list-checks` for the live table (organization
checks included).

## Selecting checks

Three sources, applied in order (later entries win):

1. **`.vycor-anneal.json`** — discovered by walking up from the working
   directory (like `.clang-tidy`), or named explicitly with
   `--checks-config <file>`:

   ```json
   { "checks": ["all", "-coverage-properties", "-compute-heavy"] }
   ```

2. **`--checks=<spec>`** on the command line, same syntax, comma-separated:

   ```bash
   vycor-cpp anneal --build-path build --source ... --checks=all,-dead-code
   ```

3. **Legacy toggle flags** (`--odr-diag`, `--coverage-diag`, `--dead-code`)
   append their check as an enable — existing invocations keep working.

Spec entries: `name` enables, `-name` disables, and group names expand to
their members. Unknown names are a hard error (typo protection). With no
configuration at all, the **default** column below applies, plus every
registered organization check.

## Groups

| Group | Meaning |
|---|---|
| `all` | every known check, built-in and organization |
| `noisy` | checks whose findings often need human triage |
| `compute-heavy` | checks that add indexing or graph-construction cost |

Group labels are selection handles, not behavior; membership below is the
initial seeding and may evolve. Organizations can define their own groups
(`registry.addCheckGroup("myorg-strict", {...})` — see
[docs/EXTENDING.md](../EXTENDING.md)).

## Built-in checks

| Check | Default | Severity | Groups | Summary |
|---|---|---|---|---|
| [adl-visibility](adl-visibility.md) | on | warning | — | Fragile ADL resolutions: an invisible overload would win or tie |
| [ctad-visibility](ctad-visibility.md) | on | warning | — | CTAD deducing differently because a deduction guide is not included |
| [specialization-visibility](specialization-visibility.md) | on | error | — | TU instantiates a primary template whose explicit specialization exists elsewhere (IFNDR) |
| [default-arg-divergence](default-arg-divergence.md) | on | warning | — | Declaration sites that disagree on a parameter's default argument |
| [static-init-order](static-init-order.md) | on | warning | — | Dynamic initializers reading another TU's dynamically-initialized global (SIOF) |
| [header-static-duplication](header-static-duplication.md) | on | warning | — | Mutable header-defined statics materialized by multiple TUs (forked per-TU state) |
| [exception-spec-divergence](exception-spec-divergence.md) | on | error | — | Declaration sites that disagree on whether a function can throw |
| [static-init-hazards](static-init-hazards.md) | off | warning | compute-heavy | Static initializers reaching dlopen/dlsym or thread create/join (loader-lock deadlock risk) |
| [exception-escape](exception-escape.md) | off | note | noisy | noexcept functions that can transitively reach an uncaught throw across TUs |
| [odr-violations](odr-violations.md) | off | error | compute-heavy | Vague-linkage definitions that differ across sites or TUs |
| [coverage-properties](coverage-properties.md) | off | note | noisy | GVA linkage / COMDAT properties that make coverage records vanish |
| [dead-code](dead-code.md) | off | note | compute-heavy | Functions unreachable from the entry points via the call graph |

Severity is the SARIF `level` and what `--fail-on` compares against:
`error` for ill-formed programs (IFNDR, ODR), `warning` for proven
hazards, `note` for leads that need triage. Two kinds are notes
whatever their check: `ADL_SameScore` (`--warn-same-score`) and
`DeadCode_Optimistic`. Organization checks are warnings.

## Organization checks

Checks registered from `ext/` (per-TU `AnnealCheck` or cross-TU
`IndexCheck`) participate in the same selection by their `name()`, default
to enabled, and should ship their own page under the fork's `docs/checks/`.
See [docs/EXTENDING.md](../EXTENDING.md).

They inherit everything below through `name()`: it is the finding's
check name, the fingerprint's check input, and the SARIF rule id (with
no `helpUri`, since the page lives in the fork). A Custom diagnostic
whose check left `checkName` empty is attributed to the check that
emitted it.

## Finding identity

Every finding carries a **fingerprint**, the identity that baselines
(`--baseline`), the JSON report, and SARIF `partialFingerprints`
(`vycorFingerprint/v1`) use. Version 1 is the first 16 hex digits of
xxh3-64 over, NUL-separated:

1. `vycor-finding/v1`;
2. the check name (`adl-visibility`, or an organization check's
   `name()`);
3. the kind (`ADL_Fallback`, `ODR_DuplicateDefinition`, ...);
4. the finding's file relative to the project root (`--project-root`,
   default the working directory), `/`-separated;
5. the identity of the entities involved: the qualified names,
   signatures, or USRs the check records (`Diagnostic::entities`); for
   the ADL, CTAD, coverage, and dead-code checks, the resolved and the
   better declaration and the missing header (relative to the root);
   for a check that records neither, the message with every
   `:<line>[:<col>]` removed and every path under the project root made
   root-relative (organization checks should still set `entities`: a
   path outside the root, or any other text that varies, stays in);
6. for the call-site checks (`adl-visibility`, `ctad-visibility`), the
   enclosing function: the USR of the nearest function around the call
   or declaration that is not a lambda or a local class's method (whose
   USRs carry byte offsets), or of the variable for a namespace-scope
   initializer (`Diagnostic::scope`).

No line or column number enters, so inserting or deleting unrelated
lines above a finding leaves its fingerprint unchanged; renaming an
entity involved, renaming the enclosing function, or moving the finding
to another file, changes it. Moving the checkout elsewhere does not:
every location is made absolute against its TU's compile directory
(so a relative compile command, `directory: build` with
`file: ../src/x.cpp` as Meson writes it, names the real file) and then
relative to the project root.

A fingerprint names a kind of finding, not one occurrence: the same
fragile call written twice in one function gives two findings with the
same fingerprint, and nothing numbers them by line (a number assigned in
line order would move to a different call whenever a copy is inserted
above). The baseline counts occurrences per fingerprint instead (below).
Exact duplicates (the same header finding reached through several TUs)
are merged into one finding.

## Baselines

`--write-baseline <file>` records the current findings; `--baseline
<file>` then reports only what is new. The file (version 2) holds one
entry per distinct fingerprint with how many findings carried it:

```json
{"version": 2, "tool": "vycor-cpp anneal", "findings": [
  {"fingerprint": "afd3147461fcb3df", "count": 2,
   "check": "adl-visibility", "file": "src/use.cpp",
   "message": "Fragile ADL resolution: ..."}]}
```

`check`, `file`, and `message` are the first such finding's, for people
reading the file; only `fingerprint` and `count` are matched. Per
fingerprint, when a run has N findings and the baseline count is B,
N − B of them are reported as new (none when N ≤ B) and B − N
occurrences are listed as stale (`staleBaseline`, with a `count`). Which
of N identical findings is reported cannot be known from the fingerprint,
so with `--patch-file` or `--git-base` the ones on changed lines are
reported first (a new copy of a baselined call, added above the old ones,
is reported at its own line and is not filtered out as unchanged), and
otherwise the first in report order.

A version-1 file (one entry per finding, colliding fingerprints suffixed
`-1`, `-2`, ...) is still read: the suffixes are dropped and the entries
counted. Rewrite it with `--write-baseline`; call-site findings now
include their enclosing function, so their fingerprints differ from the
ones a version-1 file recorded.

## Suppressing a finding

A comment on the finding's line or the line above it:

```cpp
// vycor: ignore[adl-visibility]
scale(v, 3.14);
scale(v, 2.5); // vycor: ignore[adl-visibility, odr-violations]
// vycor: ignore[*]
scale(v, 1.5);
```

`*` suppresses every check. Suppressed findings are counted in the
summary (`suppressed`) and do not affect the exit code. `-v` lists
suppressions that suppressed nothing, in the analyzed TUs and the files
holding findings. For many historical findings at once, use a baseline
(`--write-baseline` / `--baseline`) instead.
