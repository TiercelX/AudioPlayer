#include "joc-gate6c.h"
#include "qmf-bank.h"

#include <iostream>

int main(int argc, char **argv)
{
    std::string tablePath = argc > 1 ? argv[1] : "docs/dev/ts_103420_tables.c";
    std::vector<double> qwin;
    std::string reason;
    if (!eac3qmf::loadQwin(tablePath, &qwin, &reason)) {
        std::cout << "gate6cQmfPrototype=FAIL reason=" << reason << '\n';
        return 1;
    }
    const eac3gate6c::Report report = eac3gate6c::runSelfTest(qwin);
    std::cout << "gate6cSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases << '\n'
              << "gate6cResetCases=" << report.resetCases << '\n'
              << "gate6cMappingCases=" << report.mappingCases << '\n'
              << "gate6cTimingCases=" << report.timingCases << '\n'
              << "gate6cMetadataCases=" << report.metadataCases << '\n'
              << "gate6cFlushCases=" << report.flushCases << '\n'
              << "gate6cCallbackCases=" << report.callbackCases << '\n'
              << "gate6cTransactionalCases=" << report.transactionalRejects << '\n'
              << "gate6cConversionCases=" << report.conversionCases << '\n'
              << "gate6cReason=" << report.reason << '\n'
              << "gate6cAlgorithmicDelaySamples="
              << eac3gate6c::kCommonDelaySamples << '\n'
              << "gate6cResult=" << (report.pass ? "PASS" : "INCONCLUSIVE")
              << " stage=gate6c-renderer-neutral-timeline\n";
    return report.pass ? 0 : 1;
}
