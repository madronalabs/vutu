
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuBandwidth.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace ml::utu
{

// The region accumulators are double: a frame's residue sums span many
// orders of magnitude and the extra precision is free at this count.

namespace
{

// index of the last region with center at or below the fractional region
// frequency, or -1
int findRegionBelow(double binFreq, size_t howManyBins)
{
  if (binFreq < 0.)
  {
    return -1;
  }
  return int(std::min(std::floor(binFreq), howManyBins - 1.));
}

// relative contribution of a component at fractional region frequency
// binFreq to the regions above and below it; everything above the highest
// region center is lumped into that region
double computeAlpha(double binFreq, size_t howManyBins)
{
  if (binFreq > howManyBins)
  {
    return 0.;
  }
  return binFreq - std::floor(binFreq);
}

void distribute(double fractionalBin, double x, std::vector<double>& regions)
{
  const int posBelow = findRegionBelow(fractionalBin, regions.size());
  const int posAbove = posBelow + 1;
  const double alpha = computeAlpha(fractionalBin, regions.size());
  if (posAbove < int(regions.size()))
  {
    regions[posAbove] += alpha * x;
  }
  if (posBelow >= 0)
  {
    regions[posBelow] += (1. - alpha) * x;
  }
}

}  // namespace

void AssociateBandwidth::configure(float regionWidthHz, float sampleRate)
{
  assert(regionWidthHz > 0.f && sampleRate > 0.f);
  const size_t numRegions = size_t(sampleRate / regionWidthHz);
  _weights.assign(numRegions, 0.);
  _surplus.assign(numRegions, 0.);
  _regionRate = 2. / regionWidthHz;
}

double AssociateBandwidth::computeNoiseEnergy(double freq, double amp) const
{
  if (freq < 0.)
  {
    return 0.;
  }
  const double bin = freq * _regionRate;
  const int posBelow = findRegionBelow(bin, _surplus.size());
  const int posAbove = posBelow + 1;
  const double alpha = computeAlpha(bin, _surplus.size());

  // the two lowest regions are ignored; surplus share is weighted by the
  // peak's amplitude
  const int lowestRegion = 2;
  double noise = 0.;
  if ((posAbove < int(_surplus.size())) && (alpha != 0.) && (posAbove >= lowestRegion))
  {
    noise += _surplus[posAbove] * alpha * amp / _weights[posAbove];
  }
  if (posBelow >= lowestRegion)
  {
    noise += _surplus[posBelow] * (1. - alpha) * amp / _weights[posBelow];
  }
  return noise;
}

void addNoiseEnergy(Peak& pk, double enoise)
{
  double e = double(pk.amp) * pk.amp;  // current total energy
  double n = e * pk.bw;                // current noise energy
  if (e < n)
  {
    e = n;
  }
  if (n + enoise > 0.)
  {
    pk.bw = float((n + enoise) / (e + enoise));
    pk.amp = float(std::sqrt(e + enoise));
  }
  else
  {
    pk.bw = 0.f;
    pk.amp = float(std::sqrt(e - n));
  }
}

void AssociateBandwidth::associateBandwidth(PeakFrame& frame)
{
  if (frame.numKept == 0)
  {
    return;
  }
  auto& peaks = frame.peaks;

  // kept peaks accumulate as amplitude weights, rejected ones as residue
  // energy
  for (size_t i = 0; i < frame.numKept; ++i)
  {
    if (peaks[i].freq > 0.f)
    {
      distribute(peaks[i].freq * _regionRate, peaks[i].amp, _weights);
    }
  }
  for (size_t i = frame.numKept; i < peaks.size(); ++i)
  {
    if (peaks[i].freq > 0.f)
    {
      distribute(peaks[i].freq * _regionRate, double(peaks[i].amp) * peaks[i].amp, _surplus);
    }
  }
  for (size_t i = 0; i < frame.numKept; ++i)
  {
    Peak& pk = peaks[i];
    pk.bw = 0.f;
    addNoiseEnergy(pk, computeNoiseEnergy(pk.freq, pk.amp));
  }

  std::fill(_weights.begin(), _weights.end(), 0.);
  std::fill(_surplus.begin(), _surplus.end(), 0.);
}

}  // namespace ml::utu
