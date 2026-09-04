#include "joc-synthesis.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::string tablePath = "docs/dev/ts_103420_tables.c";
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--table" && index + 1 < argc) {
            tablePath = argv[++index];
        } else {
            std::cerr << "Usage: Eac3JocSynthesisProbe [--table path]\n";
            return 2;
        }
    }

    std::vector<double> qwin;
    std::string reason;
    if (!eac3qmf::loadQwin(tablePath, &qwin, &reason)) {
        std::cout << "jocSynthesisSelfTest=FAIL cases=0 reason=" << reason << '\n'
                  << "jocSynthesisResult=FAIL stage=gate6a-object-qmf-synthesis\n";
        return 1;
    }
    const eac3joc::JocSynthesisSelfTestReport report =
        eac3joc::runSynthesisSelfTest(qwin);
    std::cout << "jocSynthesisSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases << " reason=" << report.reason << '\n'
              << "jocSynthesisZeroQmfCases=" << report.zeroQmfCases << '\n'
              << "jocSynthesisSingleObjectCases=" << report.singleObjectCases << '\n'
              << "jocSynthesisMultipleObjectCases=" << report.multipleObjectCases << '\n'
              << "jocSynthesisSplitCases=" << report.splitCases << '\n'
              << "jocSynthesisResetCases=" << report.resetCases << '\n'
              << "jocSynthesisObjectCountResetCases=" << report.objectCountResetCases << '\n'
              << "jocSynthesisTransactionalRejectionCases="
              << report.transactionalRejectionCases << '\n'
              << "jocSynthesisExactSizeCases=" << report.exactSizeCases << '\n'
              << "jocSynthesisSixteenObjectCases=" << report.sixteenObjectCases << '\n'
              << "jocSynthesisBoundaryRejectionCases="
              << report.boundaryRejectionCases << '\n'
              << "jocSynthesisBoundaryTransactionalCases="
              << report.boundaryTransactionalCases << '\n'
              << "jocSynthesisAlgorithmicDelaySamples="
              << eac3joc::kSynthesisAlgorithmicDelaySamples << '\n'
              << "jocSynthesisResult=" << (report.pass ? "PASS" : "FAIL")
              << " stage=gate6a-object-qmf-synthesis\n";
    return report.pass ? 0 : 1;
}
