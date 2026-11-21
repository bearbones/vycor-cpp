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
| `lock` | an exclusive lock acquisition that waits (target: the lock identity, see "Lock facts") |
| `lock_shared` | a shared lock acquisition that waits: `lock_shared`, a `std::shared_lock`, a reader lock |

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
`operator delete` it uses (through a virtual destructor of a class that
is not final, one virtual call: the destructor, and no `operator
delete` unless it is `::delete`, since the deleting destructor the
vtable holds calls it); `__attribute__((cleanup(fn)))` calls `fn`. A
constructor inherited by a `using` declaration builds the derived
class's other bases and members and calls the base class's
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
  behind `std::make_unique<T>()` or `emplace_back` are recorded. The
  same holds for the initializer of a variable template's or a class
  template's static data member's instantiation, and for the default
  arguments an instantiation's callers use. An explicit instantiation
  definition — of a class (the members of its nested classes too) or
  of one member — reads those bodies the same way.
- **Lambdas an instantiation copies.** The call operator of a generic
  lambda, and a lambda in the body of a template's instantiation, are
  read under the lambda as written: it stays the referrer, not the
  function that calls it or the template it is passed to. A local
  class's member in an instantiation is read when it is called, as an
  instantiation of its own pattern.
- **Implicit special-member calls.** The constructor and destructor
  calls of members, bases and temporaries, the destructor at the end of
  a scope (locals, static locals, globals, a thrown exception object),
  an implicit destructor behind `delete` (a trivial one is no call),
  and `operator new`/`operator delete` behind an expression (a virtual
  destructor records the one its deleting form calls). With exceptions
  enabled, the cleanup an exception runs: a constructor that can throw
  destroys each base and member built before an initializer or a body
  that can throw (a delegating constructor's body, the whole object); a
  `new` whose initializer can throw records the `operator delete` that
  frees the storage, and an array `new` the element destructor.
- **Transparent functions.** A function that is not a symbol of its own
  — an implicitly defined special member, an inheriting constructor,
  the `operator==` a defaulted `operator<=>` declares, an instantiation
  of a system-header template (`std::make_unique`,
  `std::vector::emplace_back`) — is read once per TU,
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
| `attributes` | any of `extern_c`, `visibility` (explicitly exported: a visibility other than hidden written on the declaration, its class or namespace, or by a pragma; not the command line's default), `dllexport`, `dllimport`, `used` (also `retain`, `constructor`, `destructor`), `weak` (also `alias`, `ifunc`), `member`, `static_local`, `thread_local`, `const`, `atomic`, `dynamic_class`, `main`, `explicit_instantiation`, `virtual`, `lambda`, `explicit_specialization` (an explicit specialization of a function, variable, or class template; a symbol of its own, separate from the primary template), `sync_primitive` (a variable of a mutex, condition variable, once flag, semaphore, latch, barrier or stop source type, or an array of one), `in_loop` (a lambda written inside a loop body) — in this order |
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

### Lock facts

Each reference record also carries the locks held where its occurrences
are written, for analyses that ask whether two accesses share a lock
([unsynchronized-shared-global](checks/unsynchronized-shared-global.md)),
and a flag byte: `in_loop` when an occurrence is inside a `for`,
`while`, `do` (not a `do ... while (0)`) or range-`for` statement of the
referrer, and, on a `write`,
`mutates` when some occurrence changes the object for certain. A
`write` without `mutates` only calls non-const member functions that
have a const overload taking the same parameters and return by value or
by const reference or pointer, not a write proxy (a class assignable
from another type, such as `std::vector<bool>::reference`); a non-const
`std::map`'s `find`, `begin` and `end` are such calls:
the call does not change the object, though it may later be changed
through what it returns. The lock facts are a
text, one line per distinct lock set in order of first occurrence:

```
<line>:<col>[\t<identity>\t<display>]...
```

`line:col` is the first occurrence under that set; each lock follows as
its identity and display name, sorted by identity. A line with no lock
is an occurrence under no lock beside others under some; a record none
of whose occurrences holds a lock has no text at all. The identity is
the USR of the mutex variable or data member, `?` for a mutex the bake
cannot name (through a pointer, a reference, a call, an array element, a
variable or member of pointer or reference type, or a lock type
constructed from an argument that is not read as a mutex or handed over
from elsewhere),
and `once:` + USR for the once guard around a `std::call_once` callable
(the flag) or a function-local static's initializer (the variable).

What holds a lock: a scoped guard (the bake's lock types:
`std::lock_guard`, `std::scoped_lock`, `std::unique_lock`,
`std::shared_lock`, the configured allowlist, a `scoped_lockable`
class, or a class outside namespace `std` with a non-trivial destructor
constructed from a mutex or a pointer to one) from its declaration to
the end of its block, with `std::defer_lock` and the guard's `lock` /
`unlock` calls followed (`release` hands the mutex over still locked);
a mutex's own `lock` / `unlock` family, the capability attribute
functions, `std::lock` (on mutexes or deferred guards), and the pthread
lock calls, in statement order. Each branch of an `if` or `switch`
starts from the state before it, and after a branch, switch or loop a
lock is held when every way out that falls through holds it (a branch
ending in a jump does not count; a different lock on each way out
leaves an unknown lock held, until the outermost of those guards
ends or an unlock of a mutex that is not held); a label a forward `goto` reaches starts
from the states at those gotos; a block drops its own guards. A
`std::call_once` callable written in the call as a lambda, or named as a
function, runs under the flag's once guard. A lambda starts with no lock
held; when it is created under one, the record of the enclosing
function's `call` of the lambda carries that set, and each call of a
local holding the lambda (named as an argument of a call: `l()`,
`std::invoke(l)`, `std::sort(b, e, l)`) is a `call` record under the
locks held there; a copy into another variable, a capture or a return
is not. Lock state never crosses a function
boundary: the joins along call paths are the consumer's.

#### Lock acquisitions

A `lock` or `lock_shared` record is one lock acquisition by its
referrer: its target is the lock's identity (the text above, so a
member mutex's target is the member's USR and has no symbol of its own;
`?` for a mutex the bake cannot name), its lock text the locks held
*before* the acquisition, one line per distinct set as for any record.
An acquisition is recorded where the code waits for the lock: a scoped
guard's declaration (each mutex of a `std::scoped_lock` under the locks
held before it, so none of them orders another), a deferred guard's
`lock()`, a mutex's `lock` / `lock_shared`, the capability attribute
functions, each argument of `std::lock` (under the set before the
call), and `pthread_mutex_lock`, `pthread_spin_lock`,
`pthread_rwlock_wrlock` (`lock`) and `pthread_rwlock_rdlock`
(`lock_shared`). A try or timed acquisition (`try_lock`, `try_lock_for`,
`std::try_to_lock`, `std::adopt_lock`, `std::try_lock`, the pthread
`try` and `timed` calls) gives up instead of waiting and is no record.
Shared is read from the thread-safety attribute when there is one, else
from the spelling (`shared` in a member's name, a `Reader` prefix,
`std::shared_lock` and guard types named like a reader lock). Once
guards are not recorded, and neither is anything library code or code
the compiler writes acquires. Two more flags apply: `recursive` when
the mutex type is recursive (`std::recursive_mutex`,
`std::recursive_timed_mutex`, a class named like one or holding one),
`non_recursive` when it is known not to be (`std::mutex`,
`std::timed_mutex`, `std::shared_mutex`, `std::shared_timed_mutex`,
`absl::Mutex`); neither when it is not known (a pthread mutex takes its
kind at run time). The records are read by
[lock-order-inversion](checks/lock-order-inversion.md).

