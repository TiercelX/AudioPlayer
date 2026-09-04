#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace eac3render {

class Radix2Fft {
public:
    explicit Radix2Fft(std::size_t size);
    std::size_t size() const noexcept { return data_.size(); }
    void forward(std::complex<float>* data) const;
    void inverse(std::complex<float>* data) const;
private:
    std::vector<std::complex<float>> twiddle_;
    std::vector<std::size_t> bitReverse_;
    mutable std::vector<std::complex<float>> data_;
};

} // namespace eac3render
