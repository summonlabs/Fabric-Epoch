// Fabric Epoch 1.0.0 - Summon Software Labs
#include <iostream>
#include <string>

#include "test_harness.hpp"

int main(int argc, char** argv) {
  std::string filter;
  std::string skip;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument.rfind("--skip=", 0) == 0) {
      skip = argument.substr(7);
    } else {
      std::cerr << "unknown argument: " << argument << std::endl;
      return 2;
    }
  }
  const int failures = fe_test::run_all(filter, skip);
  if (failures != 0) {
    std::cout << "RESULT FAILED failures=" << failures << std::endl;
    return 1;
  }
  std::cout << "RESULT OK" << std::endl;
  return 0;
}
