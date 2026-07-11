
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuWindow.h"

#include <cassert>
#include <cmath>
#include <numeric>

namespace ml::utu
{

namespace kaiser
{

namespace
{
constexpr double kPi = 3.14159265358979324;

// zeroeth order modified Bessel function of the first kind, by series
// expansion, as in Loris
double zeroethOrderBessel(double x)
{
  const double eps = 0.000001;
  double besselValue = 0;
  double term = 1;
  double m = 0;
  while (term > eps * besselValue)
  {
    besselValue += term;
    ++m;
    term *= (x * x) / (4 * m * m);
  }
  return besselValue;
}

// first order modified Bessel function of the first kind
double firstOrderBessel(double x)
{
  const double eps = 0.000001;
  double besselValue = 0;
  double term = .5 * x;
  double m = 0;
  while (term > eps * besselValue)
  {
    besselValue += term;
    ++m;
    term *= (x * x) / (4 * m * (m + 1));
  }
  return besselValue;
}
}  // namespace

double computeShape(double attenDb)
{
  assert(attenDb >= 0.);
  double alpha;
  if (attenDb > 60.0)
  {
    alpha = 0.12438 * (attenDb + 6.3);
  }
  else if (attenDb > 13.26)
  {
    alpha = 0.76609L * (pow((attenDb - 13.26), 0.4)) + 0.09834L * (attenDb - 13.26L);
  }
  else
  {
    // can't have less than 13dB attenuation
    alpha = 0.0;
  }
  return alpha;
}

long computeLength(double widthOverSR, double shape)
{
  // the +1 in Kaiser and Schafer 1980 (eq. 9) acts as a cheap ceiling
  return long(1.0 + (2. * sqrt((kPi * kPi) + (shape * shape)) / (kPi * widthOverSR)));
}

void buildWindow(std::vector<double>& win, double shape)
{
  const double oneOverDenom = 1.0 / zeroethOrderBessel(shape);
  const unsigned N = static_cast<unsigned>(win.size() - 1);
  const double oneOverN = 1.0 / N;
  for (unsigned n = 0; n <= N; ++n)
  {
    const double K = (2.0 * n * oneOverN) - 1.0;
    const double arg = sqrt(1.0 - (K * K));
    win[n] = zeroethOrderBessel(shape * arg) * oneOverDenom;
  }
}

void buildTimeDerivativeWindow(std::vector<double>& win, double shape)
{
  // d/dx I0(x) = I1(x)
  const unsigned N = static_cast<unsigned>(win.size() - 1);
  const double oneOverN = 1.0 / N;
  const double commonFac = -2.0 * shape / (N * zeroethOrderBessel(shape));
  win[0] = win[N] = 0.0;
  for (unsigned n = 1; n < N; ++n)
  {
    const double K = (2.0 * n * oneOverN) - 1.0;
    const double arg = sqrt(1.0 - (K * K));
    win[n] = commonFac * firstOrderBessel(shape * arg) * K / arg;
  }
}

}  // namespace kaiser

ReassignmentWindows buildReassignmentWindows(double sampleRate, double windowWidthHz,
                                             double sidelobeDb)
{
  constexpr double kPi = 3.14159265358979324;

  const double shape = kaiser::computeShape(sidelobeDb);
  long len = kaiser::computeLength(windowWidthHz / sampleRate, shape);
  if (!(len % 2)) ++len;

  std::vector<double> win(len);
  kaiser::buildWindow(win, shape);
  std::vector<double> winDeriv(len);
  kaiser::buildTimeDerivativeWindow(winDeriv, shape);

  const double winsum = std::accumulate(win.begin(), win.end(), 0.);
  const double magScale = 2. / winsum;
  const double fancyScale = len / (winsum * kPi);
  const double center = 0.5 * (len - 1);

  ReassignmentWindows rw;
  rw.length = len;
  rw.w.resize(len);
  rw.wFreqRamp.resize(len);
  rw.wTimeRamp.resize(len);
  for (long k = 0; k < len; ++k)
  {
    rw.w[k] = static_cast<float>(magScale * win[k]);
    rw.wFreqRamp[k] = static_cast<float>(fancyScale * winDeriv[k]);
    rw.wTimeRamp[k] = static_cast<float>(magScale * win[k] * (k - center));
  }
  return rw;
}

}  // namespace ml::utu
