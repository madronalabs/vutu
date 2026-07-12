
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <memory>

#include "MLDSPBuffer.h"
#include "utuBandwidth.h"
#include "utuPeaks.h"
#include "utuSpectrum.h"
#include "utuTracker.h"
#include "vutuPartials.h"

namespace ml::utu
{

// Analysis parameters, mirroring Loris Analyzer::configure: a zero means
// "derive the Loris default from resolution / windowWidth / ampFloor".
//
// resolution and windowWidth are the separability choices of Fitz & Fulop
// Sec. 5: components closer in frequency than the window's main lobe
// (windowWidth), or events closer in time than the window length, cannot be
// resolved — by reassignment or by anything else. The hop of 1/windowWidth
// then critically samples the analysis in time.
struct AnalyzerParams
{
  float sampleRate{48000.f};
  float resolution{80.f};       // Hz, partial frequency spacing
  float windowWidth{0.f};       // Hz, main lobe width; 0 -> 2·resolution
  float ampFloor{-90.f};        // dB
  float freqFloor{0.f};         // Hz; 0 -> resolution
  float freqDrift{0.f};         // Hz; 0 -> 0.5·resolution
  float sidelobeLevel{0.f};     // dB; 0 -> -ampFloor
  float hopTime{0.f};           // s; 0 -> 1/windowWidth
  float cropTime{0.f};          // s; 0 -> hopTime
  float bwRegionWidth{2000.f};  // Hz; 0 disables residue bandwidth
  bool phaseCorrect{true};

  // experimental variants, all off by default
  float hopJitter{0.f};           // 0..0.5: randomize each hop by ±this
                                  // fraction. Estimation error is coherent at
                                  // the frame rate (every partial jitters in
                                  // step, putting pitched sidebands around
                                  // every component); dithering the hop
                                  // decorrelates it into broadband noise
  float driftTransientScale{1.f}; // >1: widen freqDrift by up to this factor
                                  // at transients (follow glides and chaotic
                                  // onsets), lock to the base drift during
                                  // sustains (no neighbor capture)
  bool onsetSnap{false};          // place a frame center on each detected
                                  // onset so every attack gets a breakpoint
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
  void detectTransients(const float* src, size_t n);
  float transientLevelAt(int64_t sample) const;

  AnalyzerParams _params;  // with derived fields resolved
  ReassignedSpectrum _spectrum;
  PeakSelector _selector;
  AssociateBandwidth _bwAssociator;
  PartialTracker _tracker;
  DSPBuffer _buffer;
  PeakFrame _peakFrame;
  std::vector<float> _windowScratch;
  long _hopSamples{0};
  int64_t _frameSample{0};    // center of the next frame, in samples from input start
  int64_t _samplesPushed{0};
  bool _configured{false};

  // causal 2 ms block-RMS transient detector (drives driftTransientScale
  // and onsetSnap); rises indexed by absolute block in a small ring
  static constexpr size_t kRiseRing = 4096;
  size_t _envBlockSize{88};
  double _envAcc{0.};
  size_t _envAccCount{0};
  int64_t _envBlock{0};
  float _prevBlockDb{-160.f};
  std::vector<float> _blockRise;  // dB per ms
  int64_t _snapTarget{-1};
  uint32_t _rng{0x9E3779B9u};
};

// offline convenience wrapper
std::unique_ptr<VutuPartialsData> analyzeToPartials(const float* samples, size_t n,
                                                    const AnalyzerParams& params);

}  // namespace ml::utu
