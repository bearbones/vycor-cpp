// Precision fixture: two overloads of process() with DISJOINT caller sets.
// Under name-keyed identity they collapse into one node and their callers
// merge — the F8 red case. Under USR identity, callersOf must keep them
// disjoint and by-name lookup must report ambiguity.
#pragma once

namespace precision {

int process(const char *raw);   // called only by fromCString
int process(double scaled);     // called only by fromDouble

int fromCString();
int fromDouble();

} // namespace precision
