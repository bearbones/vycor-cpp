# ctad-visibility

**Default:** on · **Groups:** — · **Diagnostics:** `CTAD_Fallback`

Flags class template argument deduction (CTAD) sites that would deduce a
different type if a deduction guide declared elsewhere in the project were
visible in this translation unit.

## Why no other tool catches this

Deduction guides participate in CTAD only when declared in the TU doing
the deduction. `Container c("hello")` deduces `Container<const char *>`
from the constructors alone — unless the TU includes the header carrying
`Container(const char *) -> Container<std::string>`, in which case it
deduces `Container<std::string>`. Both versions compile; the object's type
silently differs per TU with include order. A single-TU tool can't know
the guide exists.

anneal indexes every deduction guide project-wide and compares each TU's
actual deduction against what the invisible guides would have produced.

## Example

```cpp
// Guide.hpp
Container(const char *) -> Container<std::string>;

// user.cpp — no Guide.hpp include
Container c("hello");   // deduces Container<const char *> here,
                        // Container<std::string> in TUs that include Guide.hpp
```

## Remediation

Declare deduction guides in the same header as the class template they
guide, so no TU can see the template without them.
