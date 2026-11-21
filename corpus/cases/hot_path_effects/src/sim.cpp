#include "sim.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace {
std::mutex stats_mutex;
long steps_taken = 0;
} // namespace

int bad_count(int n) { throw std::invalid_argument("particle count"); }

void *pool_acquire(std::size_t bytes) { return std::malloc(bytes); }
void pool_release(void *block) { std::free(block); }

static void count_step() {
  std::lock_guard<std::mutex> lock(stats_mutex);
  ++steps_taken;
}

static void warn_slow(float dt) { std::fprintf(stderr, "slow %f\n", dt); }

void Euler::apply(Particle *p, int n, float dt) {
  for (int i = 0; i < n; ++i)
    p[i].x += p[i].v * dt;
}

void Verlet::apply(Particle *p, int n, float dt) {
  float *previous = new float[n];
  for (int i = 0; i < n; ++i) {
    previous[i] = p[i].x;
    p[i].x += p[i].v * dt;
  }
  delete[] previous;
}

void step(Integrator &integrator, Particle *p, int n, float dt) {
  n = REQUIRE_POSITIVE(n);
  float *forces = static_cast<float *>(pool_acquire(sizeof(float) * n));
  for (int i = 0; i < n; ++i)
    forces[i] = -p[i].x;
  integrator.apply(p, n, dt);
  pool_release(forces);
  count_step();
  if (dt > 1.0f)
    warn_slow(dt);
}

void report(const Particle *p, int n) {
  for (int i = 0; i < n; ++i)
    std::printf("%d %f\n", i, p[i].x);
}
