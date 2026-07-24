# specialization-visibility

**Default:** on · **Groups:** — · **Diagnostics:** `Specialization_Invisible`

Flags translation units that implicitly instantiate a primary template
while an **explicit specialization** of that template — with those exact
arguments — exists somewhere else in the project, invisible to the
instantiating TU.

## Why no other tool catches this

Per `[temp.expl.spec]`, an explicit specialization must be declared before
the first use that would cause an implicit instantiation, *in every
translation unit where such a use occurs*. Violating this is **IFNDR** —
ill-formed, no diagnostic required:

- The **compiler** only diagnoses the in-TU variant (specialization
  declared *after* an instantiation it can see). When the specialization
  lives in a header the TU never includes, the TU compiles cleanly against
  the primary.
- The **linker** sees two vague-linkage symbols (the primary's
  instantiation and the specialization) and keeps one arbitrarily —
  no error, and *which one wins can change with link order*.

The result: some TUs run the primary's code, some the specialization's, or
the whole program silently flips between them. Classic real-world instance:
a `std::hash<MyType>` specialization in `MyTypeHash.hpp` that most — but
not all — TUs with an `unordered_map<MyType, ...>` include.

anneal's phase-1 index records every explicit specialization in the
project; phase 2 walks each TU's implicit instantiations and reports the
ones whose matching specialization is not visible there.

## Example

```cpp
// traits.hpp
template <typename T> struct Traits { static int id() { return 0; } };

// traits_int.hpp
template <> struct Traits<int> { static int id() { return 1; } };

// tu_bad.cpp — does NOT include traits_int.hpp
#include "traits.hpp"
int bad() { return Traits<int>::id(); }   // instantiates the primary

// tu_good.cpp
#include "traits.hpp"
#include "traits_int.hpp"
int good() { return Traits<int>::id(); }  // uses the specialization
```

```
tu_bad.cpp:3:22: IFNDR: this TU instantiates the primary template 'Traits<int>'
    but an explicit specialization exists at traits_int.hpp:4 and is not visible
    here. TUs will disagree about which definition to use. Include
    traits_int.hpp before the first use, or declare the specialization in the
    primary template's header.
```

## Notes and limitations

- Class templates only in this version; function template specializations
  are future work.
- Full explicit specializations only — partial specializations are
  patterns, not concrete argument lists, and matching them requires
  deduction.
- Specializations declared in system headers are ignored.
- The included-but-declared-too-late variant is not reported: the compiler
  already diagnoses it in-TU as a hard error.
- Template arguments are compared as canonical type spellings; exotic
  non-type arguments that print differently across TUs would fail to
  match (missed report, never a false positive).

## Remediation

Either include the specialization's header everywhere the template can be
instantiated (usually: declare the specialization in the same header as
whatever it specializes for), or stop specializing and use a different
customization point.
