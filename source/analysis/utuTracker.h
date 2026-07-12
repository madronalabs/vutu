
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <cstdint>
#include <vector>

#include "utuPeaks.h"

namespace ml::utu
{

// One partial under construction, struct-of-arrays over breakpoints.
// Breakpoint times are kept as an exact integer frame sample plus the small
// reassignment offset in seconds: phase correction needs breakpoint time
// differences to much better precision than float seconds carries at
// tens-of-seconds absolute time.
struct BuildingPartial
{
  std::vector<int64_t> frameSample;
  std::vector<float> timeOffset;  // seconds, |offset| <= cropTime
  std::vector<float> freq;
  std::vector<float> amp;
  std::vector<float> bw;
  std::vector<float> phase;

  size_t size() const { return freq.size(); }
  float endFreq() const { return freq.back(); }
  double timeAt(size_t i, double sampleRate) const
  {
    return frameSample[i] / sampleRate + timeOffset[i];
  }
  // exact-in-samples time difference between breakpoints j > i
  double dt(size_t i, size_t j, double sampleRate) const
  {
    return (frameSample[j] - frameSample[i]) / sampleRate +
           (double(timeOffset[j]) - timeOffset[i]);
  }
};

// Port of Loris PartialBuilder with a flat frequency reference: kept peaks,
// taken in ascending frequency, either extend the nearest eligible partial
// within freqDrift or start new partials; a frame's touched partials become
// the next frame's eligible set. Streaming-friendly: consumes one frame at
// a time; partials that leave the eligible set are final.
class PartialTracker
{
 public:
  void configure(float freqDriftHz, double sampleRate);
  void reset();

  // consume frame.peaks[0..numKept), reordering them by frequency.
  // freqDriftHz <= 0 uses the configured drift; callers may vary the drift
  // per frame (e.g. wider at transients)
  void buildFrame(PeakFrame& frame, int64_t frameSample, float freqDriftHz = 0.f);

  std::vector<BuildingPartial>& partials() { return _partials; }

 private:
  void appendBreakpoint(BuildingPartial& p, int64_t frameSample, const Peak& pk);

  float _freqDrift{0.f};
  double _sampleRate{48000.};
  std::vector<BuildingPartial> _partials;
  // indices into _partials; ascending end frequency
  std::vector<uint32_t> _eligible, _newlyEligible;
};

}  // namespace ml::utu
