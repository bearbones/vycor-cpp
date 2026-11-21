#pragma once
#include <cstddef>

struct Particle {
  float x, v;
};

struct Integrator {
  virtual ~Integrator() = default;
  virtual void apply(Particle *p, int n, float dt) = 0;
};
struct Euler : Integrator {
  void apply(Particle *p, int n, float dt) override;
};
struct Verlet : Integrator {
  void apply(Particle *p, int n, float dt) override;
};

int bad_count(int n);
#define REQUIRE_POSITIVE(n) ((n) > 0 ? (n) : bad_count(n))

void *pool_acquire(std::size_t bytes);
void pool_release(void *block);
void step(Integrator &integrator, Particle *p, int n, float dt);
void report(const Particle *p, int n);
