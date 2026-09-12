void alpha();
void beta();
#ifdef NEW_TARGET
void entry() { beta(); }
#else
void entry() { alpha(); }
#endif
int main() {
  entry();
  return 0;
}
