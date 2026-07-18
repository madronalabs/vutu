
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Implemented from Fitz & Fulop, "A Unified Theory of Time-Frequency
// Reassignment" (Fig. 3 consensus regions, Sec. 7 pruning); no Loris
// source consulted. Selection behavior is pinned by the utucompare
// selftests (sine, two-sine, crop-burst, silence).

#include "utuPeaks.h"

#include <algorithm>
#include <cmath>

namespace ml::utu
{

void PeakSelector::configure(float sampleRate, float cropTimeSec)
{
  _sampleRate = sampleRate;
  _maxTimeOffsetSec = cropTimeSec;
}

// Near a dominant component the map from bin frequency to reassigned
// frequency flattens onto the component (Fig. 3), so the frequency
// correction runs positive below it and negative above it: each component
// is a positive-to-negative sign crossing of freqCorr between adjacent
// bins. Emit one candidate per crossing, at the bin needing the smaller
// correction; that bin's data is the closest sample of the consensus.
void PeakSelector::selectPeaks(const ReassignedSpectrum& spectrum, float minFreqHz,
                               PeakFrame& frame) const
{
  frame.peaks.clear();
  frame.numKept = 0;

  const auto& f = spectrum.frame();
  const long bins = f.bins;
  const double binHz = double(_sampleRate) / f.fftSize;
  for (long j = 1; j + 1 < bins; ++j)
  {
    if (!((f.freqCorr[j] > 0.f) && (f.freqCorr[j + 1] <= 0.f))) continue;
    const long idx = (f.freqCorr[j] < -f.freqCorr[j + 1]) ? j : j + 1;

    const float freq = float((idx + f.freqCorr[idx]) * binHz);
    if (freq < minFreqHz) continue;

    // Sec. 7: a large time reassignment means the energy is poorly
    // represented at this window position and is better captured by a
    // neighboring frame; keeping it would smear events across frames
    const float timeOffsetSec = f.timeCorr[idx] / _sampleRate;
    if (fabsf(timeOffsetSec) >= _maxTimeOffsetSec) continue;

    Peak pk;
    pk.freq = freq;
    pk.amp = spectrum.magnitudeAt(idx);
    pk.phase = spectrum.phaseAt(idx);
    pk.timeOffset = timeOffsetSec;
    frame.peaks.push_back(pk);
  }
}

// Loudest-first amplitude thinning: every kept peak claims ±freqResolution
// of spectrum; quieter peaks inside a claimed band, and peaks below the
// amplitude floor, are rejected but retained after the partition point as
// residue for bandwidth association. Kept amplitudes fade linearly over
// the 10 dB above the floor so partials don't pop in and out at the
// threshold. Peaks reassigned to negative absolute time are meaningless
// and dropped entirely.
void thinPeaks(PeakFrame& frame, float freqResolutionHz, float ampFloorDb, double frameTimeSec)
{
  const float threshold = powf(10.f, 0.05f * ampFloorDb);
  const float beginFade = powf(10.f, 0.05f * (ampFloorDb + 10.f));

  auto& peaks = frame.peaks;
  peaks.erase(std::remove_if(peaks.begin(), peaks.end(),
                             [frameTimeSec](const Peak& p)
                             { return frameTimeSec + p.timeOffset < 0.; }),
              peaks.end());
  std::sort(peaks.begin(), peaks.end(),
            [](const Peak& a, const Peak& b) { return a.amp > b.amp; });

  std::vector<Peak> kept, residue;
  kept.reserve(peaks.size());
  residue.reserve(peaks.size());
  for (const Peak& p : peaks)
  {
    bool masked = false;
    for (const Peak& k : kept)
    {
      if (fabsf(k.freq - p.freq) < freqResolutionHz)
      {
        masked = true;
        break;
      }
    }
    if (masked || (p.amp < threshold))
    {
      residue.push_back(p);
      continue;
    }
    Peak q = p;
    if (q.amp < beginFade)
    {
      const float alpha = (beginFade - q.amp) / (beginFade - threshold);
      q.amp *= 1.f - alpha;
    }
    kept.push_back(q);
  }

  frame.numKept = kept.size();
  std::copy(residue.begin(), residue.end(),
            std::copy(kept.begin(), kept.end(), peaks.begin()));
  peaks.resize(kept.size() + residue.size());
}

}  // namespace ml::utu
