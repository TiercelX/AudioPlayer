#pragma once

#include "joc-gate6c.h"
#include <cstddef>
#include <string>

namespace eac3bear {
class Exporter {
public:
    explicit Exporter(const std::string &directory,
                      const std::string &sourcePath = {});
    bool open(std::string *reason);
    bool append(const eac3gate6c::Batch &batch, std::string *reason);
    bool finish(std::string *reason);
    std::size_t batches() const { return batches_; }
    std::size_t metadata() const { return metadata_; }
private:
    std::string dir_;
    std::string sourcePath_;
    std::size_t batches_ = 0, metadata_ = 0;
    bool opened_ = false;
};
}
