
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

#include "utuPeaks.h"

namespace ml::utu
{

// Residue bandwidth association, the noise half of the reassigned
// bandwidth-enhanced additive model (Fitz & Haken; Fitz & Fulop Sec. 8):
// residue spectral energy — energy in rejected peaks — is collected into
// overlapping frequency regions and redistributed to the kept peaks as noise
// (bandwidth) energy, weighted by peak amplitude, so the synthesizer can
// reproduce it as noise modulation instead of dropping it. Regions are
// regionWidth Hz wide with centers spaced at half that; a frame's few dozen
// regions and few hundred peaks make this scalar work.
class AssociateBandwidth
{
 public:
  // maxResidueOffsetSec > 0 gates residue by its reassigned time: a
  // rejected peak only contributes surplus when its time correction places
  // it within ±maxResidueOffsetSec of this frame. The window overlaps many
  // frames, so without the gate a short broadband event (a mallet attack)
  // deposits noise into every frame its window touches — a window length of
  // smeared, over-counted rustle. With the gate at half a hop, each residue
  // quantum is counted once, in the frame nearest its true time: the same
  // reassignment principle the sinusoids get. 0 disables the gate.
  void configure(float regionWidthHz, float sampleRate, float maxResidueOffsetSec = 0.f);

  // associate residue energy in frame.peaks[numKept..) with the kept peaks,
  // setting their bw and adjusting their amp; resets region state after
  void associateBandwidth(PeakFrame& frame);

 private:
  double computeNoiseEnergy(double freq, double amp) const;

  std::vector<double> _weights;  // per-region amplitude weights
  std::vector<double> _surplus;  // per-region residue energy
  double _regionRate{0.};        // regions per Hz
  float _maxResidueOffset{0.f};  // seconds; 0 = no gating
};

// energy-preserving noise deposit on a Peak with bandwidth already set:
// amp² keeps total energy, bw keeps its noise fraction
void addNoiseEnergy(Peak& pk, double enoise);

}  // namespace ml::utu
