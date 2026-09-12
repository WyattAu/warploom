#include <sys/resource.h>

#include "engine/core/contract.hpp"

int main() {
  // Keep the abort fast: no 35 MB core dump through systemd-coredump on
  // every test run. The violation diagnostic (stderr) is the contract.
  struct rlimit zero { 0, 0 };
  setrlimit(RLIMIT_CORE, &zero);
  OMNICPP_CONTRACT(1 == 2);
  return 0;
}
