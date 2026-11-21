// No standard headers: the answers must not depend on the host's library.
using size_type = decltype(sizeof(0));

// The reserved placement form, declared as <new> declares it.
void *operator new(size_type, void *) noexcept;

struct Node {
  int value;
  Node *next;
};

// A class with its own allocation functions.
struct Pooled {
  static void *operator new(size_type size);
  static void operator delete(void *p);
  int id;
};

Node *build();          // new Node
int *grow(int n);       // new int[n]
Node *place(void *buf); // new (buf) Node: constructs, allocates nothing
void drop(Node *node);  // delete node
Pooled *pooled();       // new Pooled: the class's own operator new
int untouched(int v);   // no new-expression at all
