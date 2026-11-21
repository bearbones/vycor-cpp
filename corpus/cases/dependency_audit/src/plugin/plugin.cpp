// Self-registration: the only reference to this library is the static
// initializer below, which runs at load time and names no symbol `app` uses. A
// reference audit sees no use of `plugin` from `app`, and counts the
// initializer under `providerStaticInits`.
#include "core/core.h"

static int plugin_entry(int x) {
  core_log("plugin");
  return x;
}

static int registered = register_plugin(&plugin_entry);
