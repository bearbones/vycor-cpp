# static_init

Fixture for the `static-init-cost` metric
([docs/metrics/static-init-cost.md](../../docs/metrics/static-init-cost.md)):
three TUs in two directories with work before `main`.

```
core/registry.hpp     registerPlugin, a template that calls it, and an
                      inline variable every includer initializes
core/registry.cpp     the registry: allocates an entry and takes a lock
plugins/plugins.cpp   initializers that do I/O, allocate, run a lambda,
                      and an __attribute__((constructor)) function
plugins/codecs.cpp    an initializer that reaches the registry through
                      a template instantiation
targets.json          target map: core -> core, plugins -> plugins
```

```bash
F=examples/static_init
vycor-cpp metrics --metrics=static-init-cost \
    --source $F/core/registry.cpp --source $F/plugins/plugins.cpp \
    --source $F/plugins/codecs.cpp -- -std=c++17

vycor-cpp metrics --rollup target --target-map $F/targets.json \
    --source $F/core/registry.cpp --source $F/plugins/plugins.cpp \
    --source $F/plugins/codecs.cpp -- -std=c++17
```

Eight roots (seven dynamic initializers, one constructor function)
reach twelve distinct functions. `tests/test_metrics.cpp` pins every
value and `scripts/cli-golden.py` the CLI output.
