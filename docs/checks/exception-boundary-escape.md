# exception-boundary-escape

**Default:** off · **Groups:** noisy · **Severity:** warning (proven
witness), note (every other witness) · **Diagnostics:** `Custom`
(`exception-boundary-escape`)

Reports places an exception must not arrive at, but can: a function
that a thread starts in, a function handed to C code as a callback, an
`extern "C"` definition, and a frame compiled without exceptions. Each
finding names the boundary function, gives the shortest call path found
from it to something that can throw, and says how sure that path is.

[exception-escape](exception-escape.md) covers boundaries declared
`noexcept` (there the language calls `std::terminate`, and the question
is only whether a throw reaches it). This check reports the boundaries
that are **not** marked: a `noexcept` boundary function is skipped here.

## Why it matters

| Boundary | What happens when an exception reaches it |
|---|---|
| a thread's initial function | `std::terminate` (for `std::thread`, `std::jthread`); undefined behavior for `pthread_create` / `thrd_create` |
| a callback C code calls | undefined behavior: C frames have no cleanups, and are often compiled without unwind tables |
| an `extern "C"` function | the same, for every C caller |
| a frame compiled without exceptions | the unwinder finds no unwind information for it and calls `std::terminate` |

None of these is visible to a compiler: the throw is usually several
calls and several TUs away from the boundary, and the boundary is a
property of how the function is used, not of its declaration.

## Boundary kinds

