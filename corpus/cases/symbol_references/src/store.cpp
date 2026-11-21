#include "api.h"

int counter = 0;
static int scale = 2;

void only_here() { counter = counter * scale; }
void bump() {
  only_here();
  ++counter;
}
void on_event() {}
void install(Hook hook) { hook(); }

Shape::~Shape() {}
int Shape::area() const { return 0; }
