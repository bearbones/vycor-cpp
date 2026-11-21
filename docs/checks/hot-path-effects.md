# hot-path-effects

**Default:** off · **Groups:** compute-heavy · **Diagnostics:** `Custom` (`hot-path-effects`) · **Severity:** warning; latent findings are notes

Finds the heap allocations, lock acquisitions, I/O and throws reachable
through the cross-TU call graph from the functions you call hot: a
kernel, a per-frame update, a solver step, a request handler. Each
finding is one call of an effect function, with one witness path from
a hot root to it.

Given a coverage profile of a real run, it also says how hot each path
was: every path is weighted by its rarest call, and a path that never
ran in the profile is reported apart, as **latent**, from one that did
(**observed**). "This kernel can allocate" and "this kernel allocated
2,000 times in the benchmark" are different findings, and the second is
the one to fix first.

The megascope tool `query_path_effects` answers the same question over
an index, with the full payload (below); the check turns its findings
into CI-gate diagnostics.

## Example

```cpp
void *pool_acquire(std::size_t bytes) { return std::malloc(bytes); }
static void count_step() { std::lock_guard<std::mutex> lock(stats_mutex); ++steps; }
int bad_count(int n) { throw std::invalid_argument("particle count"); }

void step(Integrator &integrator, Particle *p, int n, float dt) {
  n = REQUIRE_POSITIVE(n);                 // calls bad_count when n <= 0
  float *forces = static_cast<float *>(pool_acquire(sizeof(float) * n));
  integrator.apply(p, n, dt);              // Euler::apply or Verlet::apply (new[])
  pool_release(forces);
  count_step();
}
```

```bash
vycor-cpp check --build-path build --source ... --checks=hot-path-effects \
    --hot-profile run.json --hot-min-count 1000
```

```
sim.cpp:15:48: [hot-path-effects] allocation reachable from hot root 'pool_acquire': pool_acquire -> malloc (observed, the rarest call on the path ran 2000 times in run.json)
sim.cpp:19:31: [hot-path-effects] lock reachable from hot root 'count_step': count_step -> std::lock_guard<std::mutex>::lock_guard (observed, the rarest call on the path ran 2000 times in run.json)
sim.cpp:31:21: [hot-path-effects] allocation reachable from hot root 'step': step -> Verlet::apply -> operator new[] (latent: a call on the path never ran in run.json)
sim.cpp:13:24: [hot-path-effects] throw reachable from hot root 'step': step -> bad_count -> __cxa_throw (latent: a call on the path never ran in run.json)
```

The run only ever used `Euler`, so the `new[]` in `Verlet::apply` is
latent even though the virtual call that reaches it ran 2,000 times:
the profile's entry count for `Verlet::apply` is 0. This program is the
validation corpus case `corpus/cases/hot_path_effects`, with its real
profile.

## Hot roots

A root is where a hot path starts. Three sources, unioned:

| Source | How |
|---|---|
| Names | `--hot-root <name>` (repeatable), the tool's `roots`, or the org config's `hotPaths.roots`. A qualified name names every overload and template instantiation of it (template arguments and inline namespaces are dropped before comparing, as for the effect table); a USR names one function. |
| Patterns | The tool's `root_patterns` or `hotPaths.rootPatterns`: POSIX extended regexes searched in each qualified name (`^myorg::kernels::`, `::update$`). |
| Profile | With a profile, every function entered at least `--hot-min-count` (tool: `min_count`, config: `hotPaths.minCount`) times. A function the effect table lists, or one on the allow list, is never a profile root. |

A configured name or pattern that matches no function is reported
(`roots.unmatched` in the tool, a line on stderr from the check), so a
typo does not silently check nothing, and the tool does not answer
`clean` while one is unmatched. Enabling the check without any root is
a usage error (exit 2), and so is a run in which no configured root
matched and the profile gave none.

