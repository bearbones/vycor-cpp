# static-init-cost

**Unit:** functions · **Native level:** symbol · **Roll-ups:** file, tu, directory, target · **Facts:** in-memory bake only

How much work runs before `main`. For every dynamic initializer and
every `__attribute__((constructor))` function, the number of distinct
functions it can reach through the call graph, and whether that reach
includes an allocation, a lock acquisition, or I/O.

This is the measurement. The verdict on loader-hostile work in the same
initializers is the check
[static-init-hazards](../checks/static-init-hazards.md), which stays a
check: `dlopen` under the loader lock is wrong, while 400 functions
before `main` is a number.

## Why measure it

Static initialization is startup time nobody budgeted for. It runs
before the first line of `main`, on every launch, in an order the linker
picks, usually single-threaded and often under the loader lock. A
registry that self-registers from a header, a logger constructed at
namespace scope, a table built by a lambda: each is cheap to write and
none shows up in a profile of the code people think of as "startup".
The cost is also invisible at the point of change, because the
initializer is one line and the work is whatever it transitively calls.

The metric answers three questions per initializer, per TU, and in
total: how many initializers are there, how far does each reach, and
does that reach touch the heap, a lock or a file descriptor.

## Usage

```bash
vycor-cpp metrics --metrics=static-init-cost \
    --build-path build --source-re 'src/.*\.cpp$'

# Which TUs carry the most, as NDJSON
vycor-cpp metrics --metrics=static-init-cost --rollup tu --format ndjson \
    --build-path build --source-re 'src/.*\.cpp$'

# Per link target
vycor-cpp metrics --metrics=static-init-cost --rollup target \
    --target-map build/targets.json \
    --build-path build --source-re 'src/.*\.cpp$'
```

The facts are collected by the megascope bake's parse and are held in
memory only: run it with `--source`, `--source-list` or `--source-re`.
Against a saved index the metric is `unavailable` (exit 3), not zero.

## What is measured

A **root** is one of:

- a variable of static storage duration at namespace or class scope
  whose initialization runs code: one that is not constant-initialized
  (entity kind `variable`);
- a function marked `__attribute__((constructor))` (entity kind
  `constructor-function`).

Not roots: constant-initialized variables (`constexpr`, `constinit`, or
an initialization the compiler evaluates, which includes a global
`std::mutex` or an empty `std::unique_ptr`: a `constexpr` constructor
runs nothing before `main`, whatever the destructor does), variables
whose only initialization is a trivial default constructor
(`static stack_t old;`, which stays the zeroed object it was), thread-locals
and function-local statics (both initialize on first use, not before
`main`), templated variables that are never instantiated explicitly,
and anything declared in a system header. Constant initialization is the
compiler's verdict under the TU's own flags, so the same `std::string`
global can be a root under `-std=c++17` and not under `-std=c++20`.

The initializer is read as the compiler completed it, not only as
written: the converting constructors of a braced table
(`Entry table[] = {{"a", 1}, {"b", 2}};`), default member initializers,
default arguments, and the elements an array's initializer leaves out
all count.

`value` is the number of **distinct functions reachable** from the root:
the functions its initializer expression calls, and everything those
call, transitively, through the call graph megascope already holds. A
constructor function counts what it calls, not itself.

### Components (symbol level)

| Component | Meaning |
|---|---|
| `directCalls` | Functions the initializer calls directly (for a constructor function, its distinct callees). |
| `allocation` | 1 when the reach includes an allocating function, else 0. |
| `lock` | 1 when the reach includes a lock acquisition, else 0. |
| `io` | 1 when the reach includes an I/O function, else 0. |
| `tus` | Number of TUs that run this initializer. More than one for an initializer written in a header. |

### Witness

For each effect the reach includes, the shortest call chain that shows
it, outermost call first:

```json
"witness": {
  "allocation": ["reg::registerPlugin", "reg::(anonymous namespace)::allocateEntry", "malloc"],
  "lock": ["reg::registerPlugin", "pthread_mutex_lock"]
}
```

Among chains of equal length the choice is fixed by callee name, so the
same index always shows the same chain. An initializer whose reach has
no effect carries no witness.

### Roll-ups

A group's `value` is the size of the **union** of its members' reaches:
the distinct functions that group's initializers reach together. Two
initializers that both reach `log()` run it twice but reach one
function, so group values are smaller than the sum of their rows, and
the total (`metrics[].overall`) is the union over the whole program.

| Component | Meaning in a group |
|---|---|
| `dynamicInitializers` | Number of `variable` roots in the group. |
| `constructorFunctions` | Number of `constructor-function` roots in the group. |
| `directCalls` | Sum of the members' `directCalls`. |
| `allocation`, `lock`, `io` | Number of members whose reach includes that effect. |
| `entities` | Number of roots in the group. |

Group records carry no witness; ask for the symbol level to see chains.

An initializer in a header belongs to every TU that includes it, and
counts in each of those TUs' rows, directories and targets. A `static`
(internal-linkage) header variable is one record whose `tus` says how
many separate copies run.

## Order

Largest reach first; ties by file, then line, then name. For the
fixture in `examples/static_init`:

