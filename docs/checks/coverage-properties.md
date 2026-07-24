# coverage-properties

**Default:** off · **Groups:** noisy · **Diagnostics:** `Coverage_GVAMismatch`, `Coverage_DiscardableODR`, `Coverage_AvailableExternally`, `Coverage_PropertyDivergence`

Explains why certain header-defined member functions get dummy coverage
records (hash `0x0`) while sibling methods in the same class are
instrumented normally, by analyzing GVA linkage and COMDAT properties
across the whole project.

## Why no other tool catches this

Coverage instrumentation fate is decided per TU by GVA linkage
(`GVA_DiscardableODR` methods land in COMDAT sections the linker may
discard, taking their counters with them). Which copy survives — and
whether it was instrumented — depends on cross-TU emission decisions no
single-TU tool can see. anneal indexes the linkage-relevant properties of
every method project-wide and reports classes whose siblings diverge:

- **`Coverage_GVAMismatch`** — sibling methods with different GVA linkage.
- **`Coverage_DiscardableODR`** — methods in discardable COMDAT sections
  (coverage-record loss risk).
- **`Coverage_AvailableExternally`** — methods the optimizer may discard
  entirely.
- **`Coverage_PropertyDivergence`** — siblings diverging on
  complexity/instantiation properties.

## Notes

Grouped as **noisy**: these are explanations and risk markers, not
defects — expect to triage rather than fix mechanically. Enable when
debugging missing coverage data (`--coverage-diag` remains the legacy
toggle).
