
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuAnalyzer.h"

#include <algorithm>
#include <cassert>

#include "utuPhaseFix.h"

namespace ml::utu
{

void PartialAnalyzer::configure(const AnalyzerParams& p)
{
  _params = p;
  // resolve Loris-default couplings (Analyzer::configure)
  if (_params.windowWidth <= 0.f) _params.windowWidth = 2.f * _params.resolution;
  if (_params.freqFloor <= 0.f) _params.freqFloor = _params.resolution;
  if (_params.freqDrift <= 0.f) _params.freqDrift = 0.5f * _params.resolution;
  if (_params.sidelobeLevel <= 0.f) _params.sidelobeLevel = -_params.ampFloor;
  if (_params.hopTime <= 0.f) _params.hopTime = 1.f / _params.windowWidth;
  if (_params.cropTime <= 0.f) _params.cropTime = _params.hopTime;

  _spectrum.configure(
      buildReassignmentWindows(_params.sampleRate, _params.windowWidth, _params.sidelobeLevel));
  _selector.configure(_params.sampleRate, _params.cropTime);
  if (_params.bwRegionWidth > 0.f)
  {
    // residue gated to half a hop: counted once, in the nearest frame
    _bwAssociator.configure(_params.bwRegionWidth, _params.sampleRate, 0.5f * _params.hopTime);
  }
  _tracker.configure(_params.freqDrift, _params.sampleRate);

  _hopSamples = long(_params.hopTime * _params.sampleRate);  // truncated, as in Loris
  const long winlen = _spectrum.windowLength();
  _windowScratch.assign(winlen, 0.f);
  _buffer.resize(int(4 * winlen));
  _configured = true;
  reset();
}

void PartialAnalyzer::reset()
{
  assert(_configured);
  _tracker.reset();
  _buffer.clear();
  _frameSample = 0;
  _samplesPushed = 0;

  // pre-write half a window of silence so the first frame is centered on
  // the first input sample; zero samples here are equivalent to Loris's
  // clipped window at the buffer edges
  const long half = _spectrum.windowLength() / 2;
  std::fill(_windowScratch.begin(), _windowScratch.end(), 0.f);
  _buffer.write(_windowScratch.data(), half);
}

void PartialAnalyzer::processHop()
{
  const long winlen = _spectrum.windowLength();
  _buffer.readWithOverlap(_windowScratch.data(), winlen, winlen - _hopSamples);

  _spectrum.transform(_windowScratch.data(), winlen, winlen / 2);
  _selector.selectPeaks(_spectrum, _params.freqFloor, _peakFrame);

  const double frameTime = _frameSample / double(_params.sampleRate);
  thinPeaks(_peakFrame, _params.resolution, _params.ampFloor, frameTime);
  if (_params.bwRegionWidth > 0.f)
  {
    _bwAssociator.associateBandwidth(_peakFrame);
  }
  _tracker.buildFrame(_peakFrame, _frameSample);
  _frameSample += _hopSamples;
}

void PartialAnalyzer::pushSamples(const float* src, size_t n)
{
  assert(_configured);
  const size_t winlen = size_t(_spectrum.windowLength());
  while (n > 0)
  {
    const size_t m = std::min(n, _buffer.getWriteAvailable());
    if (m > 0)
    {
      _buffer.write(src, m);
      src += m;
      n -= m;
      _samplesPushed += m;
    }
    while (_buffer.getReadAvailable() >= winlen)
    {
      processHop();
    }
  }
}

void PartialAnalyzer::finish()
{
  assert(_configured);
  // process remaining frames with centers before the end of the input,
  // padding silence as needed
  const long winlen = _spectrum.windowLength();
  std::fill(_windowScratch.begin(), _windowScratch.end(), 0.f);
  while (_frameSample < _samplesPushed)
  {
    while ((_buffer.getReadAvailable() < size_t(winlen)) && (_frameSample < _samplesPushed))
    {
      const size_t pad = std::min(size_t(winlen), _buffer.getWriteAvailable());
      _buffer.write(_windowScratch.data(), pad);
      std::fill(_windowScratch.begin(), _windowScratch.end(), 0.f);
    }
    if (_buffer.getReadAvailable() >= size_t(winlen))
    {
      processHop();
    }
  }
}

std::unique_ptr<VutuPartialsData> PartialAnalyzer::takePartials()
{
  assert(_configured);
  auto& built = _tracker.partials();
  if (_params.phaseCorrect)
  {
    fixFrequency(built, _params.sampleRate);
  }

  auto out = std::make_unique<VutuPartialsData>();
  out->partials.reserve(built.size());
  const double sr = _params.sampleRate;
  for (const BuildingPartial& bp : built)
  {
    VutuPartial vp;
    const size_t n = bp.size();
    vp.time.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
      vp.time[i] = float(bp.timeAt(i, sr));
    }
    vp.freq = bp.freq;
    vp.amp = bp.amp;
    vp.bandwidth = bp.bw;
    vp.phase = bp.phase;
    out->partials.push_back(std::move(vp));
  }

  out->resolution = _params.resolution;
  out->windowWidth = _params.windowWidth;
  out->ampFloor = _params.ampFloor;
  out->freqDrift = _params.freqDrift;
  out->loCut = _params.freqFloor;

  _tracker.reset();
  reset();
  return out;
}

std::unique_ptr<VutuPartialsData> analyzeToPartials(const float* samples, size_t n,
                                                    const AnalyzerParams& params)
{
  PartialAnalyzer analyzer;
  analyzer.configure(params);
  analyzer.pushSamples(samples, n);
  analyzer.finish();
  return analyzer.takePartials();
}

}  // namespace ml::utu
