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

| Check | Default | Groups | Summary |
|---|---|---|---|
| [adl-visibility](adl-visibility.md) | on | — | Fragile ADL resolutions: an invisible overload would win or tie |
| [ctad-visibility](ctad-visibility.md) | on | — | CTAD deducing differently because a deduction guide is not included |
| [specialization-visibility](specialization-visibility.md) | on | — | TU instantiates a primary template whose explicit specialization exists elsewhere (IFNDR) |
| [default-arg-divergence](default-arg-divergence.md) | on | — | Declaration sites that disagree on a parameter's default argument |
| [odr-violations](odr-violations.md) | off | compute-heavy | Vague-linkage definitions that differ across sites or TUs |
| [coverage-properties](coverage-properties.md) | off | noisy | GVA linkage / COMDAT properties that make coverage records vanish |
| [dead-code](dead-code.md) | off | compute-heavy | Functions unreachable from the entry points via the call graph |

## Organization checks

Checks registered from `ext/` (per-TU `AnnealCheck` or cross-TU
`IndexCheck`) participate in the same selection by their `name()`, default
to enabled, and should ship their own page under the fork's `docs/checks/`.
See [docs/EXTENDING.md](../EXTENDING.md).
