# noexcept-candidate

**Default:** off · **Groups:** compute-heavy · **Diagnostics:** `Noexcept_Candidate`

Two sub-kinds, each its own check, off by default, and selected
independently of the main one:

| Check | Diagnostic | Reports |
|---|---|---|
| `noexcept-candidate` | `Noexcept_Candidate` | every proven function that is neither virtual nor a move operation |
| `noexcept-candidate-move` | `Noexcept_CandidateMove` | move constructors and move assignment operators |
| `noexcept-candidate-virtual` | `Noexcept_CandidateVirtual` | virtual functions |

Reports functions that provably cannot throw and are not declared
`noexcept`. It is the inverse of
[exception-escape](exception-escape.md): that check starts at a
`noexcept` function and looks for one throw it can reach; this one
reports a function only when it has looked at everything the function
can run and found no way for an exception to leave it.

Severity is `note`: a finding is a place where a declaration could say
more, not a defect.

## Why no other tool catches this

A compiler infers "cannot throw" for its own optimizer, per TU, and tells
nobody. A per-TU linter can only reason about callees whose bodies it can
see, so the moment a function calls something defined in another `.cpp`
the proof ends at a declaration. This check collects one fact per
function definition during the phase-1 parse and joins them across TUs,
so `a()` in `a.cpp` calling `b()` in `b.cpp` calling a leaf in `c.cpp` is
proven from the leaf up.

## What is proven

A function is reported when **every** definition of it that the run
indexed satisfies all of the following, and the definition, with the
declarations visible where it is written, does not already say it cannot
throw:

1. No `throw` in its body is outside a `try` block with a `catch (...)`
   handler (a rethrow is the exception its `try` raised, see below).
2. Every call it makes outside such a `try` block is to
   - a function declared non-throwing (`noexcept`, `throw()`,
     `__attribute__((nothrow))`, an implicitly non-throwing destructor),
     or
   - a function that is itself proven by these rules.
3. Nothing else in its body can raise an exception (the list below).

The proof is a greatest fixed point: every function starts as "proven"
and is struck out when one of its exits is not. Functions that call each
other in a cycle are therefore proven together when the cycle has no
other throwing exit, and refuted together when it has one.

**A callee with no indexed definition is unknown, never "cannot
throw".** A function that calls it outside a `catch (...)` is not
reported. This covers library functions without a `noexcept`
declaration, functions in TUs the run was not given, and anything the
collector could not resolve to a body.

A call is matched to a definition in another TU by the callee's linkage
name, the symbol the linker resolves (an `asm` label included), not by
its source-level identity: two overloads the linker keeps apart are
never taken for one. A function with internal linkage is matched within
its own file.

### What counts as a call

The collector walks the AST of each function body itself; it does not
read the megascope call graph, so that graph's documented gaps do not
apply. Besides ordinary calls, it accounts for:

- **Constructors and conversions**, including the ones a `new`
  expression, a temporary, a default argument, or a default member
  initializer runs. Default arguments and default member initializers
  are walked at the call that uses them.
- **Implicit destructor calls**: locals leaving scope, temporaries,
  by-value parameters destroyed by the callee, structured-binding
  holders, catch variables, and in a destructor the members and bases
  (virtual bases included). A destructor is non-throwing unless declared
  otherwise, so these rarely refute a proof; a `noexcept(false)`
  destructor does. A destroyed object of incomplete type is unknown.
- **`new`**: the replaceable global allocation functions can throw
  `std::bad_alloc` whichever implementation is linked, so a `new`
  expression that uses one refutes the proof. A `nothrow` form, or a
  class-specific `operator new` that is declared non-throwing or proven,
  does not, except that an array `new` whose size is not a constant
  throws `std::bad_array_new_length` on a bad size whenever its
  allocation function is not declared non-throwing. Array-size and
  placement arguments are walked.
- **`delete`**: the deallocation function and the destructor. Deleting
  through a pointer to a non-final class whose virtual destructor is not
  declared non-throwing is unknown, as is deleting an incomplete type.
- **Virtual calls**: a call to a virtual function whose type is
  `noexcept` or `throw()` adds no obligation (an override cannot have a
  looser specification). `__attribute__((nothrow))` does not count here,
  since overrides do not inherit it, and in a TU compiled with
  `-fms-compatibility` (where Clang accepts a looser override) nothing
  does. A qualified call (`Base::f()`) or one Clang can devirtualize (a
  `final` class or method, a known dynamic type) is a call to that one
  function. Every other virtual call is unknown: the proof does not
  enumerate overriders.
- **Calls through function pointers and member pointers** are unknown
  unless the pointee type is `noexcept` and the TU is C++17 or later
  (before C++17 `noexcept` is not part of the type, so the pointer can
  hold a function that throws).
- **Template instantiations, implicit and defaulted special members,
  lambdas, local-class members, and inline functions from system
  headers** have no fact of their own. Their bodies are walked in the TU
  that uses them and folded into the calling function, so the proof sees
  the instantiation that TU actually compiled. An explicit instantiation
  definition is exported as a fact, so a TU that only sees
  `extern template` resolves to it.
