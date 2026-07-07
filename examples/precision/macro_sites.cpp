// Precision fixture: one macro expanded in two different functions. The
// two call sites share a SPELLING location (the macro definition line),
// so a spelling-keyed context index keeps only one (last wins) — the F8
// macro-collision red case. The compound (site, callerUsr) key must keep
// both contexts.
namespace precision {

int guarded(int x);
int guarded(int x) { return x + 1; }

#define CALL_GUARDED(v)                                                     \
  do {                                                                      \
    if ((v) > 0)                                                            \
      guarded(v);                                                           \
  } while (0)

int userOne(int v) {
  CALL_GUARDED(v);
  return v;
}

int userTwo(int v) {
  CALL_GUARDED(v + 2);
  return v;
}

} // namespace precision
