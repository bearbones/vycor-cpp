#include "sim.h"

int main() {
  Particle particles[256];
  for (int i = 0; i < 256; ++i)
    particles[i] = {float(i), 1.0f};
  Euler euler;
  for (int t = 0; t < 2000; ++t)
    step(euler, particles, 256, 0.01f);
  report(particles, 4);
  return 0;
}
