# could-be-internal

**Default:** off · **Severity:** warning (tier 1), note (the rest) ·
**Groups:** compute-heavy · **Diagnostics:** `Internal_Unreferenced`,
`Internal_DeclaredInHeader`, `Internal_TestOnly`, `Internal_Unproven`

Reports functions and variables with external linkage that no other
translation unit references. Such a symbol could have internal linkage
(`static`, or an anonymous namespace). Internal linkage lets the
optimizer inline or drop it, and keeps its name out of the program-wide
symbol table where another TU's same-named symbol could clash with it.

```bash
vycor-cpp check --build-path build --source-list sources.txt \
  --checks=-all,could-be-internal
```

## Findings

| Diagnostic | Severity | When |
|---|---|---|
| `Internal_Unreferenced` | warning | No header declares the symbol and no other TU references it. |
| `Internal_DeclaredInHeader` | note | A header declares it, but no other TU references it. Making it internal means removing that declaration, which may be public API for code outside the analyzed sources. |
| `Internal_TestOnly` | note | The only references from other TUs are in test sources. The message names one. |
| `Internal_Unproven` | note | What would be `Internal_Unreferenced`, but the facts do not cover the whole program (see [Coverage](#coverage)). |

The finding sits at the symbol's definition. Its identity (the
fingerprint input) is the symbol's USR.

## Why no other tool catches this

Tier 1 overlaps `-Wmissing-prototypes` and
`-Wmissing-variable-declarations` (Clang; GCC has
`-Wmissing-declarations`), which flag an external function or variable
defined without a previous declaration. Where those warnings are on,
most tier-1 findings are already warnings. The warnings see one TU, so
they differ in two ways:

- they fire whether or not another TU uses the symbol. A function that
  another TU declares for itself (`int helper();` in `b.cpp`) and calls
  is warned about in `a.cpp`, and making it `static` breaks the link;
  this check does not report it;
- a forward declaration earlier in the same file silences them; it does
  not change the verdict here.

Tier 2 and `Internal_TestOnly` are out of their reach: a header
declaration is the prototype they look for. "No other TU references
it" needs every TU's references, which is what the megascope reference
facts hold ([symbol-references.md](../symbol-references.md)).

## What is reported

A symbol is a candidate when all of these hold:

- it is a function or a variable with static storage duration, with
  linkage class `external` (one strong definition: not inline, not a
  template or instantiation, not already internal);
- exactly one TU defines it, in that TU's main file (a definition in a
  header or a `.inc` file is left alone: every includer has a copy);
- none of these exclusions applies: `main` and the names given with
  `--entry-point`; class members (their linkage follows the class);
  `extern "C"`; `dllexport`, `dllimport`, an explicit `visibility`
  attribute, `used` / `retain` / `constructor` / `destructor`; `weak`
  or an alias; explicit instantiations and explicit specializations;
  virtual functions and lambdas; a replacement `operator new` /
  `operator delete`.

A reference is from another TU when a TU other than the definer holds
it (`tuCount` greater than one, or a single TU that is not the
definer). Every role counts: a call, taking the address, a read, a
write.

### Test sources

A reference is test code when its witness site is in a test source, or
when one TU holds it and that TU is a test source. A test source is a
path matching `--test-path-re` (POSIX extended regex); the default
matches a `test`, `tests`, `testing`, `unittest` or `unittests`
directory component, a `test_` file prefix, a `_test`, `-test`,
`_unittest` suffix, and `FooTest.cpp` / `FooTests.cpp`. An empty
pattern (`--test-path-re=`) turns test detection off, and every
reference counts. References to a symbol defined in a test source
(test-support code shared by several tests) are ordinary uses. A reference whose witness is in a header that several
TUs include counts as production code even when every one of those TUs
is a test: the facts keep the smallest contributing TU, not the list.

## Coverage

"No other TU references it" is a claim about the whole program, so it
needs facts from every TU. The verdict is downgraded, with the reason
appended to each message (`Not proven: ...`), when:

- the analyzed sources are fewer than the C/C++ entries of the
  compilation database (or the database cannot list them);
- a TU did not parse (crashed, timed out, skipped), or parsed with
  errors.

Tier 1 then reports `Internal_Unproven` (a note); the other findings
keep their kind and carry the reason. A program whose compilation
database also holds tools, tests or other binaries is the usual case:
analyze the whole database for a proven answer.

## Where the facts come from

The check reads the reference facts of the megascope index. When the
index at `--index` (default `<build-path>/.vycor/megascope.vycs`) holds
exactly the selected sources and none is stale, its references section
is read (no parse; `--verbose` says so). A TU whose unchanged inputs
parsed with errors does not count as stale here: a bake would record
the same partial facts, and the coverage reason names it. Otherwise
the check bakes the sources in memory,
one parse per TU; with `dead-code` or `static-init-hazards` enabled too,
they share that bake.

## Limits — known false positives

- **References the facts do not hold.** Bodies of implicit template
  instantiations are not traversed, so a function reached only by
  argument-dependent lookup from a template instantiated in another TU
  looks unreferenced. Such a function is normally declared in a header
  (a note). References from macros that expand to nothing the parser
  sees, from inline assembly, and from linker scripts are not seen.
- **One build configuration.** The facts come from one compile of each
  TU. A declaration or a use under `#ifndef NDEBUG`, inside `assert`,
  or behind a feature macro does not exist when that configuration
  compiles it out. On LLVM's release build, `isCurrentDebugType` (its
  `Debug.h` declaration and every use sit under `#ifndef NDEBUG`) is
  tier 1, and functions called only from `assert`s look unreferenced.
  Analyze the configuration whose answer you want, or several.
- **Generated sources.** A definition in a generated `.cpp` is reported
  where it lands; the fix belongs in the generator.
- **Lookup by name at run time.** A symbol found with `dlsym`,
  `GetProcAddress`, or a registration table built from strings has no
  reference in the source. Mark it `extern "C"`, `used`, or with an
  explicit visibility attribute, or list it with `--entry-point`.
- **Code outside the analyzed program.** A library's public API used
  only by its clients is tier 2 when a header declares it; analyze the
  clients too, or treat tier 2 as a review list. A variable that the
  client redeclares for itself (`extern cl::opt<bool> Flag;` in the
  client's `.cpp`, a common LLVM idiom) has no header declaration and
  is tier 1; analyzing a part of the database makes it
  `Internal_Unproven`, which is why that kind exists.
- **Two internal symbols of one name in files of one basename** share a
  USR (a known gap of the facts); this does not affect external
  symbols, which are all the check reports.

The check does not report classes defined in a source file outside an
anonymous namespace (an ODR hazard with the same remedy).
