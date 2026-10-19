# header-static-duplication

**Default:** on · **Groups:** — · **Diagnostics:** `HeaderStatic_Duplicated`

Flags **mutable** internal-linkage (`static` / anonymous-namespace)
variables defined in a header that two or more translation units have
each materialized — every including TU owns a private copy, so writes in
one TU are invisible in the others and the variable's address differs per
TU.

## Why no other tool catches this

A single-TU tool can see the *pattern* ("static variable in a header")
but not the *duplication*: whether any other TU actually includes that
header. Pattern-level warnings drown in the benign case — `static const`
tables and tags in headers are idiomatic and harmless — so they get
disabled. check records the defining site together with the TU that
materialized it, and reports only when:

- the variable is **mutable** (const/constexpr copies are identical and
  immutable per TU — silent), and
- **two or more distinct TUs** are proven to own a copy (listed in the
  diagnostic).

The classic symptom this heads off: a counter, cache, or registration
flag in a header that "works in the unit test" (one TU) and silently
forks in production (many TUs), with each TU seeing only its own
mutations.

## Example

```cpp
// state.hpp
static int gCounter = 0;                         // flagged when >= 2 TUs include this
inline int bump() { return ++gCounter; }         // each TU bumps ITS OWN counter
```

```
state.hpp:2: Header-static duplication: 'gCounter' is defined static in
    state.hpp:2, and 2 TUs (a.cpp, b.cpp) each creates a separate copy at
    different addresses.
```

Note the `bump()` combination: an inline function mutating a header
static is *also* an ODR violation (each TU's `bump` has a different body
referencing a different variable) — [odr-violations](odr-violations.md)
sees that flavor, this check names the root cause.

## What is deliberately NOT flagged

- `static const` / `constexpr` in headers — identical immutable copies,
  the common intended usage.
- **Pointer-to-const** (`static const char *kName = "..."`): the pointer
  itself is technically mutable, but the idiom is overwhelmingly used as
  a constant — flagging it would drown the signal. A genuinely reassigned
  const-pointee pointer is therefore missed (documented recall
  tradeoff).
- Header statics included by only **one** TU — no duplication realized
  (the diagnostic would be speculative).
- Statics in the TU's **main file** — the normal, intended meaning of
  `static`.
- `inline` variables (C++17) and `extern` declarations — external
  linkage, one program-wide copy.

## Remediation

Choose the fix that matches the intended semantics of the variable:

1. **One shared mutable object across all TUs (most common):**
   - In C++17 and later, replace `static` with `inline` (`inline int gCounter = 0;`) so every TU shares a single object at one address.
   - In pre-C++17 code, declare the variable `extern int gCounter;` in the header and place a single `int gCounter = 0;` definition in one `.cpp` file.
2. **An immutable constant:**
   - Mark the variable `constexpr` (or `static const` when `constexpr` is not possible). Immutable header copies are benign and are not flagged.
3. **Intentional per-TU private state:**
   - If each translation unit genuinely needs its own independent copy (for example, a per-TU registration guard or TU-local cache), move the `static` variable into the `.cpp` files that use it, or suppress the finding at the header definition with `// vycor-disable-next-line header-static-duplication` (or `// vycor: ignore[header-static-duplication]`).
