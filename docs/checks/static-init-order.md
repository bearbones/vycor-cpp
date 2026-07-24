# static-init-order

**Default:** on · **Groups:** — · **Diagnostics:** `StaticInit_OrderDependency`

Flags the classic **static initialization order fiasco**, proven rather
than pattern-guessed: a dynamic initializer in one TU directly reads a
global that is itself dynamically initialized in a *different* TU.

## Why no other tool catches this

Initialization order between translation units is unspecified. Tools like
clang-tidy can only warn about the *pattern* ("this global has a dynamic
initializer" — `cppcoreguidelines-avoid-non-const-global-variables` and
friends), which is so broad that everyone suppresses it. Whether an actual
fiasco exists depends on a cross-TU fact: does some *other* TU's
initializer read this one before it has run? Proving that needs both TUs'
initializer expressions — anneal's phase-1 index has them all.

Reads of a not-yet-initialized global observe its zero/constant-initialized
state — not garbage, but silently wrong, and whether it bites depends on
link order.

## Example

```cpp
// a.cpp
int ga = compute();      // dynamic init

// b.cpp
int gb = ga + 1;         // reads ga — may run before a.cpp's initializers
```

```
b.cpp:2: Static initialization order fiasco: 'gb' (b.cpp:2) is dynamically
    initialized from 'ga' (a.cpp:3), which is itself dynamically initialized in
    a different TU. Cross-TU initialization order is unspecified — the reader
    may observe its zero/constant-initialized state. Use a
    construct-on-first-use function-local static, or make the dependency
    constinit/constexpr.
```

## What is deliberately NOT flagged

- Dependencies on **constant-initialized** globals (`constexpr`,
  `constinit`, constant initializers): those are baked into the binary
  image before any dynamic initialization runs — always safe.
- **Same-file** dependencies: within one TU, initialization order is
  top-to-bottom and guaranteed.
- **Function-local statics**: lazily initialized on first pass — the
  standard fix, not a hazard.
- Dependencies through **virtual or function-pointer dispatch**: the
  check follows direct calls transitively (`int g = makeG();` where
  `makeG` — or anything it calls, up to depth 16 — reads another TU's
  dynamic global is flagged, with the chain in the message), but the
  name-level call summaries it walks don't resolve indirect dispatch.

## Remediation

Construct-on-first-use (`static T &instance() { static T t; return t; }`),
or make the dependency `constinit`/`constexpr` so it is initialized before
all dynamic initialization.
