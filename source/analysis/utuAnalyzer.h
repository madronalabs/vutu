
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <memory>

#include "utuBandwidth.h"
#include "utuPeaks.h"
#include "utuSpectrum.h"
#include "utuTracker.h"
#include "vutuPartials.h"

namespace ml::utu
{

// Analysis parameters. The window width is the master knob; a zero in any
// other field derives it from the invariants the v2 tuning work found, and
// an explicit nonzero value overrides.
//
// windowWidth and resolution are the separability choices of Fitz & Fulop
// Sec. 5: components closer in frequency than the window's main lobe
// (windowWidth), or events closer in time than the window length, cannot be
// resolved — by reassignment or by anything else. resolution = W/2 keeps
// every pair either cleanly resolved or cleanly merged: nothing
// partially-resolved survives thinning to become masked noise. The hop of
// 1/windowWidth critically samples the analysis in time, and noiseWidth =
// W/3.4 keeps the residue-collection radius inside the consensus-clean zone
// around each kept partial (ear-calibrated; sparse sounds may want wider
// regions — computeAnalyzerParams sets that by regime).
struct AnalyzerParams
{
  float sampleRate{48000.f};
  float windowWidth{160.f};   // Hz, main lobe width — the master knob
  float resolution{0.f};      // Hz; 0 -> windowWidth/2
  float noiseWidth{0.f};      // Hz, residue region width; 0 -> windowWidth/3.4
  float ampFloor{-90.f};      // dB
  float freqFloor{0.f};       // Hz; 0 -> resolution
  float freqDrift{0.f};       // Hz; 0 -> 0.5·resolution
  float sidelobeLevel{0.f};   // dB; 0 -> -ampFloor
  float hopTime{0.f};         // s; 0 -> 1/windowWidth
  float cropTime{0.f};        // s; 0 -> hopTime
  float hopJitter{0.f};       // fraction of a hop (0..0.5); each frame center
                              // is offset by a uniform random ±hopJitter·hop.
                              // Experiment: decorrelates frame-rate-coherent
                              // artifacts (the flanged/ratcheting residue on
                              // breathy material). 0 = off, the regular grid
  bool associateNoise{true};  // false disables residue bandwidth association
  bool phaseCorrect{true};
};

// The analyzer: reassigned spectrum -> peaks -> thinning -> bandwidth
// association -> partial tracking, one hop at a time, with a phase/frequency
// consistency pass at the end. The core is push-based and holds only a
// window of history, so a future real-time version can stream into it.
class PartialAnalyzer
{
 public:
  void configure(const AnalyzerParams& p);
  void reset();

  // stream samples in; complete hops are processed as they become available
  void pushSamples(const float* src, size_t n);

  // flush trailing frames (window centers up to the last pushed sample)
  void finish();

  // run phase correction and emit partials; resets the analyzer
  std::unique_ptr<VutuPartialsData> takePartials();

  long windowLength() const { return _spectrum.windowLength(); }
  long hopSamples() const { return _hopSamples; }
  const AnalyzerParams& params() const { return _params; }

 private:
  void processHop();

  AnalyzerParams _params;  // with derived fields resolved
  ReassignedSpectrum _spectrum;
  PeakSelector _selector;
  AssociateBandwidth _bwAssociator;
  PartialTracker _tracker;
  PeakFrame _peakFrame;

  // history of recent input: absolute sample _historyStart + i lives at
  // _history[i]; the front is dropped once frames no longer reach it
  std::vector<float> _history;
  int64_t _historyStart{0};

  long _hopSamples{0};
  long _maxJitterSamples{0};
  uint32_t _jitterState{0};
  int64_t _frameSample{0};  // nominal center of the next frame (un-jittered grid)
  int64_t _samplesPushed{0};
  bool _configured{false};
};

// offline convenience wrapper
std::unique_ptr<VutuPartialsData> analyzeToPartials(const float* samples, size_t n,
                                                    const AnalyzerParams& params);

}  // namespace ml::utu
