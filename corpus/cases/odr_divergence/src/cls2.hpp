#pragma once
struct Cfg {
  long a = 0;
  long b = 0;
  int get() const { return static_cast<int>(a + b); }
};
