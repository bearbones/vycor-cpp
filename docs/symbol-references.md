# Symbol reference facts

The call graph answers "who calls whom". The reference facts answer
"who references what, and how" for every entity that can have linkage:
functions, variables with static storage duration, and classes. They
are recorded by the same bake (`megascope index`, one frontend parse per
TU), stored in their own section of the index, and answered by two
tools, `lookup_symbol` and `get_references`.

They exist for the questions a call graph cannot answer: could this
function be `static`, who reads or writes this global, does anything in
another TU depend on this symbol, which TU emits this class's vtable.

Code: `callgraph/ReferenceIndex.h` (the index),
`callgraph/ReferenceVisitor.h` (the bake visitor),
`query/ReferenceTools.cpp` (the tools). The envelope every payload
carries is `docs/result-contract.md`; the order contract is
`docs/deterministic-output.md`.

## What is recorded

### References

One record per **(referrer, target, role)**: the function, variable
initializer, or file scope a reference appears in; the function, global
variable, or class it names; and what the reference does. A record
carries the number of occurrences inside the referrer (`count`) and ONE
witness site (the first occurrence in source order), not one record per
site, so the section is bounded by the number of distinct pairs.

| Role | Recorded for |
|---|---|
| `call` | the callee of a call, including constructor and destructor calls the compiler writes (see "Code the compiler writes") and `operator new`/`operator delete` behind a `new` or `delete` expression |
| `address_of` | `&x`, function-to-pointer and array-to-pointer decay, binding to a non-const reference (a reference variable, or an argument for a non-const reference parameter), a pointer to member: any use that lets the entity be reached through an alias |
| `read` | a value read: lvalue-to-rvalue conversion, a const member call on the object, binding to a const reference, naming a static member through the object (`obj.staticMember`) |
| `write` | assignment, compound assignment, `++`/`--`, a non-const member call or non-const `operator[]` on the object |
| `vtable_use` | constructing an object of a dynamic class (target: the class), and a constructor or destructor body of one (it stores the vtable pointer): a user-provided one records it itself, an implicit one in its user, so constructing a derived object also uses each dynamic base's vtable |
| `typeinfo_use` | `dynamic_cast` to the class, `typeid`, `throw` of the class type, a `catch` handler for it |

On a scalar, `x += 1` and `++x` are one `read` and one `write`. On an
object of class type they are calls of an operator function and follow
the call rules: a non-const member operator is a `write` of the object
(and no `read`), a free operator taking it by non-const reference an
`address_of`. A call through a function pointer names no function and
records nothing; taking the function's address where the pointer was
made does (`address_of`).

A use of a variable that none of the rules above classifies is recorded
as `address_of`, the role that claims least: the variable is named and
may be reached by whatever it was handed to. That matters in a template
body, where the types that would decide are not known yet. There the
spelling decides what it can: an assignment or `++`/`--` is a `write`;
an operand of an arithmetic or comparison operator, a subscript index, a
value cast to a non-reference type, a value returned from a function
that returns by value, and the initializer of a scalar or `auto`
variable are `read`s; a member call is a `read` when every candidate is
a const member function and a `write` otherwise. Everything else (an
argument of a call the pattern cannot resolve, the left operand of
`<<`) is `address_of`.

Syntax the compiler expands is recorded as the calls it stands for: a
range-based `for` calls the `begin`, `end`, `operator!=`, `operator++`
and `operator*` it uses; a comparison rewritten from `operator<=>` or a
reversed `operator==` calls that operator; a structured binding through
`get` calls it; `delete p` calls the class's destructor and the
`operator delete` it uses; `__attribute__((cleanup(fn)))` calls `fn`. A constructor
inherited by a `using` declaration is a call of the base class's
constructor. A C function called without a declaration is a symbol like
any other, declared at the call.

The referrer of a reference inside a lambda body is the lambda, under
the same synthetic identity the call graph gives it. References outside
any function or variable initializer (default member initializers, for
instance) are attributed to the file, a referrer of kind `file`; a
default member initializer is also read in each constructor or
aggregate initialization that uses it.

### Code the compiler writes

The visitor reads the code as written first, then what the compiler
generates from it, and records the second as more references of the
written code:

- **Template instantiations.** The body of each instantiation a TU
  uses is read, and what it references is attributed to the template
  pattern, the same referrer the code as written has. A record the
  pattern already holds is not repeated, and one that only an
  instantiation reveals carries the largest count any single
  instantiation has, so the index does not grow with the number of
  instantiations. This is how a call resolved by argument-dependent
  lookup at instantiation time, an explicit specialization that only
  instantiations select, and the constructor call and `vtable_use`
  behind `std::make_unique<T>()` or `emplace_back` are recorded. An
  explicit instantiation definition reads its members' bodies the same
  way.
