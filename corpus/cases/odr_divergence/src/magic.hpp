#pragma once
inline int magic() {
#ifdef MODE
  return 1;
#else
  return 0;
#endif
}