An instantiation's body (read after the code as written, see "Code the
compiler writes") starts with no lock held; a reference only
instantiations hold carries the lock sets and flags of all of them, and
one the template as written holds keeps its own. A reference of a
transparent function takes, once spliced into a user, the lock sets and
`in_loop` flag of that user's reference to the transparent function, and
its own `mutates` flag; spliced into a reference the user's code holds
itself, it adds those facts and keeps that reference's count and site.

### Emission facts

Which function bodies each TU's code generation emits: how many TUs
compile one body (the
[redundant-instantiation](metrics/redundant-instantiation.md) metric),
and whether an inline body is code in a TU at all. A header defines far
more inline functions than any one TU uses; a reference recorded in a
body no TU emits is in no object.

A TU **emits** a function when Clang's CodeGen would hand its body to
the optimizer. The bake walks the TU's emission graph the way CodeGen
decides what to emit (`callgraph/EmissionGraph.h`, shared with the
[isa-leak](checks/isa-leak.md) check): from what the TU must emit on
its own — strong definitions, the global variables
`ASTContext::DeclMustBeEmitted` keeps, the vtables of classes whose key
function the TU defines, explicit instantiation definitions, `used` and
`constructor` functions — through every function an emitted body
references. A body is walked as CodeGen lowers it:

