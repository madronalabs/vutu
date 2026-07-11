
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuTracker.h"

#include <algorithm>
#include <cmath>

namespace ml::utu
{

void PartialTracker::configure(float freqDriftHz, double sampleRate)
{
  _freqDrift = freqDriftHz;
  _sampleRate = sampleRate;
  reset();
}

void PartialTracker::reset()
{
  _partials.clear();
  _eligible.clear();
  _newlyEligible.clear();
}

// insert keeping breakpoints time-ordered; reassignment offsets can reach
// ±one hop, so a breakpoint can land earlier in absolute time than its
// predecessor from the previous frame
void PartialTracker::appendBreakpoint(BuildingPartial& p, int64_t frameSample, const Peak& pk)
{
  size_t pos = p.size();
  const double t = frameSample / _sampleRate + pk.timeOffset;
  while ((pos > 0) && (p.timeAt(pos - 1, _sampleRate) > t))
  {
    --pos;
  }
  p.frameSample.insert(p.frameSample.begin() + pos, frameSample);
  p.timeOffset.insert(p.timeOffset.begin() + pos, pk.timeOffset);
  p.freq.insert(p.freq.begin() + pos, pk.freq);
  p.amp.insert(p.amp.begin() + pos, pk.amp);
  p.bw.insert(p.bw.begin() + pos, pk.bw);
  p.phase.insert(p.phase.begin() + pos, pk.phase);
}

void PartialTracker::buildFrame(PeakFrame& frame, int64_t frameSample)
{
  _newlyEligible.clear();
  auto& peaks = frame.peaks;

  // eligible partials are ascending in end frequency iff peaks are always
  // consumed in ascending frequency
  std::sort(peaks.begin(), peaks.begin() + frame.numKept,
            [](const Peak& a, const Peak& b) { return a.freq < b.freq; });

  auto endFreq = [this](size_t eligIdx) { return _partials[_eligible[eligIdx]].endFreq(); };
  auto dist = [this](size_t eligIdx, const Peak& pk)
  { return fabsf(_partials[_eligible[eligIdx]].endFreq() - pk.freq); };

  const size_t nEligible = _eligible.size();
  size_t eligible = 0;
  for (size_t i = 0; i < frame.numKept; ++i)
  {
    const Peak& pk = peaks[i];

    // find the eligible partial nearest in frequency to this peak;
    // nextEligible is the first with end frequency above the peak
    size_t nextEligible = eligible;
    if ((eligible < nEligible) && (endFreq(eligible) < pk.freq))
    {
      ++nextEligible;
      while ((nextEligible < nEligible) && (endFreq(nextEligible) < pk.freq))
      {
        ++nextEligible;
        ++eligible;
      }
      if ((nextEligible < nEligible) && (dist(nextEligible, pk) < dist(eligible, pk)))
      {
        eligible = nextEligible;
      }
    }

    // match only if within freqDrift and the next peak would not be a
    // better match for the same partial
    bool makeMatch = false;
    if (eligible < nEligible)
    {
      if (_freqDrift > fabsf(endFreq(eligible) - pk.freq))
      {
        const bool nextIsBetter =
            (i + 1 < frame.numKept) && (dist(eligible, peaks[i + 1]) < dist(eligible, pk));
        if (!nextIsBetter)
        {
          makeMatch = true;
        }
      }
    }

    uint32_t target;
    if (makeMatch)
    {
      target = _eligible[eligible];
      appendBreakpoint(_partials[target], frameSample, pk);
    }
    else
    {
      target = uint32_t(_partials.size());
      _partials.emplace_back();
      appendBreakpoint(_partials.back(), frameSample, pk);
    }
    _newlyEligible.push_back(target);

    eligible = nextEligible;
  }

  std::swap(_eligible, _newlyEligible);
}

}  // namespace ml::utu
