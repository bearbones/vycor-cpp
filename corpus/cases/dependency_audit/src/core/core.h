// The `core` library's public header: out-of-line functions, one leaf
// (core_version: no calls in its body), and a class with a virtual method.
#pragma once

int core_scale(int x);
int core_sum(int a, int b);
int core_version();
void core_log(const char *msg);
int register_plugin(int (*entry)(int));

struct Shape {
  virtual ~Shape();
  virtual int area() const;
};

int big_entry(int x);
