#pragma once

#include "joc-gate5a.h"
#include "joc-qmf.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3joctrace {

struct ReconstructionCheck {
    double maxAbs = 0.0;
    double relativeRms = 0.0;
    std::size_t mismatchCount = 0;
    std::size_t comparedCount = 0;
};

// Recomputes Qout from the captured Qin and interpolated matrix without
// calling the production reconstruction loop.
ReconstructionCheck verifyReconstruction(const eac3joc::JocMathFrame &math,
                                         const eac3joc::JocQmfFrame &qmf,
                                         double mismatchTolerance = 1.0e-6);

bool runSelfTest(std::string *reason);

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    Writer(Writer &&) = delete;
    Writer &operator=(Writer &&) = delete;

    bool open(const std::string &outputPath,
              const std::string &sourcePath,
              const std::vector<double> &qwin,
              std::string *reason);
    bool append(std::size_t unitIndex,
                std::int64_t sourceStart,
                const eac3joc::FrameReport &report,
                const eac3joc::JocMathFrame &math,
                const eac3joc::JocQmfFrame &qmf,
                const std::vector<float> &lfe,
                std::string *reason);
    bool close(std::string *reason);

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace eac3joctrace
