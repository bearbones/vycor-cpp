#include "alloc.h"

static char pool[256];

void *Pooled::operator new(size_type) { return pool; }
void Pooled::operator delete(void *) {}

Node *build() { return new Node{1, nullptr}; }

int *grow(int n) { return new int[n]; }

Node *place(void *buf) { return new (buf) Node{2, nullptr}; }

void drop(Node *node) { delete node; }

Pooled *pooled() { return new Pooled; }

int untouched(int v) { return v + 1; }
