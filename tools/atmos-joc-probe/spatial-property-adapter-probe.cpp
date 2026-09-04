#include "spatial-property-adapter.h"

#include <iomanip>
#include <iostream>

int main()
{
    const eac3gate7b::Geometry geometry = eac3gate7b::referenceGeometry();
    std::string reason;
    const bool geometryPass = eac3gate7b::validateGeometry(geometry, &reason);
    const eac3gate7b::SelfTestReport report = eac3gate7b::runSelfTest();
    std::cout << std::fixed << std::setprecision(6)
              << "referenceGeometry=room(" << geometry.roomWidthMetres << ','
              << geometry.roomDepthMetres << ',' << geometry.roomHeightMetres
              << ") listener(" << geometry.listenerX << ',' << geometry.listenerY
              << ',' << geometry.listenerZ << ") screenBottomLeft("
              << geometry.screenBottomLeftX << ',' << geometry.screenBottomLeftY
              << ',' << geometry.screenBottomLeftZ << ") screen(" << geometry.screenWidth
              << ',' << geometry.screenHeight << ") headroomDb="
              << geometry.gainHeadroomDb << '\n'
              << "geometryValidation=" << (geometryPass ? "PASS" : "FAIL")
              << " reason=" << (geometryPass ? "reference" : reason) << '\n'
              << "selfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases << " reason="
              << (report.reason.empty() ? "none" : report.reason) << '\n'
              << "gate7bResult=" << (geometryPass && report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=offline-property-adapter-only\n";
    return geometryPass && report.pass ? 0 : 1;
}