```
value  name             file:line                 allocation lock io
    7  codecSlot        plugins/codecs.cpp:20          1       1   0
    6  reg::coreSlot    core/registry.cpp:50           1       1   0
    6  reg::headerSlot  core/registry.hpp:43           1       1   0
    3  greeted          plugins/plugins.cpp:21         0       0   1
    3  banner           plugins/plugins.cpp:30         0       0   1
    2  onLoad           plugins/plugins.cpp:33         0       0   1
    1  ordinal          plugins/plugins.cpp:14         0       0   0
    1  scratch          plugins/plugins.cpp:17         1       0   0
```

Eight roots reach twelve distinct functions between them.

## The effect table

"Allocates", "locks" and "does I/O" are decided by name against one
table of functions (`include/vycor/callgraph/Effects.h`), shared with
any other analysis that asks the same question:

- **allocation**: `operator new` / `new[]`, the `malloc` family,
  `strdup`, `std::make_unique`, `std::make_shared`,
  `std::allocator::allocate`;
- **lock**: `pthread_mutex_lock` and its siblings, `mtx_lock`,
  `std::mutex::lock` and the other standard mutexes, the constructors of
  `std::lock_guard` / `unique_lock` / `scoped_lock` / `shared_lock`,
  `std::lock`, `std::call_once`;
- **io**: C stdio, POSIX descriptor, file and socket calls, and the
  file-stream constructors and `open`.

The table also has a fourth kind, `throw`, which this metric does not
report (a throw is not start-up work). A `throw` expression is a call of
`__cxa_throw` in the graph, so it counts as one reached function.

A function matches whatever its template arguments and whichever inline
namespace the standard library puts it in:
`std::__1::lock_guard<std::mutex>::lock_guard` matches
`std::lock_guard::lock_guard`.

The table lists functions whose effect is unconditional. `push_back`
allocates only past capacity and is not listed; the `operator new` it
calls is.

A new-expression is a call of its allocation function, at any depth:
`new T` reaches `operator new`, `new T[n]` reaches `operator new[]`,
and both are allocations, as are their aligned and `nothrow` forms. Two
kinds of `new` are not, by themselves:

- **placement new.** `new (buf) T` reaches the placement overload,
  which constructs in storage the caller already has. The same holds
  for a placement overload of your own (`operator new(size_t, Arena &)`):
  it is ordinary code, an allocation when what it calls is one.
- **a class's own `operator new`.** `new Pooled` reaches
  `Pooled::operator new`, which is an allocation when its body reaches a
  listed function (or when you list it).

Add your organization's allocators, lock wrappers and logging sinks in
the org config (`--org-config`):

```json
{
  "effects": {
    "allocation": ["myorg::Arena::grow"],
    "lock": ["myorg::SpinLock::acquire"],
    "io": ["myorg::log"]
  }
}
```

or in code from `ext/`:

```cpp
VYCOR_EXTENSION_SETUP(MyOrgEffects) {
  registry.addEffectFunctions(vycor::EffectKind::Io, {"myorg::log"});
}
```

## Reading the numbers

The reach is a static upper bound on the *distinct code* an initializer
can enter, not a count of calls or of time: a loop that calls one
function a million times reaches one function. It follows the call
graph's edges, including the plausible targets of virtual calls and
function pointers, so it can include functions a particular run never
enters.

The effect flags are lower bounds, for the reasons under Limitations: a
`0` means no listed function was found on a path the graph holds, not a
proof of absence.

Use it comparatively: which initializers and which TUs dominate, what a
change added, whether a target that should start instantly has anything
before `main` at all.

## Limitations

- **An instantiation no TU instantiated is a leaf.** The bake reads the
  body of every template instantiation a TU uses, at every depth, so a
  dependent call is an edge to the function it resolved to and the work
  inside standard containers and smart pointers is reached:
  `std::vector<int> v = build();` reports the vector's allocation
  through `std::allocator`. What stays a leaf is an instantiation whose
  body no indexed TU holds, chiefly one declared `extern template` (the
  standard library does this for `std::string`'s members), and a
  function a system header defines that is not a template, like any
  prebuilt code.
- **Lambdas are taken as called.** A lambda written in an initializer
  counts as called by it, whether it is invoked there or stored.
- **A lambda in a header is one function per spelling of the header's
  path.** The call graph names a lambda by the path its file was opened
  under, so a header reached as `inc/h.hpp` from one TU and
  `../lib/inc/h.hpp` from another holds two lambdas, and an initializer
  in that header reaches both.
- **Compiler-generated constructors are looked through** to the member
  and base constructors they run; they are not counted as functions.
  That covers defaulted constructors and constructors inherited with
  `using Base::Base;`.
- **The effect table matches names.** A function of your own called
  `::read` or `::send` is taken for the POSIX one. The one place the
  parameters are read is `operator new`, to tell the placement forms
  from the allocating ones.
- **Destructors are out of scope.** What runs at exit, including the
  `atexit` registration a non-trivially-destructible global implies, is
  not measured.
- **A constructor function that another initializer also calls** is not
  counted in a group's union, where it is a starting point: the group
  value can be one lower than the true union.
- **Prebuilt code is opaque.** A function with no body in the indexed
  TUs is reached and counted, and its own calls are unknown.
- **Coverage.** The measurement covers the TUs the bake indexed;
  `indexScope` on the payload says how many of the requested ones that
  is.
