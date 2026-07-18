
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Implemented from Fitz & Fulop, "A Unified Theory of Time-Frequency
// Reassignment" (eqs. 64-65 for the corrections, Sec. 8 for phase); no
// Loris source consulted. The reassignment signs and scalings are pinned by
// the utucompare selftests: an off-bin sine must reassign toward its true
// frequency (sine, chirp), and an impulse Δ samples after the frame center
// must measure timeCorr = +Δ exactly (impulse-time).

#include "utuSpectrum.h"

#include <algorithm>
#include <cmath>

namespace ml::utu
{

namespace
{

constexpr float kTwoPi = 6.28318530717958648f;

// the reassignment quotients are undefined where the spectrogram is zero
// (the paper's zero-valued-distribution caveat); clamp the denominator
constexpr float kMinMagSq = 1e-30f;

long nextPow2AtLeastTwice(long n)
{
  long N = 1;
  while (N < 2 * n) N <<= 1;
  return N;
}

// SoA arrays padded to a multiple of 4 floats for vector-friendly loops
size_t padded(long n) { return (size_t(n) + 3) & ~size_t(3); }

}  // namespace

void ReassignedSpectrum::configure(ReassignmentWindows windows)
{
  _windows = std::move(windows);
  const long N = nextPow2AtLeastTwice(_windows.length);
  _fft = std::make_unique<RealFFT>(N);
  _oversampling = float(N) / float(_windows.length);
  _input.assign(N, 0.f);

  _frame.fftSize = N;
  _frame.bins = N / 2 + 1;
  const size_t p = padded(_frame.bins);
  for (auto* v : {&_frame.re, &_frame.im, &_frame.dRe, &_frame.dIm, &_frame.tRe, &_frame.tIm,
                  &_frame.magSq, &_frame.freqCorr, &_frame.timeCorr})
  {
    v->assign(p, 0.f);
  }
}

// Window the source about sampCenter and rotate so the window's center
// sample lands at FFT index 0: the transform then reports phase relative to
// the frame center (the STFT convention of eq. 3) instead of the moving-
// window convention of eq. 5, whose phases rotate at ω (eq. 13) — one array
// rotation replaces a per-bin phase twist. Samples outside [0, srcLength)
// are zeros.
void ReassignedSpectrum::fillWindowed(const float* src, long srcLength, long sampCenter,
                                      const std::vector<float>& win)
{
  const long N = _frame.fftSize;
  const long len = _windows.length;
  const long half = (len - 1) / 2;
  std::fill(_input.begin(), _input.end(), 0.f);

  // window sample j multiplies src[sampCenter − half + j]
  const long jBegin = std::max(long(0), half - sampCenter);
  const long jEnd = std::min(len, srcLength - sampCenter + half);
  const float* x = src + sampCenter - half;
  for (long j = jBegin; j < std::min(jEnd, half); ++j)
  {
    _input[N - half + j] = x[j] * win[j];  // left of center → buffer tail
  }
  for (long j = std::max(jBegin, half); j < jEnd; ++j)
  {
    _input[j - half] = x[j] * win[j];  // center and right → buffer head
  }
}

void ReassignedSpectrum::transform(const float* src, long srcLength, long sampCenter)
{
  fillWindowed(src, srcLength, sampCenter, _windows.w);
  _fft->forward(_input.data(), _frame.re.data(), _frame.im.data());
  fillWindowed(src, srcLength, sampCenter, _windows.wFreqRamp);
  _fft->forward(_input.data(), _frame.dRe.data(), _frame.dIm.data());
  fillWindowed(src, srcLength, sampCenter, _windows.wTimeRamp);
  _fft->forward(_input.data(), _frame.tRe.data(), _frame.tIm.data());

  // Per-bin reassignment algebra on the three spectra. With the windows
  // applied as a correlation (h(τ−t), not the paper's h(t−τ)):
  //
  //   S_hd(ν) = −iν·S_h(ν) for the derivative window, so
  //   Im{Xd·Xh*}/|Xh|² = ω_bin − ω_true — the paper's eq. 65 correction
  //   with its sign flipped, hence the leading minus. ×N/len finishes the
  //   window's len/2π into the rad/sample → fractional-bin conversion.
  //
  //   An impulse p samples after the frame center gives Xt·Xh* = p·|Xh|²
  //   real and positive, so eq. 64's t̂ − t is +Re{Xt·Xh*}/|Xh|² in
  //   samples, with no sign flip.
  const long bins = _frame.bins;
  const float ovs = _oversampling;
  const float* re = _frame.re.data();
  const float* im = _frame.im.data();
  const float* dRe = _frame.dRe.data();
  const float* dIm = _frame.dIm.data();
  const float* tRe = _frame.tRe.data();
  const float* tIm = _frame.tIm.data();
  float* magSq = _frame.magSq.data();
  float* freqCorr = _frame.freqCorr.data();
  float* timeCorr = _frame.timeCorr.data();
  for (long k = 0; k < bins; ++k)
  {
    const float m = re[k] * re[k] + im[k] * im[k];
    magSq[k] = m;
    const float d = std::max(m, kMinMagSq);
    freqCorr[k] = -ovs * (dIm[k] * re[k] - dRe[k] * im[k]) / d;
    timeCorr[k] = (tRe[k] * re[k] + tIm[k] * im[k]) / d;
  }
}

float ReassignedSpectrum::magnitudeAt(long idx) const { return sqrtf(_frame.magSq[idx]); }

float ReassignedSpectrum::rawPhaseAt(long idx) const
{
  // conjugate symmetry extends the half spectrum one bin past either end
  long i = idx;
  float sign = 1.f;
  if (idx < 0)
  {
    i = -idx;
    sign = -1.f;
  }
  else if (idx > _frame.fftSize / 2)
  {
    i = _frame.fftSize - idx;
    sign = -1.f;
  }
  return sign * atan2f(_frame.im[i], _frame.re[i]);
}

// Phase at the reassigned coordinates (Sec. 8), evaluated only at accepted
// peaks. Frequency: the analysis filters are linear-phase across a
// passband, so interpolate the raw phase linearly toward the neighbor bin
// in the direction of the frequency correction. Time: advance by the travel
// over the reassignment interval, ω̂·(t̂ − t).
float ReassignedSpectrum::phaseAt(long idx) const
{
  const float fc = _frame.freqCorr[idx];
  const float tc = _frame.timeCorr[idx];
  float p = rawPhaseAt(idx);
  if (fc > 0.f)
  {
    p += fc * (rawPhaseAt(idx + 1) - p);
  }
  else
  {
    p += fc * (p - rawPhaseAt(idx - 1));
  }
  p += tc * (float(idx) + fc) * (kTwoPi / _frame.fftSize);
  return fmodf(p, kTwoPi);
}

}  // namespace ml::utu
