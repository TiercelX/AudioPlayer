#include "joc-qmf.h"

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
            std::cerr << "Usage: Eac3JocQmfProbe [--table path]\n";
            return 2;
        }
    }
    std::vector<double> qwin;
    std::string reason;
    if (!eac3qmf::loadQwin(tablePath, &qwin, &reason)) {
        std::cout << "jocQmfSyntheticSelfTest=FAIL cases=0 reason=" << reason << '\n'
                  << "jocQmfResult=FAIL stage=gate5c-object-qmf-synthetic\n";
        return 1;
    }
    const eac3joc::JocQmfSelfTestReport report = eac3joc::runQmfSelfTest(qwin);
    std::cout << "jocQmfSyntheticSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases << " reason=" << report.reason << '\n'
              << "jocQmfZeroMatrixCases=" << report.zeroMatrixCases << '\n'
              << "jocQmfSingleChannelCopyCases=" << report.singleChannelCopyCases << '\n'
              << "jocQmfIdentityLikeCases=" << report.identityLikeCases << '\n'
              << "jocQmfCancellationCases=" << report.cancellationCases << '\n'
              << "jocQmfResetEquivalenceCases=" << report.resetEquivalenceCases << '\n'
              << "jocQmfDimensionMismatchCases=" << report.dimensionMismatchCases << '\n'
              << "jocQmfNonFiniteRejectionCases=" << report.nonFiniteRejectionCases << '\n'
              << "jocQmfTransactionalRejectionCases=" << report.transactionalRejectionCases << '\n'
              << "jocQmfReservedConfigRejects=" << report.reservedConfigRejects << '\n'
              << "jocQmfConfigIdentityCases=" << report.configIdentityCases << '\n';
    std::cout << "jocQmfConfigIdentityOrder="
              << (report.configIdentityOrderPass ? "PASS" : "FAIL") << '\n'
              << "jocQmfConfigIdentityCounts=0:5,1:7,2:7,3:5,4:7\n"
              << "jocQmfReservedConfigs=5,6,7\n"
              << "jocQmfResult=" << (report.pass ? "PASS" : "FAIL")
              << " stage=gate5c-object-qmf-synthetic\n";
    return report.pass ? 0 : 1;
}
