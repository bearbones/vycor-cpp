# exception-escape

**Default:** off · **Groups:** noisy · **Diagnostics:** `Exception_Escape`

Flags `noexcept` functions that can transitively reach an uncaught
`throw` — across translation units — with no intervening handler. An
exception escaping a `noexcept` frame calls `std::terminate`.

## Why no other tool catches this

clang-tidy's `bugprone-exception-escape` analyzes one TU: it can see a
`throw` in the noexcept function's own body, or in callees whose bodies
happen to live in the same TU. The moment the call chain crosses a TU
boundary — `safeApi() noexcept` in `root.cpp` calling `mid()` in
`mid.cpp` calling `deepThrow()` in `thrower.cpp` — the single-TU view
ends at a declaration and the analysis goes silent. anneal's per-function
call summaries (collected during the same phase-1 parse, no extra cost
beyond enabling the check) cross the boundary.

## How it reasons

- Roots: every function whose exception specification means "cannot
  throw" (explicit `noexcept`, and implicitly-noexcept destructors).
- Propagation follows **unguarded** calls only: a call lexically inside a
  `try` is conservatively treated as handled there (handler type matching
  is out of scope — precision over recall).
- A hit is a function whose body contains a `throw` outside every `try`.
- A `noexcept` callee is a hard boundary: it would terminate in its own
  frame and gets its own diagnostic as a root.
- One diagnostic per root, shortest chain shown.

## Example

```
root.cpp:3: Exception escape: noexcept function 'app::safeApi' (root.cpp:3) can
    reach a throw in 'app::deepThrow' (app::mid -> app::deepThrow) with no
    intervening handler — an escaping exception calls std::terminate. Catch it
    inside, or drop the noexcept.
```

## Why it is grouped `noisy`

The summaries are name-level: overload sets sharing a qualified name are
conflated (over-approximation), virtual and function-pointer dispatch are
not followed (under-approximation), and reachability is lexical, not
path-sensitive — a throw behind an impossible condition still counts.
Treat findings as strong leads, not verdicts. Enable with
`--checks=exception-escape`.

## Remediation

Catch the exception inside the noexcept function, stop the callee from
throwing, or remove the `noexcept` if termination is not the intended
contract.
