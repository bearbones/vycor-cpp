#pragma once
#include <atomic>
#include <mutex>

extern int raced;            // two threads write, no lock: a race
extern int guarded;          // every access holds guardMutex
extern std::mutex guardMutex;
extern std::atomic<int> hits; // atomic: excluded
extern int config;           // main writes before the threads start
extern thread_local int scratch;
extern const int kLimit;

void bump();
void bumpGuarded();
int readGuarded();