- **Lambdas**: creating one runs its capture initializers; its body is
  reached through its call operator. Converting a lambda to a function
  pointer and calling that pointer is unknown.
- **`thread_local` variables**: the first use of one on a thread goes
  through a wrapper that runs the dynamic initializers of every
  `thread_local` its TU defines, and registers its destructor. A use of
  a namespace-scope or static member `thread_local` is unknown unless
  its definition is visible, needs no destruction, and has a constant
  initializer (or is declared `constinit`).
- **`__attribute__((cleanup(f)))`** calls `f` when the variable leaves
  scope.
- **Immediate invocations**: a `consteval` call is evaluated at compile
  time, but a class object it produces is destroyed at run time.

### What else can throw

- `dynamic_cast` to a reference type, and `typeid` applied to a
  potentially evaluated glvalue of polymorphic type.
- Coroutines, blocks, OpenMP directives, captured statements,
  Objective-C constructs that send messages or unwind (message sends,
  boxed and collection literals, subscripts and property accesses,
  fast enumeration, `@try`, `@throw`, `@synchronized`,
  `@autoreleasepool`), variable-length array bounds wherever they
  appear (a variable, a typedef, a parameter, a cast, a `sizeof`
  operand), and any expression Clang marked as containing an error or as
  still dependent: the collector does not model them, so they refute the
  proof.

Not evaluated, and therefore skipped: operands of `noexcept`, `sizeof`
(other than a variable-length array type), `decltype`, `requires` and
type traits; constant-evaluated expressions and `constexpr` variable
initializers; the discarded branch of an `if constexpr`; the
compile-time branch of `if consteval`. The non-constant array indexes of
an `offsetof` are evaluated.

### What a `try` block discharges

Only a handler that catches everything. A `try` whose handlers are all
typed does not discharge a call, because handler types are not matched
against what the callee throws. The handlers themselves run outside the
`try`, so a throwing call inside `catch (...)` counts. A handler with a
`throw;` in it (not inside a nested `try` or lambda) passes on what it
catches: it discharges nothing, and its `throw;` raises nothing the `try`
block did not. So `try { f(); } catch (...) { cleanup(); throw; }` is
proven exactly when `f` and `cleanup` are, and a `catch (...)` after a
typed handler that rethrows no longer discharges the `try` (what the
typed handler catches leaves). The
function-try-block of a constructor or destructor discharges nothing: it
rethrows when the handler ends.

### Bodies that prove nothing

A definition the linker may replace is treated as able to throw, as is
everything that calls it: a weak definition, a `gnu_inline` body whose
out-of-line copy is written elsewhere, and a C99 inline definition in a
C TU (the external definition the call reaches is another one).

## What is never reported

Proven, but not something to add `noexcept` to:

- functions with a written throwing specification (`noexcept(false)`,
  `throw(X)`): the author said so;
- `main`, `extern "C"` functions, `consteval` functions,
  `operator new`/`new[]`;
- functions whose name comes from a macro expansion (the declaration is
  not in the source to edit);
- code compiled as C++98/03;
- compiler-generated and defaulted-on-first-declaration members (they
  already get the specification they deserve), function-local classes
  and lambdas, template instantiations, and anything defined in a system
  header.

Functions declared `__attribute__((pure))` or `__attribute__((const))`
are not taken as non-throwing. Clang compiles calls to them as unable to
unwind and GCC does not, so their callers are left unreported.

## Completeness of the index

A proof that stays inside one TU, or that ends at functions the calling
TU sees declared `noexcept`, holds whatever else is in the build. A
proof that rests on a function defined in another TU holds only if that
definition is the one the call reaches at link time, which the run can
only know when it indexed every TU cleanly. So:

- **If any TU fails to parse, crashes, times out, or is skipped**, every
  finding whose proof crosses into a function defined in another TU is
  withheld, together with every finding that rests on one. That includes
  a callee whose definition is `noexcept` when the calling TU's
  declaration of it is not: the caller is relying on the definition, not
  on a declaration it saw. Findings whose proof is self-contained are
  still reported.
- **Facts from a TU that parsed with errors** are not trusted: a
  function defined there, and everything whose proof runs through it, is
  withheld.
- The run prints what happened on stderr:

  ```
  check: noexcept-candidate: 225 function(s) proven non-throwing without a noexcept declaration, 225 reported; 609 can throw from code in their own TU, 282 call into another TU's function that can or into one with no indexed definition
  ```

  The counts are definition sites. A site is counted under its
  function's reason: every definition of a function shares the verdict
  of the whole. "From code in their own TU" covers a `throw`, anything
  the collector does not model, and a call to a throwing function the
  same TU defines.

  and, when anything was withheld,
  `; 1 TU(s) did not parse cleanly, so 2 finding(s) whose proof crosses
  into another TU and 0 definition(s) whose proof runs through a TU with
  errors are withheld`.

Running on a subset of a build's TUs is not detected: the run cannot
know which TUs it was not given. A function defined only in an omitted
TU is unknown, which is safe. A function with a second, different
definition in an omitted TU (another binary of the same project reusing
the name) is not seen, and a proof through it reflects only the
definition the subset contains. Give the check every TU that links into
the binaries you care about.

