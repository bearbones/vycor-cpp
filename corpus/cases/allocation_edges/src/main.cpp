#include "alloc.h"

// main reaches the allocating operator new two calls down, through
// make_list -> build.
static Node *make_list() {
  Node *head = build();
  head->next = build();
  return head;
}

int main() {
  char storage[sizeof(Node)];
  Node *list = make_list();
  Node *placed = place(storage);
  int total = list->value + placed->value + untouched(3);
  drop(list->next);
  drop(list);
  return total;
}
