#include "oamd-b2b.h"

#include <iostream>

int main()
{
    const eac3oamd::B2bSelfTestReport report = eac3oamd::runB2bSelfTest();
    std::cout << "oamdB2bSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases << '\n'
              << "DefaultCases=" << report.defaultCases << '\n'
              << "FullCases=" << report.fullCases << '\n'
              << "ReuseCases=" << report.reuseCases << '\n'
              << "MixedCases=" << report.mixedCases << '\n'
              << "DifferentialCases=" << report.differentialCases << '\n'
              << "ConversionCases=" << report.conversionCases << '\n'
              << "InactiveCases=" << report.inactiveCases << '\n'
              << "LfeHelperCases=" << report.lfeHelperCases << '\n'
              << "ResetCases=" << report.resetCases << '\n'
              << "TransactionalCases=" << report.transactionalCases << '\n'
              << "BoundaryCases=" << report.boundaryCases << '\n'
              << "GainCodeCases=" << report.gainCodeCases << '\n'
              << "FormulaBoundaryCases=" << report.formulaBoundaryCases << '\n'
              << "ZoneMappingCases=" << report.zoneMappingCases << '\n'
              << "BlockSnapshotCases=" << report.blockSnapshotCases << '\n'
              << "SnapCases=" << report.snapCases << '\n'
              << "MaxShapeCases=" << report.maxShapeCases << '\n'
              << "Reason=" << report.reason << '\n';
    return report.pass ? 0 : 1;
}
