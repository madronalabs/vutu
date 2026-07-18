
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Residue bandwidth association — the noise half of the bandwidth-enhanced
// additive model (Fitz & Haken; Fitz & Fulop Sec. 8) — written from the
// behavioral spec in the header; no Loris source consulted. The gate and
// energy-update identities are pinned by the residue-gate selftest:
// association with the time gate equals ungated association over only the
// in-gate residue, region deposits are additive, and bw·amp'² recovers the
// collected noise energy exactly.

#include "utuBandwidth.h"

#include <cmath>

namespace ml::utu
{

namespace
{

// the two lowest regions sit against DC, where windowed near-silence piles
// up junk energy no partial should inherit as noise
constexpr long kLowestRegion = 2;

// linear split of a value between the two regions straddling pos
void distribute(std::vector<double>& dst, double pos, double value)
{
  const long lo = long(pos);
  const double a = pos - lo;
  if ((lo >= 0) && (lo < long(dst.size()))) dst[lo] += (1. - a) * value;
  if ((lo + 1 >= 0) && (lo + 1 < long(dst.size()))) dst[lo + 1] += a * value;
}

}  // namespace

void AssociateBandwidth::configure(float regionWidthHz, float sampleRate,
                                   float maxResidueOffsetSec)
{
  _regionRate = 2. / regionWidthHz;  // centers spaced at half a region width
  const long numRegions = long(0.5 * sampleRate * _regionRate) + 2;  // up to Nyquist
  _weights.assign(numRegions, 0.);
  _surplus.assign(numRegions, 0.);
  _maxResidueOffset = maxResidueOffsetSec;
}

// A kept peak's share of a region's residue is its amplitude's fraction of
// the total kept-amplitude weight deposited there; summing over the two
// straddled regions hands every region's surplus out completely wherever
// any kept peak reaches it.
double AssociateBandwidth::computeNoiseEnergy(double freq, double amp) const
{
  const double pos = freq * _regionRate;
  const long lo = long(pos);
  const double a = pos - lo;
  const double part[2] = {(1. - a) * amp, a * amp};
  double e = 0.;
  for (int i = 0; i < 2; ++i)
  {
    const long r = lo + i;
    if ((r < kLowestRegion) || (r >= long(_weights.size()))) continue;
    if ((part[i] <= 0.) || (_weights[r] <= 0.)) continue;
    e += _surplus[r] * (part[i] / _weights[r]);
  }
  return e;
}

void AssociateBandwidth::associateBandwidth(PeakFrame& frame)
{
  auto& peaks = frame.peaks;
  const size_t numKept = frame.numKept;

  for (size_t i = 0; i < numKept; ++i)
  {
    distribute(_weights, peaks[i].freq * _regionRate, peaks[i].amp);
  }
  for (size_t i = numKept; i < peaks.size(); ++i)
  {
    // the residue gate: a rejected peak's energy counts only in the frame
    // nearest its reassigned time (see the header for why)
    if ((_maxResidueOffset > 0.f) && (fabsf(peaks[i].timeOffset) > _maxResidueOffset))
    {
      continue;
    }
    distribute(_surplus, peaks[i].freq * _regionRate,
               double(peaks[i].amp) * peaks[i].amp);
  }

  // compute every share against the original amplitudes before any update
  std::vector<double> enoise(numKept);
  for (size_t i = 0; i < numKept; ++i)
  {
    enoise[i] = computeNoiseEnergy(peaks[i].freq, peaks[i].amp);
  }
  for (size_t i = 0; i < numKept; ++i)
  {
    addNoiseEnergy(peaks[i], enoise[i]);
  }

  std::fill(_weights.begin(), _weights.end(), 0.);
  std::fill(_surplus.begin(), _surplus.end(), 0.);
}

// energy-preserving update: sine energy e = amp², noise energy n = e·bw;
// adding enoise leaves total energy in amp² and its noise fraction in bw
void addNoiseEnergy(Peak& pk, double enoise)
{
  const double e = double(pk.amp) * pk.amp;
  const double total = e + enoise;
  if (total <= 0.) return;
  const double n = e * pk.bw;
  pk.bw = float((n + enoise) / total);
  pk.amp = float(sqrt(total));
}

}  // namespace ml::utu
