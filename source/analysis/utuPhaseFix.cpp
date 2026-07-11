
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuPhaseFix.h"

#include <cmath>

namespace ml::utu
{

namespace
{
constexpr double kPi = 3.14159265358979324;

double phaseTravel(double f0, double f1, double dt) { return 2. * kPi * 0.5 * (f0 + f1) * dt; }
}  // namespace

double wrapPi(double x)
{
  const double twoPi = 2. * kPi;
  return x + (twoPi * std::floor(.5 + (-x / twoPi)));
}

// Loris matchPhaseFwd on breakpoints i-1 (fixed) and i (adjusted), with
// damping 0.5. Analysis breakpoints are never null (zero-amplitude), but the
// null branches are kept for parity with Loris.
void fixFrequency(BuildingPartial& p, double sampleRate, double maxFixPct)
{
  const double damping = 0.5;
  for (size_t i = 1; i < p.size(); ++i)
  {
    if (!(p.amp[i] != 0.f))
    {
      continue;  // nulls are phase reset points; leave them alone
    }
    const double dt = p.dt(i - 1, i, sampleRate);
    double travel = phaseTravel(p.freq[i - 1], p.freq[i], dt);

    if (!(p.amp[i - 1] != 0.f))
    {
      // a null predecessor resets phase during rendering; give it the
      // phase that achieves this breakpoint's phase
      p.phase[i - 1] = float(wrapPi(p.phase[i] - travel));
      continue;
    }

    const double err = wrapPi(p.phase[i] - (p.phase[i - 1] + travel));
    travel += damping * err;

    const double f0 = p.freq[i - 1];
    double ftgt = (travel / (kPi * dt)) - f0;

    // clamp the frequency adjustment
    const double fmax = p.freq[i] * (1. + (maxFixPct * .01));
    const double fmin = p.freq[i] * (1. - (maxFixPct * .01));
    if (ftgt > fmax)
    {
      ftgt = fmax;
    }
    else if (ftgt < fmin)
    {
      ftgt = fmin;
    }
    p.freq[i] = float(ftgt);

    // recompute the phase for the adjusted frequency
    p.phase[i] = float(wrapPi(p.phase[i - 1] + phaseTravel(f0, ftgt, dt)));
  }
}

void fixFrequency(std::vector<BuildingPartial>& partials, double sampleRate, double maxFixPct)
{
  for (auto& p : partials)
  {
    if (p.size() > 1)
    {
      fixFrequency(p, sampleRate, maxFixPct);
    }
  }
}

}  // namespace ml::utu
