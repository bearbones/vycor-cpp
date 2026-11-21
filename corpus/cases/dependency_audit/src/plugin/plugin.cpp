// Self-registration: the only reference to this library is the static
// initializer below, which runs at load time and is not a call edge. A
// call-edge audit sees no use of `plugin` from `app`.
#include "core/core.h"

static int plugin_entry(int x) {
  core_log("plugin");
  return x;
}

static int registered = register_plugin(&plugin_entry);
