
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

#include "utuSpectrum.h"

namespace ml::utu
{

// One spectral peak candidate. Peaks are kept as an array of structs: a
// frame holds only a few hundred peaks and they are sorted and partitioned,
// so the SoA layout used for the spectral bins would only get in the way.
struct Peak
{
  float freq{0};        // Hz, reassigned
  float amp{0};         // absolute magnitude
  float bw{0};          // filled in later by bandwidth association
  float phase{0};       // radians, reassigned
  float timeOffset{0};  // seconds relative to the frame center (reassigned)
};

struct PeakFrame
{
  std::vector<Peak> peaks;
  size_t numKept{0};  // after thinning: peaks[0..numKept) kept, rest retained
                      // as noise for bandwidth association
};

// Port of Loris SpectralPeakSelector::selectReassignmentMinima: emit a peak
// wherever the frequency reassignment crosses from positive to negative
// correction, rejecting peaks below minFreq or with large time corrections.
class PeakSelector
{
 public:
  void configure(float sampleRate, float cropTimeSec);
  void selectPeaks(const ReassignedSpectrum& spectrum, float minFreqHz, PeakFrame& frame) const;

 private:
  float _sampleRate{48000.f};
  float _maxTimeOffsetSec{0.f};
};

// Port of Loris Analyzer::thinPeaks: consider peaks loudest first, drop
// peaks whose reassigned absolute time is negative, reject peaks within
// ±freqResolution of an already-kept louder peak or below the amplitude
// floor, and fade kept amplitudes over the 10 dB above the floor. Rejected
// peaks are kept after the partition point for bandwidth association.
void thinPeaks(PeakFrame& frame, float freqResolutionHz, float ampFloorDb, double frameTimeSec);

}  // namespace ml::utu