| Kind (first fingerprint entity) | Evidence |
|---|---|
| `thread-entry` | the callable passed to a `std::thread` or `std::jthread` constructor, or built in place in a standard container of them (`workers.emplace_back([this] { run(); })`); the start routine passed to `pthread_create` or `thrd_create` |
| `c-callback` | a function passed to a function-pointer parameter of a function with C language linkage (`qsort`, a C library's `register_callback`), or stored in a function-pointer field of a struct declared in an `extern "C"` context (`act.sa_sigaction = handler`, `struct ops o = {f, g}`) |
| `extern-c` | a C++ definition with C language linkage, compiled with exceptions on |
| `no-exceptions-frame` | a function compiled without exceptions (a C TU without `-fexceptions`, or `-fno-exceptions`) that calls a function compiled with them |

Taking a function pointer is not enough for `c-callback`: the receiver
must have C linkage, or the field must belong to a C struct. A callback
handed to a C-linkage function that a C++ TU of the run defines with
exceptions on is skipped: the exception unwinds into that function,
which is itself an `extern-c` boundary and reported as one. The stderr
summary counts these sites.

The callable is resolved where it is handed over: a named function, its
address, a lambda (named in findings as `lambda in 'f'`), a local
variable initialized with one of those, and a class with a single
`operator()`. A generic lambda resolves when it has exactly one
specialization. Anything else (a `std::function`, a parameter, a
function pointer read from memory) names no function and is not a
boundary here.

A frame compiled without exceptions whose callee is an `extern "C"`
function is not reported on its own: the `extern-c` finding of the
callee says it is "called from code compiled without exceptions" and
names one such caller. A function handed over as a thread entry or C
callback is not also reported as `extern-c`.

Not boundaries: `std::async`, `std::packaged_task` and
`std::promise`-based code (the exception is stored in the future and
rethrown at `get()`); hand-overs written inside the standard library;
targets defined in a system header (what `std::bind` or
`std::function` wraps is not visible).

## How it reasons

The check runs on the facts [noexcept-candidate](noexcept-candidate.md)
collects (it turns their collection on; it does not need that check to
be selected). Phase 1 walks every function body each TU defines,
folding in template instantiations, lambdas, implicit members and
system-header inlines, and records per definition:

- **reasons** it can throw by itself, each with the site of the most
  telling one: a `throw` in project code, a `throw` in a system-header
  body or a call to a standard library throw helper
  (`std::__throw_out_of_range`, `std::rethrow_exception`), a virtual or
  indirect call, a throwing `operator new`, or something the walk does
  not model;
- **callees** it can unwind through, each marked by whether a `try`
  with only typed handlers encloses every call to it, or no `try`
  encloses some call. A call inside a `try` with a `catch (...)` is not
  recorded. A handler with a `throw;` in it (outside a nested `try` or
  lambda), typed or `catch (...)`, passes on what it catches: it stops
  nothing, its `throw;` raises nothing the `try` block did not, and the
  `try` counts by its other handlers. `catch (...) { cleanup(); throw; }`
  alone leaves the calls as if no `try` were there; a typed handler that
  rethrows before a `catch (...)` that does not leaves them behind typed
  handlers. A constructor's or destructor's function-try-block stops
  nothing.

The same pass records each hand-over site with the function handed
over (summarized in place when it has no fact of its own, like a
lambda).

Phase 1.5 joins the facts across TUs by linkage name and runs one
reverse breadth-first search per witness class, from the functions that
can throw back to every caller that can unwind into them. A function
declared non-throwing stops the search (the language ends the exception
there). Each boundary then reads off the best class that reaches it and
the shortest path.

## Confidence

| Class | Witness | Severity |
|---|---|---|
| `proven` | a `throw` in project code, no `try` around any call on the path | warning |
| `library` | a standard library throw (`vector::at`, `std::stoi`, `map::at`, ...), no `try` on the path | note |
| `typed-handlers` | a throw, but every path found runs through a `try` whose handlers are all typed: it escapes only if none matches | note |
| `unknown` | a virtual or indirect call that may throw, something the walk does not model, or a C++ callee with no definition in the index, with or without a `try` with typed handlers around it | note |

A throwing `operator new` (`std::bad_alloc`) is not a witness on its
own, whether or not a `try` encloses it: nearly every function allocates, and a check that reported every
allocation behind a boundary would report everything. The standard
library's own allocation and size failures (`std::__throw_bad_alloc`,
`__throw_bad_array_new_length`, `__throw_length_error`, what
`std::allocator` and every container's growth reach) count as
allocation too; its other throw helpers are `library` witnesses. A callee with C
linkage and no definition in the index is taken not to throw (it is C).

The table's order is the report order (`Diagnostic::rank`): by class
(proven, library, typed-handlers, unknown), then by kind (thread entry,
C callback, `extern "C"`, frame without exceptions), then by function
name.

## Completeness of the index

A finding is a path that exists in the code the run indexed; it holds
whatever else is in the build. What the run cannot claim is the
negative. A boundary with no path to a throw is counted on stderr, not
reported, and the count is a proof only when every TU indexed cleanly:

```
check: exception-boundary-escape: 10 boundary function(s), 8 reported (6 proven, 1 library, 1 typed-handlers, 0 unknown); 2 with no path to a throw
```

When a TU fails, the line ends with `, not a proof: N TU(s) did not
parse cleanly`, and `unknown` findings say that the missing definition
may be in one of them. A run on a subset of a build's TUs is not
detected: a function defined only in an omitted TU is an `unknown`
callee, and a boundary whose only throw is there is missed.

### The call-graph gaps

The megascope call graph has three known gaps that matter for exception
analysis. This check does not read the call graph, so they apply
differently:

- **Calls under template instantiations (#106).** The call graph has no
  edges for what an instantiation calls. The facts are collected from
  the instantiated bodies themselves (folded into the function that
  instantiates them), so a throw inside a template is found, and calls
  that depend on a template parameter resolve to what that
  instantiation calls. Not affected.
- **Qualified calls to virtual methods (#112).** The call graph records
  `Base::run()` as virtual dispatch. The fact collector treats a
  qualified call as the direct call it is, and devirtualizes where the
  object's dynamic type is known; only a real dispatch is a virtual call
  (`unknown` when nothing else is found). Not affected.
- **Implicit destructor calls (#113).** The call graph has no edge for a
  destructor the compiler calls. The fact walk does: locals and
  temporaries going out of scope, members and bases from a destructor,
  `delete`. A destructor not declared `noexcept(false)` is non-throwing
  by default and stops the search, as in the language; one that can
  throw is on the path like any call. Not affected.

## Other limits

- Handler types are not matched: a `try` with only typed handlers
  demotes a path to `typed-handlers`, whatever it catches.
- A path is a sequence of calls that can each unwind into the previous
  one; whether the throwing branch is reachable from the boundary with
  real values is not decided (a throw guarded by a check the caller
  always satisfies is still a witness).
- An `extern "C"` function reported for its C callers may in fact only
  be called from C++. The check cannot see callers outside the run, and
  `extern "C"` makes them possible.
- Two `static` functions with the same name in files with the same base
  name share an identity in the facts (see noexcept-candidate's
  [completeness](noexcept-candidate.md#completeness-of-the-index)
  section); their paths merge.

## Message

```
src/pool.cpp:12:6: [exception-boundary-escape] exception-boundary-escape: an
    exception can leave thread entry 'Pool::workerLoop', started by
    std::thread at src/pool.cpp:30:15; an exception that leaves a thread's
    initial function calls std::terminate. Witness: 'Pool::workerLoop' ->
    'Pool::runOne' -> 'parse'; 'parse' throws at src/parse.cpp:41.
    Confidence: proven (no try encloses any call on the path).
```

The location is the boundary function's definition. A thread entry or
callback handed over at several sites is one finding that gives the
first site and the number of others. A witness longer than ten
functions shows its first and last five.

The fingerprint's entities are the kind, the boundary function's name,
and for a hand-over the API it is handed to (`std::thread`, `qsort`,
`sigaction::sa_sigaction`). Two lambdas handed to the same API in one
function share a fingerprint; a baseline counts them as two.

## Cost

The check shares noexcept-candidate's phase-1 body walk (see its
[cost](noexcept-candidate.md#cost) section) plus a check per statement
for a hand-over; phase 1.5 is four breadth-first searches, linear in the
facts. Enabling it turns `--checkpoint` off for the whole run, as the
other checks over these facts do.

Measured with `--threads 4` against the same run with every check off
(`--checks=-all`), user CPU time: spdlog (8 TUs) 9.8 s and 9.4 s,
libzmq (122 TUs) 38.8 s and 40.0 s (+3%), cppcheck (86 TUs) 80.7 s and
81.2 s (+0.6%); peak memory within 3% in each. The parse dominates.

## Remediation

- **Thread entry:** catch everything at the top of the thread function
  and report it, or carry it to the joining thread in a
  `std::exception_ptr` (or use `std::async`, which does).
- **C callback / `extern "C"`:** wrap the body in `try { ... } catch
  (...) { ... }` and translate the exception into the C API's error
  convention (a return code, an error slot), then declare the function
  `noexcept` so the guarantee is checked from then on by
  [exception-escape](exception-escape.md).
- **Frame without exceptions:** catch in the callee before it returns
  into code built without exceptions, or build that code with them.

## Inline suppression

On the boundary function's definition:

```cpp
// vycor-disable-next-line exception-boundary-escape
void *worker(void *arg) { return run(arg); }
```

or `// vycor: ignore[exception-boundary-escape]` on the same line.