Two `static` functions with the same name in two files with the same
base name (`a/util.cpp` and `b/util.cpp`) share an identity in the
index. They are treated as one function with two definitions, so both
must be proven for either to be reported: a candidate can be missed, not
wrongly reported.

## Assumptions

The proof takes these as given:

- Declarations mean what they say: a function declared `noexcept` does
  not throw (it terminates instead), and every TU agrees on the
  definition of an inline function (see
  [odr-violations](odr-violations.md) and
  [exception-spec-divergence](exception-spec-divergence.md) for when they
  do not). A function declared `noexcept` in one TU and defined without
  it in another, out of sight of that declaration, is reported at its
  definition, and its callers through the `noexcept` declaration are not
  counted toward its rank.
- An exception object's destructor does not throw, so leaving a
  `catch (...)` handler is safe.
- No forced unwinding: thread cancellation and `-fnon-call-exceptions`
  (a trap turned into an exception) are outside the model.
- No link-time or load-time interposition of a non-weak definition. GCC
  honours a `weak` attribute added on a redeclaration after the
  definition; Clang ignores it (with a warning), so such a definition is
  taken as strong.
- A class-specific `operator delete` declared `noexcept(false)` is
  reached only through a `delete` whose static type selects it. Deleting
  through a base class with a virtual destructor runs the dynamic type's
  deallocation function, which the proof does not look up. Deallocation
  functions are non-throwing unless declared otherwise, so this only
  matters for one that says `noexcept(false)`.

## Order of findings

Findings come out most useful first, and carry their position:

1. functions called from a `noexcept` function or a destructor, where an
   exception is already fatal and the declaration lets the compiler drop
   the terminate path: most such callers first;
2. `swap` functions and move operations, which generic code tests with
   `noexcept(...)`;
3. everything else, by the number of indexed functions that call it.

The JSON report gives each finding a `rank` (1 is first), SARIF carries
it as `properties.order`, and text output is in that order. Ranks are
assigned across the enabled checks before inline suppressions, baselines
and changed-line filters apply, so a filtered report keeps the order and
may skip numbers.
Callers are counted among functions that have a fact; calls from template
instantiations count toward the function the instantiation was folded
into.

## The sub-kinds

**Move operations** are separate because a per-TU linter can already
require `noexcept` on them (`performance-noexcept-move-constructor`
and its kin), and an organization that enforces that rule would see only
duplicates here. `noexcept-candidate-move` adds what the linter cannot:
the proof that the declaration would be true.

**Virtual functions** are separate because `noexcept` on a virtual
function binds every override, present and future. The finding says the
function itself cannot throw; it says nothing about its overriders. A
virtual destructor is not reported at all, since it is already
non-throwing.

## Builds without exceptions

In a TU compiled with `-fno-exceptions` no function contains a `throw`
and the compiler already treats every call as non-throwing. The proof is
the same and the findings are true, but adding `noexcept` changes no
generated code there. It still changes what `noexcept(...)`, type traits
and `std::move_if_noexcept` report, and what a client compiled with
exceptions enabled may assume about a header's inline functions. On such
a codebase `noexcept-candidate-move` is the useful check and the main
one is mostly a list of leaf functions.

## Example

```cpp
// leaf.cpp
int leaf(int x) { return x + 1; }

// mid.cpp
int leaf(int);
int mid(int x) { return leaf(x) * 2; }

// api.cpp
int mid(int);
void api() noexcept { mid(1); }
```

```
mid.cpp:2:5: [noexcept-candidate] noexcept candidate: 'mid' cannot throw and
    is not declared noexcept: no throw leaves its body, and every call it
    makes is to a noexcept function, to a function proven the same way, or
    inside a try with a catch (...). Called from 1 noexcept function (such as
    'api'), 1 caller in all.
leaf.cpp:1:5: [noexcept-candidate] noexcept candidate: 'leaf' cannot throw
    and is not declared noexcept: ... Called from 1 function.
```

## Cost

The checks add a body walk per function to the phase-1 parse (functions
from headers and template instantiations included, once per TU that uses
them) and a linear pass over the merged facts. Measured against a run
with no check enabled, that is 24% more CPU time on 214 LLVM TUs
(`lib/Support` and neighbours) and 1% on vycor-cpp's own sources, where
parsing Clang's headers dominates; peak memory does not move.
Enabling any of them turns `--checkpoint` off for the whole run: no TU
is journaled or replayed, for any check, and stderr says so. A fact
depends on every header a body reaches, which the journal does not
revalidate.

## Remediation

Add `noexcept` to every declaration of the function. Do it when the
guarantee is one you intend to keep: `noexcept` is part of the
interface, and removing it later breaks callers that came to rely on it.
A function that cannot throw today only because of how it is currently
implemented is better left alone and suppressed.

## Inline suppression

On the definition, with the name of the check that reported it:

```cpp
// vycor-disable-next-line noexcept-candidate
int parse(const char *s) { return fastPath(s); }
```

or `// vycor: ignore[noexcept-candidate]` on the same line.
