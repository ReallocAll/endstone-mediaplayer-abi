#include "runtime_probe.h"

#include <iostream>

int main()
{
    const auto result = abi_probe::run_measurement_self_test();
    for (const auto &failure : result.failures) {
        std::cerr << failure << '\n';
    }
    std::cout << "abi_probe_measurement_self_test unresolved=" << result.unresolved
              << " valid=" << (result.valid ? "true" : "false") << '\n';
    return result.valid ? 0 : 1;
}
