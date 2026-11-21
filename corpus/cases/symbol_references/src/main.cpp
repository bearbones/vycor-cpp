#include "api.h"

int peek() { return counter; }

int main() {
  bump();
  install(&on_event);
  Shape shape;
  return peek() + shape.area();
}
