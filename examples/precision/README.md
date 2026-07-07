# precision/ — F8 identity-precision fixtures

Fixtures for the USR-identity work (`docs/design-f8-usr-identity.md`).
Each encodes a case where name-keyed node identity gives a WRONG answer:

- `overloads.*` — two `process` overloads with disjoint callers; name
  identity merges their caller sets.
- `templates.*` — two explicit `parse<T>` specializations with disjoint
  callers; same merge.
- `macro_sites.cpp` — one macro expanded in two functions: call sites
  share a spelling location, so a spelling-keyed context index drops one.

The F8 PR-A test suite builds graphs over these files and asserts the
PRECISE behavior (disjoint callers, both macro contexts). Those tests
are expected to FAIL until the identity core lands (PR B) — they are the
red half of the fixtures-first gate.
