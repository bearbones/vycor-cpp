# unsynchronized-shared-global

**Default:** off · **Severity:** warning (`SharedGlobal_Race`), note (the
rest) · **Groups:** noisy, compute-heavy · **Diagnostics:**
`SharedGlobal_Race`, `SharedGlobal_Unproven`, `SharedGlobal_Plausible`,
`SharedGlobal_InitThenPublish`

Reports a variable with static storage duration (a global, a static data
member, a function-local static) that is written on at least one path,
accessed from two execution contexts that can run at the same time, and
has no lock common to the two accesses. That is a data race unless
something the check does not see orders the accesses.

```bash
vycor-cpp check --build-path build --source-list sources.txt \
  --checks=-all,unsynchronized-shared-global
```

## Findings

| Diagnostic | Severity | When |
|---|---|---|
| `SharedGlobal_Race` | warning | Two threads reach the variable, at least one writes it, the two lock sets have nothing in common, both thread searches finished within their bounds, and the facts cover the whole program. |
| `SharedGlobal_Unproven` | note | The same, but a thread's search stopped at its bound or the facts do not cover the whole program (see [Coverage](#coverage)). |
| `SharedGlobal_Plausible` | note | The same, but a path takes a plausible edge (a virtual call expanded to an override, a call through a function pointer) or a thread is a `std::packaged_task`, whose runner is not known. |
| `SharedGlobal_InitThenPublish` | note | One side runs on the main thread. A write in `main` before it starts the thread, then reads in the thread, is the usual shape and is safe (thread creation orders it), but the check cannot tell before from after. |

One finding per variable, the most severe pair. It sits at the
variable's definition and names:

- the two access sites, each with its function, file, line and column,
  and whether it writes, reads or takes the address;
- the lock set held at each (`no lock`, or `{m1, m2}`);
- the thread each runs in: the spawner (`std::thread`, `std::async`) and
  where it was called, or the main thread;
- the entry path from the thread's start function to the access
  (`worker -> helper -> bump`; a long path keeps its first two and last
  three functions);
- for a note, why it is not proven (`Not proven: ...`).

The finding's identity (the fingerprint input) is the variable's USR.

## What is reported

### Variables

A candidate is a variable symbol of the reference facts (static storage
duration, including static data members and function-local statics)
that none of these exclude, each decided from the facts the bake
recorded for the variable's declarations:

| Excluded | Decided by |
|---|---|
| `const` / `constexpr` | the `const` attribute: the declared type is const-qualified |
| `std::atomic`, `_Atomic` | the `atomic` attribute |
| `thread_local` | the `thread_local` attribute: one copy per thread |
| a lock or other synchronization object | the `sync_primitive` attribute: a mutex type (the standard mutexes, `pthread_mutex_t` / `pthread_rwlock_t` / `pthread_spinlock_t`, a class with the `capability` attribute, a class whose name contains `mutex`, `spinlock`, `spin_lock`, `rwlock`, `rw_lock` or `CriticalSection`, a class with `lock()` / `unlock()` or `Lock()` / `Unlock()` members of its own or inherited that is not constructed from a reference or pointer to another object; never a guard: a class constructed from a reference or pointer to a mutex, whatever its name, or one named like a guard: `MutexLock`, `ScopedSpinLock`, `RWLockReadScope`, `...Guard`, `...Locker`, `...Holder`), a condition variable, `std::once_flag` / `pthread_once_t`, a semaphore, latch, barrier or `std::stop_source`; arrays of them too |
| a variable code locks | its USR is the identity of a lock held somewhere |
| library state | every declaration is in a system header |

### Accesses