A path never runs through another root: every finding attaches to a
root above it with no root in between. Without a profile that is the
nearest one (the shortest path); with a profile it is the root of the
heaviest path, which can be a farther one. With profile roots, every hot
function is a root, so the witnesses are short and start at an innermost
hot function; name the outer roots in the config to see the longer chains
(`query_path_effects --roots main --profile run.json` gives each
finding's path from `main`).

Annotation roots (`[[clang::annotate("vycor::hot")]]` in the source)
need a per-function fact the index does not hold yet; they will follow
the next snapshot-format change
([#122](https://github.com/bearbones/vycor-cpp/issues/122)).

## Effects

The effect table (`vycor/callgraph/Effects.h`) is shared with the
`static-init-cost` metric. Built in:

| Effect | Functions |
|---|---|
| `allocation` | `operator new` / `operator new[]` (the replaceable forms; placement new is not an allocation), `malloc`, `calloc`, `realloc`, `aligned_alloc`, `strdup`, `std::make_unique`, `std::make_shared`, `std::allocator::allocate`, ... |
| `lock` | `std::mutex::lock` and the other standard mutexes, `std::lock_guard` / `std::unique_lock` / `std::scoped_lock` construction, `pthread_mutex_lock`, ... |
| `io` | stdio (`printf`, `fprintf`, `fwrite`, ...), POSIX `read` / `write` / `open`, ... |
| `throw` | every `throw` expression (a call of `__cxa_throw`; a bare `throw;` of `__cxa_rethrow`), `std::rethrow_exception`, `std::throw_with_nested`, ... |

`new T` and `delete p` are calls of the allocation functions they select
(call-graph allocation edges), and a `throw` expression is a call of
`__cxa_throw`, so all of these appear as ordinary edges. Add your own
allocators, lock wrappers and loggers through the org config's
`effects` key (`docs/EXTENDING.md`).

A function the table lists is where a path ends: an organization
allocator that itself calls `malloc` is one finding, at the call of the
allocator.

### The allow list

`hotPaths.allow` (tool: `allow`) lists accepted functions — a
preallocated pool's `acquire`, a lock-free logger's slow path. A call of
an allowed function is neither reported nor followed. The tool reports
how many such calls it skipped (`allow.calls`).

## The profile

Pass the JSON that `llvm-cov export` writes for a run of an instrumented
build (source-based coverage):

```bash
clang++ -fprofile-instr-generate -fcoverage-mapping ... -o app
LLVM_PROFILE_FILE=app.profraw ./app <representative workload>
llvm-profdata merge -sparse app.profraw -o app.profdata
llvm-cov export ./app -instr-profile=app.profdata --skip-branches > run.json
```

The profile is read once per run and joined to the index **by source
location**:

- a call site's count is the count of the coverage region at its
  location (the last segment starting at or before it, the way
  `llvm-cov show` colours a line);
- a function's entry count is the count of the profile function body
  that holds most of its call sites. A function that makes no call
  cannot be joined, and needs no count: nothing is reachable from it.
- Files are matched by path suffix: the index spells a file the way the
  compile command did (often relative to the build directory), the
  profile with the absolute path of the machine that ran it. The longest
  run of trailing path components decides, and a tie between two
  profile files matches neither. The match is two-sided: when two index
  files reach the same profile file, it goes to the one sharing more
  trailing components, and to neither on a tie. A profile of one binary
  holding `lib/a/util.cpp` therefore does not lend its counts to an
  indexed `tools/b/util.cpp`.

The **weight** of a path is the smallest count on it: of every call site
and of every function entered that has an entry count. The witness of a
finding is a path of the largest weight (then the fewest calls; among
equal paths, a Proven call is preferred over a Plausible one, then the
caller's USR and the call site decide). The
entry counts are what tell the targets of one virtual call apart: the
call site ran, but only one override was entered.

| Evidence | Meaning |
|---|---|
| `observed` | Every call on the path ran; `weight` is how often the rarest did. |
| `unprofiled` | Nothing on the path is known not to have run, and something has no count: a file the profile does not have, code the profiled binary did not contain, or a run without a profile. |
| `latent` | Some call on the path never ran (weight 0). |

Findings are ordered observed, unprofiled, latent; heaviest first;
then by effect, function, call site and callee. The check reports latent
findings at note severity; `--fail-on warning` gates only on the paths
that ran (and the unprofiled ones).

What a count does not say:

- Counts are per call site, not per stack. An observed path is one whose
  every step ran, which does not prove they ran as that one chain.
- A call written inside a macro is recorded at the macro body's spelling,
  where the coverage mapping also puts the macro's code, so the count is
  the body's, summed over every expansion of the macro.
- A file compiled into the binary several times with different flags
  (or one header's inline functions in many TUs) has one merged count
  per location.
- The profile describes the workload that produced it. Latent is "not in
  this run", not "unreachable".

The tool reports the join so its coverage is visible: `profile.join`
counts the index's distinct call sites (one per location, however many
edges leave it), those in a file the profile has, those with a count,
and the functions with calls and those joined to an entry count. The
check warns when the profile matched no call site, and stops with a
usage error (exit 2) when the profile was the only root source and gave
no root: a wrong profile must not pass the gate silently.

## Usage

```bash
# Configured roots, no profile: every finding is unprofiled.
vycor-cpp check --build-path build --source ... --checks=hot-path-effects \
    --hot-root 'myorg::Solver::step'

# Profile roots: everything entered at least 10,000 times.
vycor-cpp check ... --checks=hot-path-effects \
    --hot-profile run.json --hot-min-count 10000

# The tool, over an index: the full witness paths and counts.
vycor-cpp megascope query-path-effects --roots main --profile run.json \
    --effects allocation --effects lock --format ndjson
```

Org config (`--org-config`):

```json
{
  "hotPaths": {
    "roots": ["myorg::Solver::step"],
    "rootPatterns": ["^myorg::kernels::"],
    "allow": ["myorg::Pool::acquire"],
    "minCount": 10000
  },
  "effects": { "allocation": ["myorg::Arena::alloc"], "throw": ["myorg::raise"] }
}
```

`minCount` only takes effect with a profile (`--hot-profile`): without
one, the configured names and patterns are the roots. An explicit
`--hot-min-count` (tool: `min_count`) without a profile is a usage
error. The profile itself is per run, not part of the config. The
`--hot-*` flags are ignored, with a warning, when the check is not
enabled.

A finding's identity (its baseline fingerprint) includes its evidence,
so a finding that moves from latent to observed when a new profile is
used comes back as new: that change is the news.

Grouped **compute-heavy**: it builds the full cross-TU call graph (shared
with `dead-code` and `static-init-hazards` when they are enabled, or read
from the build directory's megascope index when that index is clean).

## The tool: `query_path_effects`

Arguments: `roots`, `root_patterns` (added to the org config's),
`profile`, `min_count` (overrides the config's), `effects` (subset of
`allocation`, `lock`, `io`, `throw`; default all, and an empty list is a usage error), `allow` (added to the
config's), `limit` / `offset`. Reads only the graph section of an index.

Payload:

| Member | |
|---|---|
| `findings` | One record per (effect call, effect kind): `effect`, `evidence`, `weight` (null when unprofiled), `function` / `usr` (the caller of the effect function), `callee` / `calleeUsr`, `callSite`, `count`, `root` / `rootUsr`, `rootSources` (`config`, `profile`), `rootCount`, `confidence` (the weakest hop), `depth`, and `path`: the hops from the root, each `{caller, callerUsr, callee, calleeUsr, callSite, kind, confidence, count, calleeCount}`. |
| `summary` | Finding counts by evidence, then effect. |
| `roots` | `total`, `config`, `profile`, `minCount`, `unmatched`. |
| `profile` | `null`, or `{path, files, functions, executedFunctions, join}`. |
| `allow` | `functions` (entries), `calls` (calls skipped). |
| `reachedFunctions` | Functions visited from the roots, roots included. |
| `templateLeaves` | Reached template instantiations with no recorded call: functions whose calls the graph does not hold (#106, below). |
| `exhaustive` | True when `templateLeaves` is 0. The search has no depth or fan-out limit; what it cannot see is inside instantiations. |
| `verdict` | `effects`; `clean` when there were roots, every configured root matched, no finding, no template leaf, and the index states complete coverage of what was requested; otherwise `unknown`. |

Usage errors (exit 2): no root from any source; `min_count` without a
profile; an unreadable or malformed profile; an unknown effect or an
empty `effects`; an invalid root pattern. Like the other list tools, a
run with no finding exits 1.

## Limitations

The search visits everything reachable in the call graph, so what it
misses is what the graph misses:

- **Calls under template instantiations** (#106). The graph records the
  calls written in a template's body under the template, not under its
  instantiations, and calls that depend on a template parameter not at
  all, so a forward walk stops at the first instantiation it meets. An
  allocation inside a project's own templated helper or container,
  called from a hot root, is missed. A call of a listed function is
  still found (`std::make_unique<T>(...)` is itself the effect). The
  tool counts the instantiations it reached that have no recorded call
  (`templateLeaves`) and does not answer `clean` while there are any;
  the check prints the count as a warning. In template-heavy code this
  is most of what a root reaches (vycor-cpp's own `main` reaches 4,172
  functions, 2,401 of them such leaves, mostly standard-library
  instantiations), and a root that is itself an instantiation reaches
  nothing: name the non-template functions it calls instead.
- **Qualified calls to a virtual function** (#112) are recorded as
  virtual dispatch, so `Base::f()` written explicitly also reaches every
  override of `f`: possible false findings, never missed ones. With a
  profile, an override that never ran is latent.
- **Implicit destructor calls** (#113) have no edge: a destructor that
  frees or locks, run at the end of a scope, is not reached (`delete p`
  is, through its `operator delete` edge).
- **Default member initializers** (#120): a call or `new` written in a
  member's default initializer has no edge from the constructors that
  run it.
- **Standard-library internals.** Edges inside system headers are not
  recorded, so the standard library's own functions are leaves: a
  `std::vector::push_back` that grows its buffer, `std::string`
  concatenation, or `std::cout <<` is not an effect unless the function
  called is in the effect table. Add the container operations your
  policy bans (`std::vector::push_back`, `std::vector::resize`, ...) to
  `effects.allocation`.
- **Function pointers and callbacks** follow the graph's Plausible-edge
  semantics; an effect reached only through an unresolved pointer is
  missed.
- **Prebuilt code** whose source is not in the compilation database is
  invisible beyond the call into it.

## Regenerating the corpus profile

The fixture `corpus/cases/hot_path_effects/profile.json` is a real
`llvm-cov export` of that case's program, with the build machine's
absolute paths rewritten to `/build/hot_path/` (the join matches files
by suffix):

```bash
cd corpus/cases/hot_path_effects/src
clang++ -std=c++17 -O1 -fprofile-instr-generate -fcoverage-mapping \
    -I . sim.cpp main.cpp -o /tmp/sim
LLVM_PROFILE_FILE=/tmp/sim.profraw /tmp/sim > /dev/null
llvm-profdata merge -sparse /tmp/sim.profraw -o /tmp/sim.profdata
llvm-cov export /tmp/sim -instr-profile=/tmp/sim.profdata --skip-branches \
    | sed "s#$PWD#/build/hot_path#g" > ../profile.json
```

## Remediation

Move the effect off the hot path: preallocate (a pool, an arena, a
reserved buffer sized once), take the lock outside the loop or replace
it with a per-thread or lock-free structure, make the error path
return a status instead of throwing, and batch I/O. When an effect is
accepted (a pool's refill, a once-per-run log), put the function on the
allow list so the next finding is a new one.