- **Implicit special-member calls.** The constructor and destructor
  calls of members, bases and temporaries, the destructor at the end of
  a scope (locals, static locals, globals, a thrown exception object),
  an implicit destructor behind `delete`, and `operator new`/`operator
  delete` behind an expression (a `new` also records the `operator
  delete` that frees the storage if the constructor throws; a virtual
  destructor records the one its deleting form calls).
- **Transparent functions.** A function that is not a symbol of its own
  — an implicitly defined special member, an instantiation of a
  system-header template (`std::make_unique`, `std::vector::emplace_back`),
  a generic lambda's instantiated call operator — is read once per TU,
  and what it references is recorded in each referrer that uses it,
  transitively. `std::make_unique<Impl>()` in `build()` records
  `build → Impl::Impl` and `build → vtable_use Impl`, with the site of
  the `make_unique` call.
- **Virtual functions held by a vtable.** A class whose vtable is used
  references what its implicit virtual destructor does (a vtable slot
  holds it whether or not anything deletes through it), and the
  virtual member functions of a class template instantiation are read
  as instantiations.

None of this changes a record's role or confidence: a reference found
in an instantiation or a transparent function is one the compiled code
makes, recorded like one written in the referrer.

### Symbols

For each function, global variable, and class that is a referrer or a
target, plus every one defined in a TU's main file whether referenced or
not:

