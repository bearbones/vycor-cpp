# callback-under-lock

**Default:** off. **Severity:** warning. **Group:** compute-heavy.

```sh
vycor-cpp check --build-path build --source application.cpp \
  --checks=-all,callback-under-lock
```

Reports an application callback reachable while a known nonrecursive standard
mutex guard remains live. Reentering code that acquires the same mutex can
self-deadlock, even on one thread. A diagnostic is a reentrancy review candidate,
not proof that the application callback actually reenters or violates its API
contract. No rewrite is generated.

```cpp
struct Sink {
  std::mutex mutex;
  std::function<void()> callback;
  void log() {
    std::lock_guard<std::mutex> guard(mutex);
    callback(); // warning: callback can reenter log()
  }
};
```

Prefer taking a stable snapshot under the lock and invoking callbacks after
releasing it, when the API's mutation/lifetime semantics permit that. A recursive
mutex avoids this particular same-thread lock cycle but does not establish that
arbitrary reentrant operations are correct.

## Recognition and evidence

The check follows lexical guard lifetimes and explicit `unique_lock::lock()` /
`unlock()` operations. It recognizes `std::lock_guard`, `std::unique_lock`,
`std::scoped_lock` and `std::shared_lock` around `std::mutex`, `std::timed_mutex`,
`std::shared_mutex` and `std::shared_timed_mutex`. Aliases and instantiated template
arguments use their canonical types. Recursive and unknown/custom/null mutexes
are excluded. Deferred, try-lock and adopted guards do not establish acquisition;
an explicit subsequent `lock()` can establish it. Guard ownership passed to a
helper, referenced through an alias, or moved is treated as opaque.

Callback boundaries include indirect function-pointer calls, `std::function` /
`std::move_only_function` invocation, and call operators on opaque supplied parameters
or stored functor fields. A supplied functor with a known body is followed as a
normal helper rather than treated as an unknown callback. Merely constructing a lambda does not execute its body.
The helper search follows definitions and possible virtual overrides available
in the current TU (including instantiated code in included headers). It is
cycle-safe without a call-depth cutoff. The message includes the acquisition
location, held guard, helper call-site path and callback boundary. Virtual targets
are possibilities; receiver identity and a path back to that exact mutex are not
proven. The normal finding fingerprint, suppression, baseline, changed-line and
text/JSON/SARIF reporting machinery applies.

Lock-taking helpers do not export their destroyed guards to their callers:

```cpp
Snapshot snapshot() {
  std::lock_guard<std::mutex> guard(mutex);
  return copy_state();
}
void notify() {
  auto state = snapshot();
  callback(state); // snapshot's guard is no longer held: no warning
}
```

## Real-world regressions

[Sigslot v1.1.0's release notes](https://github.com/palacaze/sigslot/releases/tag/v1.1.0)
acknowledge a recursive-signal deadlock in v1.0, fixed by taking a copy-on-write
snapshot before invoking slots. The old version is a positive case; fixed/current
sigslot and eventpp's short-lock callback traversal are negative controls.
Spdlog's callback sink supplies the inherited virtual-dispatch example:
`base_sink::log` holds a guard while `callback_sink::sink_it_` invokes user code.
Reentrant logging through that same sink is a reproduced API hazard, not an
assertion that a downstream project violates a documented contract.
Pinned clients and runtime receipts are in
[open-source-todos](https://github.com/bearbones/open-source-todos/blob/main/case-studies/callbacks-under-lock.md).

## Limits and freshness

This initial check is TU-local: it does not persist helper/lock summaries across
separately parsed TUs. An out-of-line helper whose definition is unavailable,
arbitrary pointer aliases, manual mutex locking, ownership-transfer summaries,
custom lock wrappers, lock/unlock operations hidden inside helpers, and general
path feasibility are not modeled. Conditional paths can produce conservative
warnings. Guard release through unmodeled aliases can produce false positives;
unknown callback/locking wrappers can hide hazards. A silent result is not a
liveness guarantee.

When enabled, this check disables checkpoint reuse for the entire run. The legacy
check journal does not validate header and compile-command dependencies, which
can change guard scope or template instantiation without changing the main file.
In-process and isolated-worker execution both analyze fresh ASTs. No graph
snapshot or worker payload format change is required.
