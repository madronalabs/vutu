
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

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

void PeakSelector::selectPeaks(const ReassignedSpectrum& spectrum, float minFreqHz,
                               PeakFrame& frame) const
{
  frame.peaks.clear();
  frame.numKept = 0;

  const long n = spectrum.fftSize();
  const float* freqCorr = spectrum.frame().freqCorr.data();
  const float* timeCorr = spectrum.frame().timeCorr.data();
  const float sampsToHz = _sampleRate / n;
  const float minFreqSample = minFreqHz / sampsToHz;
  const float maxCorrSamples = _maxTimeOffsetSec * _sampleRate;
  const float oneOverSR = 1.f / _sampleRate;

  const long endJ = (n / 2) - 2;
  long startJ = 1;

  // skip bins below the frequency floor; as in Loris, fsample enters the
  // scan one bin stale
  float fsample;
  do
  {
    fsample = startJ + freqCorr[startJ];
    ++startJ;
  } while ((fsample < minFreqSample) && (startJ < endJ));

  for (long j = startJ; j < endJ; ++j)
  {
    // a change from positive to negative frequency correction indicates a
    // concentration of energy in the spectrum
    const float nextFsample = (j + 1) + freqCorr[j + 1];
    if ((fsample > j) && (nextFsample < j + 1))
    {
      // take the candidate with the smaller correction
      float freqSample;
      long peakIdx;
      if ((fsample - j) < ((j + 1) - nextFsample))
      {
        freqSample = fsample;
        peakIdx = j;
      }
      else
      {
        freqSample = nextFsample;
        peakIdx = j + 1;
      }

      const float freq = freqSample * sampsToHz;
      if (freq >= minFreqHz)
      {
        const float tc = timeCorr[peakIdx];
        if (fabsf(tc) < maxCorrSamples)
        {
          Peak pk;
          pk.freq = freq;
          pk.amp = spectrum.magnitudeAt(peakIdx);
          pk.bw = 0.f;
          pk.phase = spectrum.phaseAt(peakIdx);
          pk.timeOffset = tc * oneOverSR;
          frame.peaks.push_back(pk);
        }
      }
    }
    fsample = nextFsample;
  }
}

void thinPeaks(PeakFrame& frame, float freqResolutionHz, float ampFloorDb, double frameTimeSec)
{
  auto& peaks = frame.peaks;

  // absolute magnitude thresholds; fade quiet kept peaks out over 10 dB
  const float threshold = static_cast<float>(std::pow(10., 0.05 * ampFloorDb));
  const float beginFade = static_cast<float>(std::pow(10., 0.05 * (ampFloorDb + 10.)));

  // louder peaks are preferred
  std::sort(peaks.begin(), peaks.end(),
            [](const Peak& a, const Peak& b) { return a.amp > b.amp; });

  // negative absolute times are not real; discard them entirely
  peaks.erase(std::remove_if(peaks.begin(), peaks.end(),
                             [frameTimeSec](const Peak& p)
                             { return (p.timeOffset + frameTimeSec) < 0.; }),
              peaks.end());

  size_t numKept = 0;
  for (size_t i = 0; i < peaks.size(); ++i)
  {
    Peak& pk = peaks[i];
    const float lower = pk.freq - freqResolutionHz;
    const float upper = pk.freq + freqResolutionHz;
    bool masked = false;
    for (size_t m = 0; m < numKept; ++m)
    {
      if ((peaks[m].freq > lower) && (peaks[m].freq < upper))
      {
        masked = true;
        break;
      }
    }

    if ((pk.amp > threshold) && !masked)
    {
      if (pk.amp < beginFade)
      {
        const float alpha = (beginFade - pk.amp) / (beginFade - threshold);
        pk.amp *= (1.f - alpha);
      }
      if (i != numKept)
      {
        std::swap(peaks[i], peaks[numKept]);
      }
      ++numKept;
    }
  }
  frame.numKept = numKept;
}

}  // namespace ml::utu
