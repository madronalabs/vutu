
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Partial tracking: greedy nearest-frequency matching of kept peaks onto
// growing partials, written from the behavioral spec in the header (see
// Fitz & Fulop Sec. 8: partials follow ridges on the reassigned surface);
// no Loris source consulted. Pinned by the sine/chirp/two-sine selftests.

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

// Reassignment can place a breakpoint slightly before its predecessor
// (time offsets reach ±one hop), so insertion keeps breakpoints
// time-ordered; the common case is a plain append.
void PartialTracker::appendBreakpoint(BuildingPartial& p, int64_t frameSample, const Peak& pk)
{
  const double t = frameSample / _sampleRate + pk.timeOffset;
  size_t pos = p.size();
  while ((pos > 0) && (p.timeAt(pos - 1, _sampleRate) > t)) --pos;

  p.frameSample.insert(p.frameSample.begin() + pos, frameSample);
  p.timeOffset.insert(p.timeOffset.begin() + pos, pk.timeOffset);
  p.freq.insert(p.freq.begin() + pos, pk.freq);
  p.amp.insert(p.amp.begin() + pos, pk.amp);
  p.bw.insert(p.bw.begin() + pos, pk.bw);
  p.phase.insert(p.phase.begin() + pos, pk.phase);
}

void PartialTracker::buildFrame(PeakFrame& frame, int64_t frameSample, float freqDriftHz)
{
  const float drift = (freqDriftHz > 0.f) ? freqDriftHz : _freqDrift;

  std::sort(frame.peaks.begin(), frame.peaks.begin() + frame.numKept,
            [](const Peak& a, const Peak& b) { return a.freq < b.freq; });

  // Two-pointer merge of ascending peak frequencies against ascending
  // partial end frequencies. Each eligible partial takes at most one peak;
  // a peak extends the nearest eligible partial within the drift limit
  // unless the following (higher) peak is an even better match for it, in
  // which case this peak starts a new partial and leaves the eligible one
  // for its neighbor.
  _newlyEligible.clear();
  size_t e = 0;
  auto endFreq = [this](size_t ei) { return _partials[_eligible[ei]].endFreq(); };
  for (size_t i = 0; i < frame.numKept; ++i)
  {
    const Peak& pk = frame.peaks[i];

    // partials left behind here are below every remaining peak and only
    // recede further: they exit eligibility for good
    while ((e + 1 < _eligible.size()) &&
           (fabsf(endFreq(e + 1) - pk.freq) < fabsf(endFreq(e) - pk.freq)))
    {
      ++e;
    }

    bool matched = false;
    if (e < _eligible.size())
    {
      const float dist = fabsf(endFreq(e) - pk.freq);
      const bool nextPeakCloser =
          (i + 1 < frame.numKept) &&
          (fabsf(frame.peaks[i + 1].freq - endFreq(e)) < dist);
      if ((dist < drift) && !nextPeakCloser)
      {
        const uint32_t idx = _eligible[e];
        appendBreakpoint(_partials[idx], frameSample, pk);
        _newlyEligible.push_back(idx);
        ++e;
        matched = true;
      }
    }
    if (!matched)
    {
      BuildingPartial p;
      _partials.push_back(std::move(p));
      appendBreakpoint(_partials.back(), frameSample, pk);
      _newlyEligible.push_back(uint32_t(_partials.size() - 1));
    }
  }
  std::swap(_eligible, _newlyEligible);
}

}  // namespace ml::utu
