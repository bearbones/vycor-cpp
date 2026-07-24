# default-arg-divergence

**Default:** on · **Groups:** — · **Diagnostics:** `DefaultArg_Divergent`

Flags functions whose declaration sites disagree on a parameter's default
argument: two headers spell **different** defaults for the same parameter
of the same function.

## Why no other tool catches this

Default arguments belong to *declarations*, not to the function. Each TU
uses whatever defaults its included declarations provided — and different
headers can legally carry different ones. Within one TU, redefining a
default is a compile error, so a compiler can never see the conflict: it
only exists *across* TUs. The program compiles and links everywhere, and
`log("msg")` quietly means `log("msg", 1)` in one TU and `log("msg", 2)`
in another.

anneal indexes every written-out default (declarations that merely inherit
a default from an earlier redeclaration in the same TU are skipped) and
compares the spellings across sites project-wide.

## Example

```cpp
// log_a.hpp
namespace myorg { void log(const char *msg, int level = 1); }

// log_b.hpp
namespace myorg { void log(const char *msg, int level = 2); }
```

```
log_a.hpp:2: Default-argument divergence: parameter 'level' of 'myorg::log' has
    conflicting defaults ('1' at log_a.hpp:2, '2' at log_b.hpp:2). Each TU
    silently calls with whichever value its includes provided — keep the default
    in exactly one declaration.
```

## What is deliberately NOT flagged

- **Absence vs. presence**: a site that omits the default where another
  writes one. The omitting side fails to compile short calls — a visible
  failure — and "header declares the default, .cpp redeclares without it"
  is a common, legal pattern.
- **Token-identical defaults** at multiple sites.
- **Same-site macro divergence** (`int level = DEFAULT_LEVEL` evaluating
  differently under different `-D` flags): the source spelling is
  identical, so this check cannot see it — but [odr-violations](odr-violations.md)
  catches the inline-function flavor of the same disease.

## Remediation

Keep the default argument in exactly one declaration — normally the one
canonical header — and make every other declaration site default-free.
