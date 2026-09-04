#include "radix2-fft.h"
#include <cmath>
#include <stdexcept>

namespace eac3render {
Radix2Fft::Radix2Fft(std::size_t n) : twiddle_(n / 2), bitReverse_(n), data_(n) {
    if (n < 2 || (n & (n - 1)) != 0) throw std::invalid_argument("fft-size-not-power-of-two");
    const float pi = 3.14159265358979323846F;
    for (std::size_t k = 0; k < n / 2; ++k) twiddle_[k] = std::polar(1.0F, -2.0F * pi * float(k) / float(n));
    std::size_t bits = 0; for (std::size_t x = n; x > 1; x >>= 1) ++bits;
    for (std::size_t i = 0; i < n; ++i) { std::size_t r = 0; for (std::size_t b = 0; b < bits; ++b) r = (r << 1) | ((i >> b) & 1U); bitReverse_[i] = r; }
}
void Radix2Fft::forward(std::complex<float>* x) const {
    for (std::size_t i=0;i<size();++i) data_[i]=x[bitReverse_[i]];
    for (std::size_t len=2;len<=size();len<<=1) for (std::size_t i=0;i<size();i+=len) for (std::size_t j=0;j<len/2;++j) { auto u=data_[i+j], v=data_[i+j+len/2]*twiddle_[j*size()/len]; data_[i+j]=u+v; data_[i+j+len/2]=u-v; }
    for (std::size_t i=0;i<size();++i)x[i]=data_[i];
}
void Radix2Fft::inverse(std::complex<float>* x) const { for(std::size_t i=0;i<size();++i)x[i]=std::conj(x[i]); forward(x); for(std::size_t i=0;i<size();++i)x[i]=std::conj(x[i])/float(size()); }
} // namespace eac3render
