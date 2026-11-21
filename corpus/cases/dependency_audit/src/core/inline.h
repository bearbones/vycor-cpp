// An inline function in `core`'s header: its body is compiled into every
// object that calls it, so its call to core_scale is a link dependency the
// caller's source never names.
#pragma once

#include "core/core.h"

inline int twice(int x) { return core_scale(x) * 2; }
