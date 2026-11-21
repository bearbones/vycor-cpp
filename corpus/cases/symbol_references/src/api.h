// Everything the two TUs share is declared here.
extern int counter;  // defined in store.cpp, read in main.cpp
void bump();         // defined in store.cpp, called from main.cpp
void only_here();    // defined in store.cpp, used by store.cpp alone
void on_event();     // defined in store.cpp, address taken in main.cpp

using Hook = void (*)();
void install(Hook);

struct Shape {
  virtual ~Shape(); // key function: store.cpp is the vtable's home
  virtual int area() const;
};
