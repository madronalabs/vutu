
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuSpectrum.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "mldsp.h"

namespace ml::utu
{

namespace
{
constexpr float kTwoPiF = 6.28318530717958648f;

long nextPowerOfTwoAtLeastTwice(long n)
{
  long p = 1;
  while (p < 2 * n) p <<= 1;
  return p;
}

size_t padTo4(long n) { return static_cast<size_t>((n + 3) & ~3L); }
}  // namespace

void ReassignedSpectrum::configure(ReassignmentWindows windows)
{
  _windows = std::move(windows);

  const long n = nextPowerOfTwoAtLeastTwice(_windows.length);
  _fft = std::make_unique<RealFFT>(n);
  _input.assign(n, 0.f);
  _oversampling = static_cast<float>(n) / _windows.length;

  _frame.fftSize = n;
  _frame.bins = n / 2 + 1;
  const size_t padded = padTo4(_frame.bins);
  for (auto* v : {&_frame.re, &_frame.im, &_frame.dRe, &_frame.dIm, &_frame.tRe, &_frame.tIm,
                  &_frame.magSq, &_frame.freqCorr, &_frame.timeCorr})
  {
    v->assign(padded, 0.f);
  }
}

// multiply src[begin..begin+count) by win[winOffset..) and write the products
// into the FFT input at their post-rotation positions: input[(i - rotateBy) mod N]
void ReassignedSpectrum::fillWindowed(const float* src, long begin, long count, long winOffset,
                                      long rotateBy, const std::vector<float>& win)
{
  const long n = _frame.fftSize;
  float* buf = _input.data();
  const float* x = src + begin;
  const float* w = win.data() + winOffset;

  // i in [0, rotateBy) lands at the end of the buffer, the rest at the front
  float* tail = buf + (n - rotateBy);
  for (long i = 0; i < rotateBy; ++i)
  {
    tail[i] = x[i] * w[i];
  }
  for (long i = rotateBy; i < count; ++i)
  {
    buf[i - rotateBy] = x[i] * w[i];
  }
}

void ReassignedSpectrum::transform(const float* src, long srcLength, long sampCenter)
{
  assert(_fft);
  assert((sampCenter >= 0) && (sampCenter < srcLength));

  const long half = _windows.length / 2;  // length is odd; both halves are equal
  const long begin = std::max(0L, sampCenter - half);
  const long end = std::min(srcLength, sampCenter + half + 1);
  const long count = end - begin;
  const long winOffset = half - (sampCenter - begin);
  const long rotateBy = sampCenter - begin;

  SpectrumFrame& f = _frame;
  struct Pass
  {
    const std::vector<float>* win;
    float *re, *im;
  };
  const Pass passes[3] = {{&_windows.w, f.re.data(), f.im.data()},
                          {&_windows.wFreqRamp, f.dRe.data(), f.dIm.data()},
                          {&_windows.wTimeRamp, f.tRe.data(), f.tIm.data()}};
  for (const Pass& p : passes)
  {
    std::memset(_input.data(), 0, _input.size() * sizeof(float));
    fillWindowed(src, begin, count, winOffset, rotateBy, *p.win);
    _fft->forward(_input.data(), p.re, p.im);
  }

  // per-bin reassignment kernels, 4 bins at a time:
  //   magSq    = |Xh|²
  //   freqCorr = -(N/winlen)·(Xh.re·Xd.im − Xh.im·Xd.re) / |Xh|²
  //   timeCorr = (Xh.re·Xt.re + Xh.im·Xt.im) / |Xh|²
  // |Xh|² is clamped away from zero: garbage corrections in spectral-floor
  // bins only produce candidates that the amplitude threshold rejects.
  const float4 vNegOversampling(-_oversampling);
  const float4 vFloor(1e-30f);
  const size_t padded = f.re.size();
  for (size_t i = 0; i < padded; i += 4)
  {
    const float4 hr = loadFloat4(f.re.data() + i);
    const float4 hi = loadFloat4(f.im.data() + i);
    const float4 dr = loadFloat4(f.dRe.data() + i);
    const float4 di = loadFloat4(f.dIm.data() + i);
    const float4 tr = loadFloat4(f.tRe.data() + i);
    const float4 ti = loadFloat4(f.tIm.data() + i);

    const float4 magSq = multiplyAdd(hr, hr, hi * hi);
    const float4 denom = max(magSq, vFloor);
    const float4 freqCorr = vNegOversampling * (hr * di - hi * dr) / denom;
    const float4 timeCorr = multiplyAdd(hr, tr, hi * ti) / denom;

    storeFloat4(f.magSq.data() + i, magSq);
    storeFloat4(f.freqCorr.data() + i, freqCorr);
    storeFloat4(f.timeCorr.data() + i, timeCorr);
  }
}

float ReassignedSpectrum::magnitudeAt(long idx) const
{
  assert((idx >= 0) && (idx < _frame.bins));
  return sqrtf(_frame.magSq[idx]);
}

// phase of Xh at idx, mirroring bins above N/2 by conjugate symmetry
// (phaseAt can look one bin past the last)
float ReassignedSpectrum::rawPhaseAt(long idx) const
{
  const long n = _frame.fftSize;
  if (idx < 0) idx += n;
  if (idx < _frame.bins)
  {
    return atan2f(_frame.im[idx], _frame.re[idx]);
  }
  const long mirror = n - idx;
  return atan2f(-_frame.im[mirror], _frame.re[mirror]);
}

float ReassignedSpectrum::phaseAt(long idx) const
{
  assert((idx >= 0) && (idx < _frame.bins));
  float phase = rawPhaseAt(idx);
  const float offsetTime = _frame.timeCorr[idx];
  const float offsetFreq = _frame.freqCorr[idx];

  // phase is locally linear; interpolate toward the neighbor bin in the
  // direction of the frequency correction
  if (offsetFreq > 0)
  {
    const float slope = rawPhaseAt(idx + 1) - phase;
    phase += offsetFreq * slope;
  }
  else
  {
    const float slope = phase - rawPhaseAt(idx - 1);
    phase += offsetFreq * slope;
  }

  // shift phase to the reassigned time
  const float fracFreqSample = idx + offsetFreq;
  phase += offsetTime * fracFreqSample * kTwoPiF / _frame.fftSize;

  return fmodf(phase, kTwoPiF);
}

}  // namespace ml::utu
