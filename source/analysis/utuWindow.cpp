
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Implemented from Fitz & Fulop, "A Unified Theory of Time-Frequency
// Reassignment" (Sec. 6.2) and the published Kaiser window formulas of
// Kaiser & Schafer 1980; no Loris source consulted. Correctness is pinned
// by the utucompare selftests (window-shape, window-hd), which cross-check
// the analytic derivative window against the paper's eq. 70 construction
// and an 8th-order finite difference.

#include "utuWindow.h"

#include <cmath>

namespace ml::utu
{

namespace
{

constexpr double kPi = 3.14159265358979324;

// modified Bessel functions of the first kind by power series, in double.
// The series is stopped at 1e-12 relative, well below the float precision
// of the stored window tables.

double besselI0(double x)
{
  const double q = 0.25 * x * x;
  double term = 1., sum = 1.;
  for (int k = 1; k < 1000; ++k)
  {
    term *= q / (double(k) * k);
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

double besselI1(double x)
{
  // I1(x) = (x/2)·Σ (x²/4)^k / (k!·(k+1)!)
  const double q = 0.25 * x * x;
  double term = 1., sum = 1.;
  for (int k = 1; k < 1000; ++k)
  {
    term *= q / (double(k) * (k + 1));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return 0.5 * x * sum;
}

}  // namespace

namespace kaiser
{

// Kaiser & Schafer 1980: shape parameter β for a requested sidelobe
// attenuation (dB) of the window transform
double computeShape(double attenDb)
{
  if (attenDb > 60.)
  {
    return 0.12438 * (attenDb + 6.3);
  }
  if (attenDb > 13.26)
  {
    return 0.76609 * pow(attenDb - 13.26, 0.4) + 0.09834 * (attenDb - 13.26);
  }
  return 0.;
}

// The Kaiser window transform's first nulls sit at ±2·sqrt(π²+β²)/N
// rad/sample, so a main lobe spanning `widthOverSR` cycles/sample between
// nulls (2π·widthOverSR radians) needs N = 2·sqrt(π²+β²)/(π·widthOverSR)
// samples; +1 and forced odd so the window has an exact center sample.
long computeLength(double widthOverSR, double shape)
{
  const long len = long(1. + 2. * sqrt(kPi * kPi + shape * shape) / (kPi * widthOverSR));
  return len | 1;
}

// h[n] = I0(β·sqrt(1−K²)) / I0(β),  K = 2n/(len−1) − 1 ∈ [−1, 1]
void buildWindow(std::vector<double>& win, double shape)
{
  const long len = long(win.size());
  const double i0Shape = besselI0(shape);
  const double c = 0.5 * (len - 1);
  for (long n = 0; n < len; ++n)
  {
    const double K = (n - c) / c;
    const double arg = sqrt(std::max(0., 1. - K * K));
    win[n] = besselI0(shape * arg) / i0Shape;
  }
}

// dh/dn analytically, using d/dx I0(x) = I1(x):
//   dh/dn = −(2β / ((len−1)·I0(β))) · I1(β·a)·K/a,  a = sqrt(1−K²),
// with the K → ±1 endpoint limit I1(β·a)/a → β/2. This replaces the
// FFT-based construction of eq. 70 (the selftest verifies both agree).
void buildTimeDerivativeWindow(std::vector<double>& win, double shape)
{
  const long len = long(win.size());
  const double i0Shape = besselI0(shape);
  const double c = 0.5 * (len - 1);
  const double fac = -2. * shape / ((len - 1.) * i0Shape);
  for (long n = 0; n < len; ++n)
  {
    const double K = (n - c) / c;
    const double arg = sqrt(std::max(0., 1. - K * K));
    const double slope = (arg > 1e-9) ? besselI1(shape * arg) / arg : 0.5 * shape;
    win[n] = fac * slope * K;
  }
}

}  // namespace kaiser

// The three windows of Auger-Flandrin reassignment (Sec. 6.2), scaled as
// documented in the header: w reads sinusoid amplitude directly off |X|,
// wFreqRamp carries len/2π of the rad/sample → fractional-bin conversion
// (the spectrum kernel's N/len supplies the rest), and wTimeRamp's ramp is
// in samples so eq. 64 needs no further scaling.
ReassignmentWindows buildReassignmentWindows(double sampleRate, double windowWidthHz,
                                             double sidelobeDb)
{
  ReassignmentWindows out;
  const double shape = kaiser::computeShape(sidelobeDb);
  const long len = kaiser::computeLength(windowWidthHz / sampleRate, shape);

  std::vector<double> h(len), hd(len);
  kaiser::buildWindow(h, shape);
  kaiser::buildTimeDerivativeWindow(hd, shape);

  double winsum = 0.;
  for (double v : h) winsum += v;
  const double magScale = 2. / winsum;  // 2 = analytic-signal factor
  const double freqScale = len / (winsum * kPi);
  const double center = 0.5 * (len - 1);

  out.length = len;
  out.w.resize(len);
  out.wFreqRamp.resize(len);
  out.wTimeRamp.resize(len);
  for (long n = 0; n < len; ++n)
  {
    out.w[n] = float(magScale * h[n]);
    out.wFreqRamp[n] = float(freqScale * hd[n]);
    out.wTimeRamp[n] = float(magScale * h[n] * (n - center));
  }
  return out;
}

}  // namespace ml::utu
