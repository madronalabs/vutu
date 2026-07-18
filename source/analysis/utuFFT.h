
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

#include "ffft/FFTReal.h"

namespace ml::utu
{

// Real-input forward FFT for power-of-two lengths, wrapping the vendored
// FFTReal (Laurent de Soras). Emits bins 0..N/2 as separate re/im arrays
// using the convention X[k] = sum x[n]·exp(-i·2π·k·n/N), matching FFTW,
// with no output scaling.
class RealFFT
{
 public:
  explicit RealFFT(long length);

  long length() const { return _length; }
  long bins() const { return _length / 2 + 1; }

  // in: length() samples. re, im: bins() floats each. im[0] = im[N/2] = 0.
  void forward(const float* in, float* re, float* im);

 private:
  long _length;
  ffft::FFTReal<float> _fft;
  std::vector<float> _packed;
};

}  // namespace ml::utu
