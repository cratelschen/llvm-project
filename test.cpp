#include "./x_add/add.h"
#include <iostream>

using namespace std;
class A {
public:
  A() { std::cout << "ctor" << std::endl; }

  A(const A &a) { std::cout << "copy ctor" << std::endl; }

  A(A &&a) noexcept { std::cout << "move ctor" << std::endl; }
};

A getA() {
  int i = 1;
  if (i) {
    return A();
  }
  return A();
}

int main(int argc, const char **argv) {

  int i = add(1, 2);
  std::cout << i << std::endl;

  A act(getA());

  return 0;
}
