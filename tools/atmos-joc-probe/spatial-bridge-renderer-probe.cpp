#include "spatial-bridge-renderer.h"

#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test") {
        std::cerr << "Usage: Eac3SpatialBridgeRendererProbe --self-test\n";
        return 2;
    }
    const auto report = eac3renderer::runSelfTest();
    std::cout << "selfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases << " reason="
              << (report.reason.empty() ? "none" : report.reason) << '\n'
              << "gate7cRendererResult=" << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=endpoint-free-renderer-contract-only\n";
    return report.pass ? 0 : 1;
}