| Fact | Values |
|---|---|
| `kind` | `function`, `variable`, `class` (and `file` for a file-scope referrer) |
| `linkage` | `external` (one strong definition), `template_instantiation`, `vague` (inline, defined in class, a class without a key function, a static local of an inline function), `internal` (`static`, anonymous namespace, function-local), `none` (a file scope) |
| `attributes` | any of `extern_c`, `visibility` (explicitly exported: a visibility other than hidden written on the declaration, its class or namespace, or by a pragma; not the command line's default), `dllexport`, `dllimport`, `used` (also `retain`, `constructor`, `destructor`), `weak` (also `alias`, `ifunc`), `member`, `static_local`, `thread_local`, `const`, `atomic`, `dynamic_class`, `main`, `explicit_instantiation`, `virtual`, `lambda`, `explicit_specialization` (an explicit specialization of a function, variable, or class template; a symbol of its own, separate from the primary template) — in this order |
| `declarations` | every declaration some indexed TU saw: `file` (absolute, `.` and `..` removed, so one file has one name however it was included), `line`, `definition`, `inHeader` (not in that TU's main file), and the TUs that saw it (`tuCount`, `firstTu`) |
| `definingTu`, `definingTuCount` | the TUs holding a definition. A strong external symbol has one; an inline function has one per TU that recorded it, and `definingTu` is the smallest path |
| `vtableHomeTu` | for a dynamic class, the TU that defines its key function and therefore emits the vtable and typeinfo strongly |
| `bodyLines` / `sizeBytes` | a function's body in source lines; a variable's or class's object size in bytes |

When TUs disagree about a symbol's linkage the merged view reports the
strongest class seen (the order of the table row above); attributes are
the union.

Of the standard library and other system-header code only strong
external symbols are kept (`malloc`, `std::exception`'s typeinfo): those
are link dependencies. Inline and template code from system headers is
not recorded as a target.

## Tools

Both tools read the references section alone (`megascope tools --format
json` lists `needs: ["references"]`), so a query does not decode the
call graph or the call-site contexts.

A symbol is named by `name` (its qualified name; a function-local static
is `function::name`) or `usr`. Several symbols with one name answer
`{"ambiguous": true, "candidates": [...]}` (exit 4), each candidate with
its `usr`, `kind`, `linkage`, and declaring `file`/`line`; `filter`
narrows by a literal substring of a candidate's usr, name, or declaring
file. A name the index does not hold is `not_found` (exit 1).

### `lookup_symbol`

```bash
megascope lookup-symbol --name ns::counter
```

```json
{"qualifiedName": "ns::counter", "usr": "c:@N@ns@counter",
 "kind": "variable", "linkage": "external", "attributes": [],
 "sizeBytes": 4, "declaredInHeader": true,
 "definingTuCount": 1, "definingTu": "/src/store.cpp",
 "declarations": [
   {"file": "/src/api.h", "line": 2, "definition": false,
    "inHeader": true, "tuCount": 2, "firstTu": "/src/main.cpp"},
   {"file": "/src/store.cpp", "line": 3, "definition": true,
    "inHeader": false, "tuCount": 1, "firstTu": "/src/store.cpp"}],
 "referrers": {"call": 0, "address_of": 0, "read": 3, "write": 2,
               "vtable_use": 0, "typeinfo_use": 0},
 "referencedFromOtherTu": true,
 "status": "ok", "indexScope": {...}}
```

`referrers` counts distinct referrers per role (a function that reads
and writes the symbol counts once under each).

### `get_references`

```bash
megascope get-references --name ns::counter                    # who references it
megascope get-references --name ns::counter --roles write      # who writes it
megascope get-references --name main --direction from          # what main references
```

| Parameter | Meaning |
|---|---|
| `name` / `usr` / `filter` | the symbol, as above |
| `direction` | `to` (default): who references the symbol. `from`: what the symbol (a function body, a variable's initializer) references |
| `roles` | keep only these roles (repeat the flag); default all |
| `limit`, `offset` | paging (`docs/result-contract.md`, "Paging"); the default limit is 200 |

A `to` record:

```json
{"referrer": "bump", "referrerUsr": "c:@F@bump#", "referrerKind": "function",
 "role": "write", "count": 1,
 "file": "/src/store.cpp", "line": 9, "col": 5,
 "tuCount": 1, "firstTu": "/src/store.cpp", "fromOtherTu": false}
```

A `from` record has `target`, `targetUsr`, `targetKind` in place of the
referrer members and no `fromOtherTu`. The payload also carries the
resolved `qualifiedName`, `usr`, `kind`, the `direction`, the paging
members, and for `to`, `referencedFromOtherTu` — over the records of the
chosen `roles`, so `--roles write` answers "is it written from another
TU". The list member is `references`; an empty list exits 1.

### Referenced from another TU

`fromOtherTu` (per record) and `referencedFromOtherTu` (per symbol) are
defined only for a symbol with **exactly one** defining TU. A reference
comes from another TU when more than one TU contributed it, or the one
that did is not the defining TU. A symbol defined in several TUs (inline
code, a class in a header) or in none that was indexed has no such
member: the absence is "not determined", never "no".

Both are statements about the TUs the index holds. A symbol no indexed
TU references may still be used by code outside the selection; check
`indexScope` for how much of the selection parsed.

## Order

`get_references` orders its records by (referrer name, referrer usr,
target name, target usr, role); roles order as in the table above.
`lookup_symbol`'s declarations order by (file, line, definition), and
the candidates of an ambiguous name by usr. Like every list the tools
emit, the order does not depend on the TU order of the bake, the thread
count, or whether the index was built cold, refreshed warm, or merged
from shards (`tests/test_determinism.cpp`,
`scripts/warm-refresh-check.py`).

## Storage

The facts are the fifth section of the index (`references`, format
v15), with its own checksum, decoded only by a load that asks for it.
The header's summary carries the symbol and reference counts, which
`megascope info` and the `index` summary line report as `symbols` and
`references`.

Records are flat structs over interned strings and are deduplicated by
content, each with the list of TUs that contributed it: an inline
function in a header that 900 TUs include is one record with 900
contributors (stored as runs). That list is what makes a warm refresh
exact — re-indexing a TU removes its contributions and nothing else —
and what `megascope merge` uses to combine per-TU shards, dropping the
duplicate of a TU that two inputs both hold.

The section is written in a canonical order, so two indexes holding the
same facts are byte-identical in it whatever order their TUs were baked
in.

The decoder treats the bytes as untrusted and bounds what they can make
it allocate: the string table (front-coded) may expand to at most 64
times the bytes read so far plus 64 KiB, and one contributor run covers
at most 64 records. The writer stays inside both (it stores a string
whole when sharing a prefix would exceed the first, and splits a longer
run). A read-only load (the query verbs) skips the contributor lists
without checking them, since no query reads them; the section checksum
still covers them, and a mutable load (`index`, `serve`, `merge`)
checks every list against its records. An index loaded read-only
cannot be saved.

Other shard formats: a `.shard.json` shard carries the facts as two
arrays, `symbols` and `references`, beside the call-graph members. A
`.mgsc` snapshot does not carry them (its schema has no slot); the
reference tools answer `unavailable` over one.

### Cost

Measured on the llvm-project testbed (938 TUs of `lib/`, `--threads 4`,
cold bake with subprocess workers), against the same revision without
the section, runs interleaved (without, with, with, without):

| | Without the section | With it | Change |
|---|---|---|---|
| Cold bake, wall | 5:09.6, 5:07.7 | 5:32.4, 5:33.1 | +7.8% |
| Cold bake, CPU (user) | 1110 s, 1100 s | 1170 s, 1201 s | +7.3% |
| Peak RSS of the largest process | 2.64 GB | 2.72 GB | +3.3% |
| Index size | 560.1 MB | 572.0 MB | +2.1% (11.9 MB) |

The host was shared during these runs, so the wall and CPU figures carry
a few percent of noise: on 227 TUs (`lib/Support`, `lib/IR`) the same
interleaving measured +2.3% wall and +2.2% CPU, and one TU parsed and
traversed in process, best of seven, +1.8%. Loading and saving the
finished index costs the same CPU with and without the section.

The section holds 54,524 symbols and 239,018 references. A one-shot
`lookup-symbol` or `get-references` over that index takes about 60 ms and
66 MB (process start, header and checksum, the references section
alone); graph queries cost what they did.

The visitor is a fourth traversal of the AST the bake already holds; an
occurrence is one hash-map increment keyed by (referrer, target, role),
with no string built and nothing allocated per occurrence. USRs, names,
and attributes are computed once per entity when the TU's facts are
lowered into the index.

## Limits

The visitor reads the code as written and the code the compiler writes
from it in the same TU ("Code the compiler writes").

- **Unresolved overload sets.** An overload set a template pattern
  cannot resolve yet records an `address_of`/`call` to each candidate
  when there are at most 16, and nothing beyond that; an instantiation
  the TU uses adds the overload it actually selects.
- **Instantiations no TU makes.** Only the instantiations a TU uses are
  read. A template that no indexed TU instantiates holds only what its
  pattern names.
- **Compiler builtins and deduction guides** (`__builtin_expect`) are
  never targets; an implicitly defined special member is not a target
  either, its references are recorded in its users.
- **Unevaluated operands.** `sizeof`, `decltype`, `noexcept`, a
  requires-expression, a concept's constraint, and the non-polymorphic
  `typeid` name an entity without using it; no record.
- **Internal symbols in files of one name.** The identity of a symbol
  with internal linkage includes its file's name without the directory.
  Two `static void init()` in `a/util.cpp` and `b/util.cpp` therefore
  share one identity: their declarations, defining TUs and referrers
  are merged, and `definingTuCount` is 2.
- **Type names, enumerators, macros.** Only functions, variables with
  static storage, and classes (through their vtable and typeinfo) are
  targets: they are what a link resolves. Naming a class as a type, an
  enumerator, or a macro leaves nothing in the object code, so it is not
  a reference.
- **Default arguments** are attributed where they are written, not to
  each call that uses them.
- **Conservative roles.** A non-const member call or non-const
  `operator[]` on a global is a `write` whether or not it mutates;
  binding to a non-const reference is `address_of`.
- **Declarations** are recorded as seen by the TUs that define or
  reference the symbol; a TU that merely includes a declaration does
  not contribute it, so a declaration's `tuCount` is a lower bound on
  the TUs that can see it. Recording every included declaration would
  repeat each header's declarations once per including TU, which is
  most of the cost of an index.
- **Classes** are recorded by the TUs that define them in their main
  file, home their vtable, or reference their vtable or typeinfo.
- **`inHeader`** means "not in the TU's main file"; a textually included
  `.inc` or `.def` counts as a header.
- **`explicit_instantiation`** covers class and function templates, not
  variable templates.
- **Lambdas** appear as referrers only; their names embed the file
  position (`lambda#file:line:col#enclosing`), as in the call graph.

## Tests

| What | Where |
|---|---|
| each role, linkage class, attribute, and the visitor's attribution rules, template bodies and expanded syntax included | `tests/test_reference_index.cpp` |
| the index: dedup, per-TU removal, absorb, canonical encoding, decoding untrusted bytes, the decoder's allocation bounds | `tests/test_reference_index.cpp` |
| tool payloads, identity resolution, ambiguity, paging, the section round trip, selective load, damage detection, shard merge (disjoint and overlapping) | `tests/test_reference_tools.cpp` |
| the verbs, exit codes, section-only load, `info` counts | `tests/test_megascope_cli.cpp` |
| order under any TU order, thread count, isolated workers, and warm refresh | `tests/test_determinism.cpp`, `scripts/warm-refresh-check.py` |
| hand-written witnesses and negatives | `corpus/cases/symbol_references` |
| CLI output shapes | `scripts/cli-golden.py` (`lookup-symbol*`, `get-references*`) |
| the section decoder against arbitrary bytes | `fuzz/fuzz_snapshot.cpp` |
