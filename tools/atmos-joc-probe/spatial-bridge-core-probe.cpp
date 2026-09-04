#include "spatial-bridge-core.h"

#include <iostream>

int main()
{
    const eac3bridge::SelfTestReport report = eac3bridge::runSelfTest();
    std::cout << "selfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases << " reason="
              << (report.reason.empty() ? "none" : report.reason) << '\n'
              << "queueStatusTexts=" << eac3bridge::queueStatusText(
                     eac3bridge::QueueStatus::Pass)
              << ',' << eac3bridge::queueStatusText(eac3bridge::QueueStatus::Timeout)
              << ',' << eac3bridge::queueStatusText(eac3bridge::QueueStatus::Closed)
              << ',' << eac3bridge::queueStatusText(eac3bridge::QueueStatus::Canceled)
              << '\n'
              << "renderStatusTexts=" << eac3bridge::renderStatusText(
                     eac3bridge::RenderStatus::Pass)
              << ',' << eac3bridge::renderStatusText(eac3bridge::RenderStatus::NotReady)
              << ',' << eac3bridge::renderStatusText(eac3bridge::RenderStatus::EndOfStream)
              << '\n'
              << "gate7cResult=" << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=offline-bridge-core-only\n";
    return report.pass ? 0 : 1;
}
