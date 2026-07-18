
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// The analysis pipeline shell: streams samples through the reassigned
// spectrum, peak selection/thinning, residue bandwidth association and
// partial tracking, one hop at a time. Written from the paper (Fitz &
// Fulop) and the module specs; no Loris source consulted.

#include "utuAnalyzer.h"

#include <algorithm>

#include "utuPhaseFix.h"

namespace ml::utu
{

namespace
{

// residue-collection radius over window width, ear-calibrated: residue must
// be collected inside the consensus-clean zone (~W/2) around kept partials
constexpr float kWindowToNoiseRatio = 3.4f;

}  // namespace

void PartialAnalyzer::configure(const AnalyzerParams& p)
{
  _params = p;

  // resolve the window-primary derivations; explicit nonzero values win
  auto& q = _params;
  if (q.windowWidth <= 0.f)
  {
    // legacy resolution-first callers
    q.windowWidth = (q.resolution > 0.f) ? 2.f * q.resolution : 160.f;
  }
  if (q.resolution <= 0.f) q.resolution = 0.5f * q.windowWidth;
  if (q.noiseWidth <= 0.f) q.noiseWidth = q.windowWidth / kWindowToNoiseRatio;
  if (q.freqFloor <= 0.f) q.freqFloor = q.resolution;
  if (q.freqDrift <= 0.f) q.freqDrift = 0.5f * q.resolution;
  if (q.sidelobeLevel <= 0.f) q.sidelobeLevel = -q.ampFloor;
  if (q.hopTime <= 0.f) q.hopTime = 1.f / q.windowWidth;
  if (q.cropTime <= 0.f) q.cropTime = q.hopTime;

  _spectrum.configure(buildReassignmentWindows(q.sampleRate, q.windowWidth, q.sidelobeLevel));
  _selector.configure(q.sampleRate, q.cropTime);
  // truncated: the frame grid must land on integer samples
  _hopSamples = std::max(1L, long(q.hopTime * q.sampleRate));
  if (q.associateNoise)
  {
    // residue is gated at half a hop so each residue quantum lands in the
    // one frame nearest its reassigned time (see utuBandwidth.h)
    _bwAssociator.configure(q.noiseWidth, q.sampleRate, 0.5f * q.hopTime);
  }
  _tracker.configure(q.freqDrift, q.sampleRate);

  reset();
  _configured = true;
}

void PartialAnalyzer::reset()
{
  _history.clear();
  _historyStart = 0;
  _frameSample = 0;
  _samplesPushed = 0;
  _tracker.reset();
}

void PartialAnalyzer::processHop()
{
  _spectrum.transform(_history.data(), long(_history.size()),
                      long(_frameSample - _historyStart));
  _selector.selectPeaks(_spectrum, _params.freqFloor, _peakFrame);
  thinPeaks(_peakFrame, _params.resolution, _params.ampFloor,
            _frameSample / double(_params.sampleRate));
  if (_params.associateNoise)
  {
    _bwAssociator.associateBandwidth(_peakFrame);
  }
  _tracker.buildFrame(_peakFrame, _frameSample);
  _frameSample += _hopSamples;

  // drop history no future frame reaches (window left edge of the next
  // frame); the transform sees zeros outside the kept range anyway
  const long half = (windowLength() - 1) / 2;
  const int64_t needed = _frameSample - half;
  if (needed - _historyStart > 4 * _hopSamples)
  {
    const long drop = long(needed - _historyStart);
    _history.erase(_history.begin(), _history.begin() + drop);
    _historyStart = needed;
  }
}

void PartialAnalyzer::pushSamples(const float* src, size_t n)
{
  _history.insert(_history.end(), src, src + n);
  _samplesPushed += n;

  // a frame is ready once the window's right edge is inside the input;
  // the first frames' left edges see leading zeros (frame 0 is centered on
  // sample 0, as if preceded by half a window of silence)
  const long half = (windowLength() - 1) / 2;
  while (_frameSample + half < _samplesPushed)
  {
    processHop();
  }
}

void PartialAnalyzer::finish()
{
  // flush frames whose centers lie within the input; their right edges see
  // trailing zeros
  while (_frameSample < _samplesPushed)
  {
    processHop();
  }
}

std::unique_ptr<VutuPartialsData> PartialAnalyzer::takePartials()
{
  auto& built = _tracker.partials();
  if (_params.phaseCorrect)
  {
    fixFrequency(built, _params.sampleRate);
  }

  auto out = std::make_unique<VutuPartialsData>();
  out->partials.reserve(built.size());
  for (const BuildingPartial& b : built)
  {
    if (b.size() == 0) continue;
    VutuPartial p;
    const size_t n = b.size();
    p.time.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
      p.time.push_back(float(b.timeAt(i, _params.sampleRate)));
    }
    p.freq = b.freq;
    p.amp = b.amp;
    p.bandwidth = b.bw;
    p.phase = b.phase;
    out->partials.push_back(std::move(p));
  }

  out->resolution = _params.resolution;
  out->windowWidth = _params.windowWidth;
  out->ampFloor = _params.ampFloor;
  out->freqDrift = _params.freqDrift;
  out->loCut = _params.freqFloor;

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