- unevaluated operands (`sizeof`, `decltype`, `noexcept`, a
  requires-expression, a non-polymorphic `typeid`), a value the
  compiler computed (a constant expression, an immediate invocation, a
  `consteval` function), the discarded branch of `if constexpr`, and a
  branch or loop a constant condition never enters are no code;
- a constant initializer (a `constexpr` variable, a static local or
  global the compiler initializes as data) runs nothing, but the
  functions and globals whose addresses its value holds are emitted,
  and an object of a dynamic class without a key function in it emits
  that class's vtable;
- a virtual call reaches its target only where CodeGen devirtualizes it
  (a `final` class or method, a qualified call, an object whose dynamic
  type is known); a constructor or vptr-storing destructor of a class
  without a key function emits its vtable and every function in its
  slots.

Of the functions reached, a record is kept for each with a body whose
code-generation linkage is vague or internal to a header:

| Linkage (`GVALinkage`) | Recorded | `flags` |
|---|---|---|
| `DiscardableODR`: inline functions, functions defined in a class, implicit instantiations, implicitly defined members | yes | — |
| `AvailableExternally`: an inline function an `extern template` declaration covers, kept only for inlining | yes | `available_externally` |
| `Internal`: a `static` or anonymous-namespace function written in a header (not the TU's main file) | yes | `internal` |
| strong (`StrongExternal`, `StrongODR`), or internal to the main file | no: one copy, where it is written | |

A trivial special member (a defaulted constructor, assignment or
destructor that copies bytes or does nothing) is not recorded: CodeGen
lowers its uses to a copy or to nothing and emits no body (the rare
trivial assignment operator whose address is taken does get one, and is
missed). Anything
declared in a system header is left out, so the facts do not depend on
the host's standard library.

Each record holds:

| Field | Meaning |
|---|---|
| `name` | the linkage name (mangled; the base-object variant of a constructor or destructor, the one CodeGen always emits), so each instantiation is a record of its own |
| `symbol` | the USR the reference facts identify it by: the function, or the template pattern of an instantiation. It has a symbol record only when the reference facts keep one (a pattern only system code instantiates has none); none for a function the reference facts never identify |
| `file`, `line` | its definition |
| `size` | statements and expressions CodeGen lowers for the body: the nodes of its syntax tree the walk visits, implicit ones included, discarded code excluded. A size proxy (redundant-instantiation, "Reading the numbers") |

Records are deduplicated by content across TUs, like the other records,
with the TUs that emit each one. The section also lists every TU whose
emission facts were recorded, even one that emits nothing, so a reader
can tell "emits nothing" from "not recorded" (a TU baked by an older
version, or by a frontend plugin that records none).


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
               "vtable_use": 0, "typeinfo_use": 0, "lock": 0,
               "lock_shared": 0},
 "referencedFromOtherTu": true,
 "status": "ok", "indexScope": {...}}