An access is a `write`, `read` or `address_of` reference from a function
to the variable (roles as in [symbol-references.md](../symbol-references.md)).
Taking the address counts as an access but not as a write. A `write`
without the `mutates` flag (only calls of non-const members that have a
const overload with the same parameters and return by value or by const
reference or pointer, such as a non-const map's `find` or `begin`)
counts as a read. A member returning a write proxy (a class with an
assignment operator from another type, such as `std::vector<bool>`'s or
`std::bitset`'s `reference`) mutates. A reference
from a variable's initializer or from file scope is static
initialization, which runs before `main` and before any thread starts:
it is never an access here.

### Threads

Each thread entry of the call graph is a context: the function passed to
`std::thread` / `std::jthread` (`ThreadSpawn`), `std::async`
(`AsyncTask`) or `std::packaged_task` (`PackagedTask`), lambdas
included, grouped by the function started. The main thread is a context
rooted at `main` and the `--entry-point` functions. `std::invoke` and
`std::bind` run synchronously and are followed like calls. Without at
least one thread entry the check reports nothing.

Two accesses are concurrent when they are in different contexts, at
least one of which is a thread, or in the same thread context when that
context can run more than once at the same time:

- its function is started from two or more call sites;
- it is a lambda written inside a loop;
- the start (the `std::thread` construction) is inside a loop;
- the function containing the start is called from inside a loop.

Two accesses on the main thread are never concurrent.

### Lock sets

The lock set at an access is the locks held where the access is written
joined with the locks held at each call on the path from the thread's
start function. The bake records, for each reference, the set of locks
live at each occurrence (format v17, [symbol-references.md](../symbol-references.md#lock-facts)):

- a scoped guard (`std::lock_guard`, `std::scoped_lock`,
  `std::unique_lock`, `std::shared_lock`, a lock type of the org
  config (`--org-config`), a class with the `scoped_lockable`
  attribute, or a class outside namespace `std` with a non-trivial
  destructor constructed from a mutex or a pointer to one, such as
  Abseil's `MutexLock l(&mu)`) holds its mutexes from its declaration to
  the end of the enclosing block; `std::defer_lock` starts it released,
  its `lock()` / `unlock()` calls toggle it, and `release()` hands the
  mutex over still locked: it stays held past the guard's block until an
  unlock of that mutex. A lock type constructed from an argument the bake
  does not read as a mutex, or handed over from elsewhere (a factory, a
  move), holds `unknown`;
- `m.lock()` / `m.unlock()` (and `try_lock`, the shared forms, the
  capability-attribute functions, Abseil's `Lock` / `Unlock` family),
  `std::lock(a, b)` and `std::try_lock(a, b)` (on mutexes, or on
  deferred guards, which they activate; a try is read as taken),
  `pthread_mutex_lock` / `pthread_rwlock_*lock` / `pthread_spin_lock`
  and their unlocks, in statement order;
- control flow: each branch of an `if` and each `case` of a `switch`
  starts from the state before it (a case also from the case above when
  that one falls through), and after the `if`, `switch` or loop a lock
  counts as held only when it is held on every way out that falls
  through: each branch's end, each `break`, a loop's condition (from
  before the loop when it can run zero times, from the end of the body,
  from each `continue`). A branch that ends in a jump (`return`, `break`,
  `continue`, `goto`, `co_return`, `throw`, a `noreturn` call, a
  `switch` with a `default` and no `break` whose last statement jumps,
  a `for (;;)` or `while (true)` with no `break`) does not count. A
  label a forward `goto` reaches starts from the states at those gotos,
  met with the state above it when that falls through. A different lock
  on each path (`if (x) a.lock(); else b.lock();`) leaves `unknown`
  held: until the outermost of those guards goes out of scope, or (for
  locks taken by hand) until an unlock of a mutex that is not held, such
  as the paired `if (x) a.unlock(); else b.unlock();`. A block keeps the lock and unlock calls made in it and
  drops the guards declared in it;
- a lock is named by the mutex variable (a global, a static member, a
  function-local static) or by the data member (`this->m` and `other.m`
  are the same lock: every object's mutex alike). A mutex reached through
  a pointer, a reference, a call or an array element, including a
  variable or member of pointer or reference type (`gp->lock()`,
  `std::lock_guard g(*gp)`), is `unknown`, never "no lock".

A call on the path adds the lock sets its call reference was recorded
under. A call with no call reference adds the sets where the callee's
address was taken (a call through a pointer, `std::invoke`, an
algorithm). A lambda has a call reference where it is created under a
lock and wherever a local holding it is named as an argument of a call
(`l()`, `std::invoke(l)`, `std::sort(b, e, l)`, `f(&l)`); a copy into
another variable, a capture or a return is no call. A lambda with neither was created and
used holding no lock. A lambda or function handed to a function of the
program's own (`withLock([&] { ... })`, `withLock(body)`) runs where
that function calls it, under locks the call does not show: it adds
`unknown`. Any other edge with no record (an implicit destructor call,
say) adds `unknown`.

Two lock sets are disjoint when neither holds `unknown` and they have no
lock in common, or when either is empty. An `unknown` lock against a
non-empty set is assumed common: the check stays silent rather than
guess.

### Ordering it does see

- **`std::call_once`.** Code in the callable runs under a once guard of
  the flag, when the callable is a lambda written in the call or a named
  function (`std::call_once(flag, init)`, or `&init`); a lambda stored in
  a variable first, or a function object, gets no once guard. An access there is not reported: it is ordered before every
  `call_once` on that flag returns. An access elsewhere that does not
  pass through the `call_once` is not known to be ordered after it, and
  a race between the two is missed.
- **Function-local static initialization.** The initializer of a static
  local runs under a once guard of the variable, including what it
  calls.
- **Static initialization.** Initializers of globals are not accesses
  (above).

Everything else that orders memory (a `join`, a condition variable, a
channel, an atomic flag used as a publication barrier, a `fork` before
the threads) is not seen; the init-then-publish note says so for the
common case.

## Coverage

The thread searches walk the whole call graph, and a claim that no lock
is common needs every access, so the verdict needs facts from every TU.
`SharedGlobal_Race` is downgraded to `SharedGlobal_Unproven`, with the
reason appended to the message, when:

- the analyzed sources are fewer than the C/C++ entries of the
  compilation database (or the database cannot list them);
- a TU did not parse (crashed, timed out, skipped), or parsed with
  errors;
- a thread's search stopped at its bound: at most 64 states (distinct
  lock sets, each reached by proven edges only or through a plausible
  one) per function per thread, 2²¹ states per thread, and 2²³ states
  for all contexts together. The threads are searched before the main
  thread, so the total bound is spent on them first; a context that
  stops at it is named by `--verbose`, and one that got no state at all
  says so.

## Where the facts come from

The check reads the call graph and the reference facts of the megascope
index. When the index at `--index` (default
`<build-path>/.vycor/megascope.vycs`) holds exactly the selected
sources, none is stale, and it was baked with the same lock types, its
graph and references sections are read (no parse; `--verbose` says so).
Otherwise the check bakes the sources in memory, one parse per TU,
shared with `dead-code`, `static-init-hazards` and `could-be-internal`
when they are enabled too.

The search is a breadth-first walk per thread over (function, lock set)
states with one parent pointer per state, so each finding's path is a
real route and the cost is bounded by the state caps above.

`--verbose` prints what the check saw: the thread contexts (and how many
can overlap themselves), whether a main thread was found, the candidate
variables (how many), the states searched and the contexts that hit a
bound (each named), and why a saved index was not read when it was
baked with different lock types. No
thread context means no finding was possible, not that the program is
free of races.

## Limits — what it does not see

Precision is chosen over recall: where the facts cannot tell, the check
stays silent or reports a note.

- **Threads it does not recognize.** Only direct calls of the standard
  spawners in user code start a context. `threads.emplace_back(fn)`,
  `std::vector<std::thread>(n, ...)`, thread pools, `llvm::thread`,
  `pthread_create`, `boost::thread`, executors and callbacks registered
  with an event loop are not thread entries, so code they run is either
  missing or counted on the thread that registered it. A program whose
  threads all start that way gets no findings.
- **Orderings other than locks.** See [above](#ordering-it-does-see). A
  global written before `pthread_create` / `std::thread` and read after
  is reported as init-then-publish when the writer is on the main
  thread, and as a race when the writer is another thread that starts
  the reader after writing.
- **Objects that lock themselves.** A class with `lock()` and `unlock()`
  members is read as a mutex, so a global of such a class (a monitor
  that guards its own data) is a synchronization object and is not
  reported, even when its data is used without locking it.
- **Conditions are not read.** `if (needLock) m.lock(); ... if (needLock)
  m.unlock();` holds `m` on one path only, so the access between them is
  read as unlocked.
- **Per-object locks.** A data-member mutex is one lock for every
  object, so `a.m` and `b.m` count as common and a race between two
  objects' locks is missed. A mutex reached through a pointer is
  `unknown` and silences the pair.
- **Loops.** A `do { ... } while (0)` (a lock macro's wrapper) is not a
  loop; any other `for`, `while`, `do` or range-`for` statement is. A
  loop body is read once, from the state before the loop: a lock
  released at the end of one iteration (or before a `continue`) is
  still read as held at the top of the body.
- **Jumps it does not read.** A backward `goto`, a label directly in a
  `switch` body, a computed `goto`, `longjmp`, and a fatal call that is
  not declared `noreturn` (an abort macro around a plain function) are
  read as falling through.
- **Lock scope is lexical.** A guard returned from a function, moved
  into another object, or held across a call that unlocks it is not
  followed. A function that returns with a lock held (or entered with
  one, unlocking it) is seen only through the calls on the path.
- **Indirect calls.** A call through a function pointer or
  `std::function` reaches only the targets the call graph resolved
  (plausible, so a note); an unresolved indirect call is a dead end.
- **Template instantiations.** The search walks into an instantiation
  and on through the calls its body makes, but the reference facts are
  the pattern's, under the template's own identity: an access written
  in the instantiation's body is not found there, and a call it makes
  has no call reference, so it adds `unknown`. An access reached through
  a function the instantiation calls is found, under `unknown` for the
  locks the body holds: reported against an access that holds no lock,
  silent against one that holds any. An access only instantiations
  reveal carries the lock sets of every instantiation (two that guard
  it differently look like one function that does both). Code the
  compiler writes (an implicit special member, a default member
  initializer it runs) is read as part of its user, under the locks of
  the use.
- **Writes through a returned iterator.** `table.find(k)->second = v`
  changes `table` through the iterator `find` returns; the reference
  to `table` is a `find` call, counted as a read, so the write is not
  seen. A write through a pointer or reference to the variable kept
  elsewhere is not seen either.
- **One program per run.** Every program's `main` is the same symbol,
  so sources of several executables analyzed together share one main
  thread, and a thread started in one program can pair with an access in
  another's `main` (`--verbose` says how many files define `main`).
  Select the sources of one program, or read such a pair with that in
  mind.
- **One build configuration**, as for every bake fact.
- **Atomics used non-atomically.** An `std::atomic` is excluded whatever
  its memory order; a race through `memory_order_relaxed` publication is
  not this check's business.
