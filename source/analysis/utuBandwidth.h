
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

#include "utuPeaks.h"

namespace ml::utu
{

// Port of Loris AssociateBandwidth: residue spectral energy — energy in
// rejected peaks — is collected into overlapping frequency regions and
// redistributed to the kept peaks as noise (bandwidth) energy, weighted by
// peak amplitude. Regions are regionWidth Hz wide with centers spaced at
// half that; a frame's few dozen regions and few hundred peaks make this
// scalar work.
class AssociateBandwidth
{
 public:
  void configure(float regionWidthHz, float sampleRate);

  // associate residue energy in frame.peaks[numKept..) with the kept peaks,
  // setting their bw and adjusting their amp; resets region state after
  void associateBandwidth(PeakFrame& frame);

 private:
  double computeNoiseEnergy(double freq, double amp) const;

  std::vector<double> _weights;  // per-region amplitude weights
  std::vector<double> _surplus;  // per-region residue energy
  double _regionRate{0.};        // regions per Hz
};

// Loris Breakpoint::addNoiseEnergy on a Peak with bandwidth already set
void addNoiseEnergy(Peak& pk, double enoise);

}  // namespace ml::utu