```

`referrers` counts distinct referrers per role (a function that reads
and writes the symbol counts once under each). `lock` and `lock_shared`
count the functions acquiring a mutex variable (format v18); a data
member mutex has no symbol of its own, so its acquisitions show only in
`get_references`.

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
v15; v16 adds each record's largest contributing TU; v17 adds the lock
facts and the flag byte to each reference record; v18 the lock
acquisition records and their recursion flags; v20 the emission
facts), with its own
checksum, decoded only by a load that asks for it.
The header's summary carries the symbol and reference counts, which
`megascope info` and the `index` summary line report as `symbols` and
`references`.

Records are flat structs over interned strings and are deduplicated by
content, each with the list of TUs that contributed it: an inline
function in a header that 900 TUs include is one record with 900
contributors (stored as runs). That list is what makes a warm refresh
exact — re-indexing a TU removes its contributions and nothing else —
and what `megascope merge` uses to combine per-TU shards, dropping the
duplicate of a TU that two inputs both hold. Each record also stores its
contributor count and its smallest contributor by path, and a reference
its largest too, so a load that skips the lists still knows them: the
dependency audit scopes an inline body's reference to one target set
when every TU between those two belongs to it
(`docs/dependency-audit.md`, "Inline bodies and the closure").

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
arrays, `symbols` and `references`, beside the call-graph members; a
reference has `locks` (the lock text), `in_loop: true` and
`mutates: true` only when they are set. It does not carry the emission
facts. A
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

Reading instantiation bodies and the code the compiler writes is part
of that traversal, always on. Measured the same way (without it, with,
with, without), it adds no cost the host's noise lets one see: wall
371.8 s and 329.0 s without, 317.5 s and 318.7 s with; CPU 1339 s and
1190 s without, 1137 s and 1138 s with; peak RSS 2.727 GB against
2.735 GB (+0.3%); index 573.5 MB against 575.2 MB (+0.3%). One TU
(`lib/Analysis/ScalarEvolution.cpp`), interleaved, best of five: 3.83 s
without, 3.70 s with. It adds 22% references and 4% symbols. Storing
each reference's largest contributor ("Storage") costs about one byte
per reference, 0.1% of an 892-TU index of LLVM's libraries and tools.

The section holds about 56,700 symbols and 291,000 references. A one-shot
`lookup-symbol` or `get-references` over that index takes about 60 ms and
66 MB (process start, header and checksum, the references section
alone); graph queries cost what they did.

The lock facts (format v17) were measured the same way against the
revision before them, interleaved (without, with, with, without), on a
host shared with other builds (load 4 to 10):

| | Without lock facts | With them | Change |
|---|---|---|---|
| Cold bake, wall | 6:30.2, 6:05.4 | 6:12.8, 6:32.8 | +1.3% (noise) |
| Cold bake, CPU (user) | 1411 s, 1328 s | 1336 s, 1382 s | −0.8% (noise) |
| Peak RSS of the largest process | 2.72 GB | 2.73 GB | +0.1% |
| Index size | 573.51 MB | 574.05 MB | +0.09% (0.54 MB) |

The run-to-run spread (25 s wall, 83 s CPU between two runs of one
binary) is larger than any difference between the binaries. Lock state
is a small vector of held locks per function body; a reference records
a lock set only when one is held, and lock sets are interned per TU.

The emission facts (format v20) were measured with one binary, the
facts switched off and on, interleaved (off, on, on, off), on a host
shared with other builds (load 3 to 4.5):

| | Without emission facts | With them | Change |
|---|---|---|---|
| Cold bake, wall | 5:57.8, 5:52.2 | 5:56.4, 5:18.0 | −5.0% (noise) |
| Cold bake, CPU (user) | 1290 s, 1251 s | 1268 s, 1136 s | −5.4% (noise) |
| Peak RSS of the largest process | 2.743 GB | 2.754 GB | +0.4% |
| Index size | 576.44 MB | 583.06 MB | +1.15% (6.62 MB) |

The spread between two runs of one setting (38 s wall, 132 s CPU)
hides the difference, so the CPU cost was measured per TU, in process,
interleaved, best of five: `lib/Analysis/ScalarEvolution.cpp` 3.12 s
without, 3.15 s with; `lib/Transforms/Scalar/GVN.cpp` 2.63 s and
2.65 s; `lib/IR/Verifier.cpp` 2.41 s and 2.43 s;
`lib/Support/CommandLine.cpp` 0.92 s and 0.94 s: +1.0% CPU over the
four, +0.2% peak RSS. The emission walk is one more traversal of the
function bodies the TU emits, over flat per-function arrays, and the
records are lowered once per TU.

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
  when there are at most 16, and nothing beyond that. An instantiation
  that selects one of them leaves the same record (same referrer, target
  and site), so nothing marks which candidate was selected.
- **Instantiations no TU makes.** Only the instantiations a TU uses are
  read. A template that no indexed TU instantiates holds only what its
  pattern names. An instantiation the TU uses but does not define (the
  template is only declared there) is read in the TU that defines it,
  if any; its default arguments are not read. An explicit instantiation
  of a static data member is read only where the member is used. A
  default argument of a transparent function (a system template's) is
  recorded in each of its callers, also one that passed the argument.
- **One pattern for all instantiations.** A template's references are
  the union over every instantiation the index read: `make<T>()` used
  with `A` in one TU and `B` in another references both constructors.
  A lambda in a template's body is a referrer whether or not anything
  calls it.
- **Exception paths, judged from the form of the code.** An initializer
  cannot throw when it is a literal, a name, value-initialization, or a
  trivial or non-throwing construction from such operands; anything
  else, a call to a `noexcept` function included, is taken to throw, and
  so is any non-empty constructor body. A cleanup the compiler proves
  unnecessary may therefore be recorded. A TU compiled with
  `-fno-exceptions` records none.
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
- **Default arguments** are attributed where they are written (an
  instantiation's, to the template), not to each call that uses them.
- **Conservative roles.** A non-const member call or non-const
  `operator[]` on a global is a `write` whether or not it mutates (the
  `mutates` flag of the [lock facts](#lock-facts) narrows it);
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
