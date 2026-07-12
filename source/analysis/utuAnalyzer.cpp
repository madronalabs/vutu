
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
    _bwAssociator.configure(_params.bwRegionWidth, _params.sampleRate);
  }
  _tracker.configure(_params.freqDrift, _params.sampleRate);

  _hopSamples = long(_params.hopTime * _params.sampleRate);  // truncated, as in Loris
  const long winlen = _spectrum.windowLength();
  _windowScratch.assign(winlen, 0.f);
  _buffer.resize(int(4 * winlen));
  _envBlockSize = std::max(size_t(1), size_t(0.002 * _params.sampleRate + 0.5));
  _blockRise.assign(kRiseRing, 0.f);
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
  _envAcc = 0.;
  _envAccCount = 0;
  _envBlock = 0;
  _prevBlockDb = -160.f;
  std::fill(_blockRise.begin(), _blockRise.end(), 0.f);
  _snapTarget = -1;
  _rng = 0x9E3779B9u;

  // pre-write half a window of silence so the first frame is centered on
  // the first input sample; zero samples here are equivalent to Loris's
  // clipped window at the buffer edges
  const long half = _spectrum.windowLength() / 2;
  std::fill(_windowScratch.begin(), _windowScratch.end(), 0.f);
  _buffer.write(_windowScratch.data(), half);
}

// causal 2 ms block-RMS rise tracking; rises are read back by frame center
void PartialAnalyzer::detectTransients(const float* src, size_t n)
{
  const float msPerBlock = float(1000. * _envBlockSize / _params.sampleRate);
  for (size_t i = 0; i < n; ++i)
  {
    _envAcc += double(src[i]) * src[i];
    if (++_envAccCount == _envBlockSize)
    {
      const float db = 10.f * log10f(float(std::max(1e-16, _envAcc / _envBlockSize)));
      const float rise = std::max(0.f, db - _prevBlockDb) / msPerBlock;
      _blockRise[size_t(_envBlock % int64_t(kRiseRing))] = rise;

      // onset: a sharp rise becomes a snap target for the frame clock,
      // one at a time, always ahead of the frames being processed
      if (_params.onsetSnap && (rise > 2.f) && (_snapTarget < 0))
      {
        const int64_t onsetSample = _envBlock * int64_t(_envBlockSize);
        if (onsetSample > _frameSample)
        {
          _snapTarget = onsetSample;
        }
      }
      _prevBlockDb = db;
      _envAcc = 0.;
      _envAccCount = 0;
      ++_envBlock;
    }
  }
}

float PartialAnalyzer::transientLevelAt(int64_t sample) const
{
  const int64_t block = sample / int64_t(_envBlockSize);
  if ((block < 0) || (block >= _envBlock)) return 0.f;
  const float rise = _blockRise[size_t(block % int64_t(kRiseRing))];
  // 1 dB/ms is the sustain/transient boundary used throughout; full
  // transient credit at 5 dB/ms
  return std::min(1.f, std::max(0.f, (rise - 1.f) / 4.f));
}

void PartialAnalyzer::processHop()
{
  const long winlen = _spectrum.windowLength();

  // decide the hop to the next frame before reading: the buffer read
  // advances by (winlen - overlap)
  long hopNow = _hopSamples;
  if (_params.hopJitter > 0.f)
  {
    // xorshift32, deterministic per run: dithering the hop decorrelates
    // frame-rate-coherent estimation error
    _rng ^= _rng << 13;
    _rng ^= _rng >> 17;
    _rng ^= _rng << 5;
    const float u = (int32_t(_rng) / 2147483648.f);  // [-1, 1)
    hopNow = std::max(1L, long(_hopSamples * (1.f + _params.hopJitter * u)));
  }
  if ((_snapTarget > _frameSample) && (_snapTarget <= _frameSample + hopNow))
  {
    hopNow = std::max(1L, long(_snapTarget - _frameSample));
    _snapTarget = -1;
  }
  hopNow = std::min(hopNow, winlen - 1L);

  _buffer.readWithOverlap(_windowScratch.data(), winlen, winlen - hopNow);

  _spectrum.transform(_windowScratch.data(), winlen, winlen / 2);
  _selector.selectPeaks(_spectrum, _params.freqFloor, _peakFrame);

  const double frameTime = _frameSample / double(_params.sampleRate);
  thinPeaks(_peakFrame, _params.resolution, _params.ampFloor, frameTime);
  if (_params.bwRegionWidth > 0.f)
  {
    _bwAssociator.associateBandwidth(_peakFrame);
  }

  // freqDrift widens toward transients so glides and chaotic onsets stay
  // on one track, and locks down in sustains so neighbors are not captured
  float drift = _params.freqDrift;
  if (_params.driftTransientScale > 1.f)
  {
    drift *= 1.f + (_params.driftTransientScale - 1.f) * transientLevelAt(_frameSample);
  }
  _tracker.buildFrame(_peakFrame, _frameSample, drift);
  _frameSample += hopNow;
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
      detectTransients(src, m);
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
