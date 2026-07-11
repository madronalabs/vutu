
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <memory>
#include <vector>

#include "utuFFT.h"
#include "utuWindow.h"

namespace ml::utu
{

// One frame of reassigned short-time spectrum data, stored as
// struct-of-arrays over bins 0..N/2 (padded to a multiple of 4 floats,
// 16-byte aligned) and reused across hops.
struct SpectrumFrame
{
  long fftSize{0};
  long bins{0};  // N/2 + 1 valid entries in each array

  // half spectra of the three windowed signals
  std::vector<float> re, im;    // Xh:  x·w
  std::vector<float> dRe, dIm;  // Xd:  x·wFreqRamp
  std::vector<float> tRe, tIm;  // Xt:  x·wTimeRamp

  // per-bin reassignment data
  std::vector<float> magSq;     // |Xh|²
  std::vector<float> freqCorr;  // fractional bins
  std::vector<float> timeCorr;  // samples
};

// Time-frequency reassigned short-time spectrum, replacing Loris
// ReassignedSpectrum. Uses three real FFTs per hop instead of Loris's two
// complex ones; the mixed-derivative (convergence) spectrum is not computed
// since residue bandwidth association always overwrites it.
class ReassignedSpectrum
{
 public:
  void configure(ReassignmentWindows windows);

  long fftSize() const { return _frame.fftSize; }
  long bins() const { return _frame.bins; }
  long windowLength() const { return _windows.length; }
  float oversampling() const { return _oversampling; }

  // Transform with the analysis window centered on src[sampCenter];
  // window samples falling outside [0, srcLength) see zeros. The windowed
  // input is rotated so the window center lands at index 0, making spectral
  // phase zero-centered as in Loris.
  void transform(const float* src, long srcLength, long sampCenter);

  const SpectrumFrame& frame() const { return _frame; }

  // scalar accessors for candidate peaks, idx in [0, N/2]
  float magnitudeAt(long idx) const;

  // reassigned phase per Loris: linear interpolation of phase toward the
  // neighbor bin in the direction of the frequency correction, then a time
  // correction term, wrapped by fmod
  float phaseAt(long idx) const;

 private:
  float rawPhaseAt(long idx) const;
  void fillWindowed(const float* src, long begin, long count, long winOffset, long rotateBy,
                    const std::vector<float>& win);
  ReassignmentWindows _windows;
  std::unique_ptr<RealFFT> _fft;
  std::vector<float> _input;  // windowed, rotated, zero-padded FFT input
  float _oversampling{1.f};
  SpectrumFrame _frame;
};

}  // namespace ml::utu
