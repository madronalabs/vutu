
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuFFT.h"

#include <cassert>

namespace ml::utu
{

namespace
{
bool isPowerOfTwo(long n) { return (n > 0) && ((n & (n - 1)) == 0); }
}  // namespace

RealFFT::RealFFT(long length) : _length(length), _fft(length), _packed(length)
{
  assert(isPowerOfTwo(length) && (length >= 8));
}

void RealFFT::forward(const float* in, float* re, float* im)
{
  // FFTReal packs the half spectrum as [Re(0)..Re(N/2), Im(1)..Im(N/2-1)],
  // with Im stored for the exp(+i) convention; negate for exp(-i).
  _fft.do_fft(_packed.data(), in);

  const long half = _length / 2;
  for (long k = 0; k <= half; ++k)
  {
    re[k] = _packed[k];
  }
  im[0] = 0.f;
  im[half] = 0.f;
  for (long k = 1; k < half; ++k)
  {
    im[k] = -_packed[half + k];
  }
}

}  // namespace ml::utu
