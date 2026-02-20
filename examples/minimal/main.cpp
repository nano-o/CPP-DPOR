#include "dpor/api/session.hpp"

#include <iostream>

int main() {
  dpor::api::Session session{{"minimal-example", 256}};
  std::cout << session.describe() << '\n';
  return 0;
}
