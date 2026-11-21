# lock-order-inversion

**Default:** off · **Severity:** warning (`LockOrder_Inversion`), note
(the rest) · **Groups:** compute-heavy · **Diagnostics:**
`LockOrder_Inversion`, `LockOrder_Unproven`, `LockOrder_Plausible`,
`LockOrder_Shared`

Reports locks acquired in opposite orders on different code paths,
across translation units: one path holds `A` while it acquires `B`,
another holds `B` while it acquires `A`. Two threads that take those
paths at the same time can each hold one lock and wait forever for the
other. Cycles through three or more locks are reported the same way,
and so is a mutex acquired again while its holder still holds it (a
self-deadlock).

```bash
vycor-cpp check --build-path build --source-list sources.txt \
  --checks=-all,lock-order-inversion
```

## Findings

| Diagnostic | Severity | When |
|---|---|---|
| `LockOrder_Inversion` | warning | Every edge of the cycle has a witness made of proven call edges and exclusive acquisitions, and the facts cover the whole program; for a self-deadlock, the mutex is also known not to be recursive. |
| `LockOrder_Unproven` | note | The same, but the facts do not cover the whole program (see [Coverage](#coverage)), the acquisition closure stopped at its bound, or a self-deadlock is on a mutex whose kind is not known (a `pthread_mutex_t` takes it at run time). |
| `LockOrder_Plausible` | note | An edge's witness takes a plausible call edge: a virtual call expanded to an override, or a call through a function pointer. |
| `LockOrder_Shared` | note | An edge's witness acquires its lock shared (`lock_shared`, `std::shared_lock`, a reader lock): it blocks only while another thread holds that lock exclusively (or, on a writer-preferring lock, waits to). |

When several apply, the kind is the first of plausible, shared,
unproven, and the message lists every reason (`Not proven: ...`).

One finding per elementary cycle. The message names the cycle in order
(`'accounts' -> 'audit' -> 'accounts'`) and one witness per edge:

```
a.cpp:5:3: [lock-order-inversion] Locks acquired in opposite orders:
'accounts' -> 'audit' -> 'accounts'. 'accounts' then 'audit': credit
calls record at a.cpp:5:3 while holding 'accounts', and record acquires
'audit' at b.cpp:3:45. 'audit' then 'accounts': flush acquires
'accounts' at b.cpp:6:31 while holding 'audit'. Threads taking these
paths at the same time can deadlock.
```

- An acquisition in the holding function itself names the function and
  the acquisition site.
- An acquisition through calls names the call made while the lock is
  held (its site), the shortest call path down to a function that
  acquires the second lock (a long path keeps its first two and last two
  functions), and that acquisition's site. The path ends at an
  exclusive acquisition when the edge's witness is exclusive, never at
  a nearer shared one.

The witness shown for an edge is the strongest one: proven before
plausible, exclusive before shared, direct before through calls, then
the holder's USR and the site. The cycle starts at its lock with the
smallest identity, and the finding sits at its first edge's witness
site. Its entities (`Diagnostic::entities`) are the cycle's lock
identities in that order. The fingerprint hashes them with the check,
the kind and the finding's file, so it holds while the witnesses
change within one file, and moves when the first edge's witness moves
to another file or the kind changes (a plausible witness replaced by
a proven one, say).

A self-deadlock reads `'m' is acquired while already held: ...` with
the one witness.

## What is reported

### The held-while-acquiring graph

An edge `A -> B` exists when lock `B` is acquired while lock `A` is
held:

- **directly:** a function acquires `B` and one of the lock sets held
  before that acquisition contains `A`;
- **through calls:** a function calls `g` while holding `A` (one of the
  lock sets the call was made under contains `A`), and `B` is acquired by
  `g` or by anything `g` reaches through the call graph.

Acquisitions come from the reference facts' `lock` and `lock_shared`
records (format v18, [symbol-references.md](../symbol-references.md#lock-acquisitions)):
a scoped guard's declaration, a deferred guard's `lock()`, a mutex's
`lock` / `lock_shared`, the capability-attribute functions, `std::lock`,
and the waiting pthread lock calls. The locks held, and the locks a call
was made under, are the lock facts unsynchronized-shared-global reads,
through the same code (`check/LockJoin.h`), so the two checks agree on
what the same lock is:

- a lock is named by the mutex variable (a global, a static data
  member, a function-local static) or the data member (every object's
  member mutex is one lock);
- a mutex reached through a pointer, a reference, a call or an array
  element has no name (`unknown`): it is no node of the graph and closes
  no cycle (`--verbose` counts its acquisitions);
- a call with no call record takes the lock sets where the callee's
  address was taken; a callable handed to a function of the program's
  own runs under locks the call does not show (`unknown`).

### What forms no edge

- **Several locks taken at once.** The mutexes of one `std::scoped_lock`
  (or `std::lock(a, b)`, or a `std::lock` of deferred guards) are each
  acquired under the locks held before it: they order none of each
  other, since the standard acquires them with deadlock avoidance.
  Locks held before still order each of them.
- **A try or timed lock.** `try_lock`, `try_lock_for`, `try_lock_until`,
  `std::try_to_lock`, `std::try_lock`, `pthread_mutex_trylock`,
  `pthread_mutex_timedlock` and the other `try` and timed pthread calls
  give up instead of waiting: no acquisition, so no edge ends at them.
  A lock taken that way still orders what is acquired while it is
  held. `std::adopt_lock` takes over a lock already acquired.
- **A recursive mutex** acquired again by its holder: no self-cycle.
  `std::recursive_mutex`, `std::recursive_timed_mutex`, and a class
  whose name contains `recursive` or that holds a standard recursive
  mutex are recursive; the standard non-recursive mutexes and
  `absl::Mutex` are known not to be; anything else is not known.
- **A lock re-acquired while held**, directly or through a call, for
  every other lock held there: the inner acquisition does not wait on
  the others (it self-deadlocks first, or does not block at all if
  recursive).
- **A data member re-acquired while one is held** (`Node::visit` locking
  its child's `mu` while holding its own): a member names every object's
  mutex, so the check cannot tell two objects (a hierarchy locked parent
  first) from one. Counted by `--verbose`, never reported.
- **Once guards** (`std::call_once`, a function-local static's
  initializer) are not locks here.

### Gate locks

A cycle whose every edge is witnessed only under one more global lock
(every witness of every edge holds `G`) cannot deadlock: two threads on
it serialize on `G` first. It is counted (`--verbose`: "under a common
lock") and not reported. A member mutex is not a gate: two paths may
hold two objects' mutexes.

### Shared and exclusive

An acquisition is shared when it is a `lock_shared` record: the
thread-safety attribute says so (`acquire_shared_capability`), or the
spelling does (`lock_shared`, `std::shared_lock`, a guard named like a
reader lock, `pthread_rwlock_rdlock`). The lock sets do not record in
which mode a lock is *held*, so a cycle with a shared acquisition on any
edge is a `LockOrder_Shared` note rather than dropped: two shared
holders do not block each other, but a writer-preferring lock can still
deadlock them against a waiting writer.

## Algorithm and bounds

1. **Facts.** One pass over the reference facts: the acquisition records
   by function, the lock texts (interned lock identities and sets,
   shared with unsynchronized-shared-global), and the functions that
   call under a named lock. Nodes are the named locks some code waits
   to acquire, ranked by identity.
2. **Acquisition closures.** For every function reachable from those
   callers, the set of locks it can acquire, itself or through its
   calls: Tarjan's strongly connected components over the call graph
   (iterative), each component's set computed when it completes from
   its own acquisitions and its callees' sets, as a bitset of two bits
   per lock (exclusive, shared). Identical sets are stored once. This is
   done twice, over every call edge and over proven edges only, so an
   edge knows whether a proven witness exists. Bound: 2²³ 64-bit words
   of distinct sets per pass; past it, the remaining components get no
   set (their edges are missed), every finding is unproven, and the run
   says so on stderr with or without `--verbose`.
3. **Order edges.** Each acquisition under a lock set adds `A -> B` for
   every named `A` in the set; each call under a set adds `A -> B` for
   every lock `B` in the callee's closure. Each edge keeps its best
   witness and the gate locks common to all of its witnesses.
4. **Cycles.** Tarjan's components of the lock graph; in each of two or
   more locks, every elementary cycle of at most 8 locks, each
   enumerated once by a depth-first search from its lowest-ranked lock
   through higher-ranked ones. Bounds: 64 cycles per component, 4096 in
   all (which stops every component), 2²² search steps per component,
   and the length (a component with a path longer than 8 locks counts
   as bounded). A component that hits one says so in each of its
   findings ("the cycle search among these locks stopped at a bound"),
   and the run says so on stderr, also when the component has no
   finding; the cycles it found are still real.
5. **Witness paths.** For an edge through calls, a breadth-first search
   from the callee to a function that acquires the lock, through
   functions whose closure holds it (proven edges only for a proven
   witness), at most 2²⁰ functions; past it the message says the path
   was not shown.

All of it runs on flat arrays sized once (per-function slots, the
closure pool, the lock graph in CSR form); no exception is used for
control flow.

## Coverage

A missing TU can hide an acquisition, a call, or a lock held around
one, so `LockOrder_Inversion` needs facts from every TU. It is
downgraded to `LockOrder_Unproven`, with the reason appended, when the
analyzed sources are fewer than the C/C++ entries of the compilation
database (or the database cannot list them), when a TU did not parse
(crashed, timed out, skipped) or parsed with errors, or when the closure
bound was reached.

## Where the facts come from

The check reads the call graph and the reference facts of the megascope
index. When the index at `--index` (default
`<build-path>/.vycor/megascope.vycs`) holds exactly the selected
sources, none is stale, and it was baked with the same lock types, its
graph and references sections are read (no parse). An index older than
format v18 has no acquisition records and is not read. Otherwise the
check bakes the sources in memory, one parse per TU, shared with the
other graph-backed checks enabled with it.

`--verbose` prints what the check saw: the acquisitions and the
functions making them, the named locks, the acquisitions of unnamed
locks, the functions calling under a lock, the closures computed (and
whether the bound was reached), the order edges, the cyclic lock
groups, the cycles (and how many a gate lock orders), the
self-deadlocks, and the member mutexes acquired while one is held.

## Limits — what it does not see

Where the facts cannot tell, the check stays silent or reports a note.

- **Threads.** The check does not ask whether the two paths of a cycle
  can run at the same time: an inversion that happens only on one
  thread, or only before a second thread starts, is reported as any
  other. Happens-before reasoning is #140.
- **Class-level identity.** A member mutex is one lock for every object
  of the class: `Registry::mu -> Logger::mu` and `Logger::mu ->
  Registry::mu` on two different pairs of objects are reported as a
  cycle though no object is locked in both orders. Telling objects
  apart by the receiver is #159.
- **Lock state does not cross a function boundary.** A callee that
  unlocks a lock its caller holds (`lk.unlock()` on a `std::unique_lock`
  passed by reference, which is an unnamed lock anyway) is still read as
  holding it, and a lock a function returns still held is not seen by
  its caller. A lock every caller of a function already holds is not
  seen there either: a recursive mutex re-acquired under a second lock,
  on paths that all hold the first, still makes an edge back to it
  (#160). A condition variable's wait, which releases and reacquires
  its lock, is no acquisition.
- **Template instantiations** (#106). A call made inside a template
  instantiation has no call graph edge, so an acquisition reached only
  through one is not in its caller's closure. Acquisitions written in a
  template body are recorded on the template as written, so they join
  the graph as soon as the instantiation's calls do.
- **Unnamed locks.** A mutex reached through a pointer or a reference
  parameter (`std::lock_guard g(*mu)`, `void f(std::mutex &m)`) closes no
  cycle; an inversion between such locks is missed.
- **Spawners and tasks.** `std::packaged_task` (#138) and configurable
  spawners (#139) do not matter to the graph itself (a cycle needs no
  thread context), but a call that hands work to another thread while a
  lock is held is not a call: work started that way does not run under
  the caller's lock. Thread entry edges are not followed.
- **Once guards and static initialization.** A `std::call_once` or a
  function-local static's initializer that waits on a lock held by
  another thread waiting on the same once guard deadlocks too; once
  guards are not part of the graph (#161).
- **Conditions are not read.** `if (a) lock x then y; if (!a) lock y
  then x` is reported, though the two orders never run together under
  one condition.
