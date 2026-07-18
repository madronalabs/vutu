
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Frequency/phase reconciliation for phase-correct additive modeling
// (Fitz & Fulop Sec. 8), written from the behavioral spec in the header;
// no Loris source consulted. The consistency invariant — stored phase
// equals the integrated travel of stored frequencies, with per-step
// frequency changes clamped — is pinned by the phasefix selftest.

#include "utuPhaseFix.h"

#include <algorithm>
#include <cmath>

namespace ml::utu
{

namespace
{
constexpr double kPi = 3.14159265358979324;
constexpr double kTwoPi = 2. * kPi;
}  // namespace

double wrapPi(double x) { return x - kTwoPi * floor(x / kTwoPi + 0.5); }

// Walk the partial forward. A rendering oscillator integrating a linear
// frequency segment travels π·(f0+f1)·dt radians; where the stored arrival
// phase disagrees, nudge the arrival frequency to absorb half the wrapped
// error (full correction would chase measurement noise breakpoint by
// breakpoint), clamp the nudge to ±maxFixPct percent, then recompute the
// stored phase from the actual travel so that phase and frequency agree
// exactly from here on.
void fixFrequency(BuildingPartial& partial, double sampleRate, double maxFixPct)
{
  const size_t n = partial.size();
  if (n < 2) return;

  const double maxFix = 0.01 * maxFixPct;
  for (size_t i = 1; i < n; ++i)
  {
    const double dt = partial.dt(i - 1, i, sampleRate);
    if (dt <= 0.) continue;
    const double f0 = partial.freq[i - 1];
    const double f1 = partial.freq[i];
    const double travel = kPi * (f0 + f1) * dt;
    const double err = wrapPi(double(partial.phase[i]) - partial.phase[i - 1] - travel);

    double ftgt = (travel + 0.5 * err) / (kPi * dt) - f0;
    const double lo = f1 * (1. - maxFix), hi = f1 * (1. + maxFix);
    ftgt = std::min(hi, std::max(lo, ftgt));

    partial.freq[i] = float(ftgt);
    partial.phase[i] =
        float(wrapPi(partial.phase[i - 1] + kPi * (f0 + ftgt) * dt));
  }
}

void fixFrequency(std::vector<BuildingPartial>& partials, double sampleRate, double maxFixPct)
{
  for (auto& p : partials)
  {
    fixFrequency(p, sampleRate, maxFixPct);
  }
}

}  // namespace ml::utu
