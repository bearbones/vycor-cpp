#pragma once
#include <mutex>
#include <string>

// A registry that logs what it registers, and a logger that reports how
// many entries the registry holds when it flushes: each takes its own
// lock and then calls into the other, so the two locks are taken in
// opposite orders (a deadlock when two threads do it at once).
extern std::mutex registryMutex;
extern std::mutex logMutex;
extern std::mutex statsMutex;
extern std::recursive_mutex reentrantMutex;

void registerItem(const std::string &name); // registryMutex, then logMutex
int registryCount();                        // registryMutex
void logLine(const std::string &line);      // logMutex
void flushLog();                            // logMutex, then registryMutex
void snapshotBoth();                        // scoped_lock of both: no order
void pollStats();                           // try_lock of logMutex: no wait
void reenter(int depth);                    // recursive mutex re-entered
