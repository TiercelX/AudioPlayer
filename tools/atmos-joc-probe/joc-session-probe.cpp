#include "joc-session.h"
#include "qmf-bank.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    const std::string tablePath = argc > 1 ? argv[1] : "docs/dev/ts_103420_tables.c";
    std::vector<double> qwin;
    std::string reason;
    if (!eac3qmf::loadQwin(tablePath, &qwin, &reason)) {
        std::cout << "jocSessionQmfPrototype=FAIL reason=" << reason << '\n';
        return 1;
    }
    const eac3jocsession::SelfTestReport report =
        eac3jocsession::runSelfTest(qwin);
    std::cout << "jocSessionSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " coreCases=" << report.coreCases
              << " sessionCases=" << report.sessionCases
              << " gate6cCases=" << report.gate6cCases
              << " reason=" << report.reason << '\n'
              << "jocSessionResult=" << (report.pass ? "PASS" : "INCONCLUSIVE")
              << " stage=gate8a-1-injected-core-session"
              << " evidenceLimit=no-libav-packet-adapter" << '\n';
    return report.pass ? 0 : 1;
}
