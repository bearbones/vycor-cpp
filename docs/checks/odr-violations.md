# odr-violations

**Default:** off · **Groups:** compute-heavy · **Diagnostics:** `ODR_DuplicateDefinition`, `ODR_DivergentDefinition`

Detects One-Definition-Rule violations among **vague-linkage** entities —
inline functions, in-class method bodies, class definitions — whose
definitions differ across definition sites or across translation units.

## Why no other tool catches this

Linkers reject duplicate *strong* symbols, so two non-inline definitions
already fail the build. Vague-linkage (weak/COMDAT) symbols are the
opposite: defining them in many TUs is *expected*, and the linker keeps
one arbitrary copy **without comparing content**. Two different bodies for
the same inline function sail through every ordinary build; behavior then
depends on link order. Catching this otherwise requires LTO + `-Wodr` or
ASan's ODR checker at runtime — neither of which most builds run.

When enabled, anneal's phase-1 index records a `clang::ODRHash` per
vague-linkage definition and compares project-wide:

- **`ODR_DivergentDefinition`** — ONE definition site whose body hashes
  differently across TUs: the definition depends on preprocessor state
  that differs between compile commands (`-D` flags, `NDEBUG`, config
  headers).
- **`ODR_DuplicateDefinition`** — the same entity defined at multiple
  distinct sites with differing content (two headers each define it).

Token-identical copies at different sites are deliberately **not**
flagged: vendored duplicates are benign, and since identical content
hashes identically this also eliminates path-spelling false positives.
Method-level findings inside an already-flagged class are suppressed as
echoes of the same root cause.

## Example

```
./limits.hpp:2: ODR violation: 'limits' at ./limits.hpp:2 compiles to 2 different
    definitions across TUs — its body depends on preprocessor state that differs
    between compile commands. Every TU must see an identical definition.
```

## Notes and limitations

- External-linkage entities only (internal linkage is per-TU by design);
  templates and implicit instantiations are out of scope; system headers
  are skipped.
- Grouped as compute-heavy: hashing every inline definition in every TU
  adds measurable phase-1 cost on large trees.

## Remediation

For divergence: make the definition independent of preprocessor state, or
unify the flags across every TU that can see it. For duplicates: unify the
two definitions into one header, or rename one of the entities.
