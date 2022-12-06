
// vutu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "mldsp.h"

#include "madronalib.h"

// stats about partials

namespace ml
{

static constexpr size_t kPartialsPerVoice{ 64 };

struct PartialsStats
{
  // get range of values for each parameter (except phase)
  Interval timeRange; // min and max time in all partials
  Interval ampRange;
  Interval bandwidthRange;
  Interval freqRange;
  size_t nPartials;
  size_t maxFrames;   // max number of frames in any partial
  std::vector< Interval > partialTimeRanges; // time range for each partial
};

struct SumuPartial
{
  std::vector< float > time;
  std::vector< float > amp;
  std::vector< float > freq;
  std::vector< float > bandwidth;
  std::vector< float > phase;
};

struct SumuPartialsData
{
  PartialsStats stats;
  std::vector< SumuPartial > partials;
};

struct PartialFrame
{
  float amp{0};
  float freq{0};
  float bandwidth{0};
  float phase{0};
};

Interval getVectorRange(const std::vector< float >& vec);
Interval getParamRangeInPartials(const SumuPartialsData& partialData, Symbol param);
PartialsStats getPartialsStats(const SumuPartialsData& partialData);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrame(const SumuPartialsData& partialData, size_t partialIndex, float time);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrameDownsampled(const SumuPartialsData& partialData, size_t partialIndex, float time, float dTime);

// return a frame of a single Partial at the nearest sample to the specified time.
PartialFrame getPartialFrameNearest(const SumuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial by its frame index.
PartialFrame getPartialFrameByIndex(const SumuPartialsData& partialData, size_t partialIndex, size_t frameIndex);

inline std::ostream& operator<< (std::ostream& out, const PartialFrame& r)
{
  out << "[";
  out << r.amp;
  out << ", ";
  out << r.freq;
  out << ", ";
  out << r.bandwidth;
  out << ", ";
  out << r.phase;
  out << "]";
  return out;
}


}
