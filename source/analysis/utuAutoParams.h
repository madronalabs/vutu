
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <cstddef>

#include "utuAnalyzer.h"

namespace ml::utu
{

// Result of automatic analysis-parameter estimation. hiCut is returned
// beside the AnalyzerParams because the analyzer itself has no hiCut — vutu
// applies it downstream via cutHighs(). params.sidelobeLevel is always set
// explicitly (90 dB): by default it would follow -ampFloor, so raising the
// floor to meet the partial budget would silently degrade sidelobe
// rejection and reintroduce the leakage beating this function exists to
// prevent.
struct AutoAnalyzerParams
{
  AnalyzerParams params;
  float hiCut{20000.f};

  float fundamental{0.f};     // Hz; 0 when no confident pitch was found
  float pitchConfidence{0.f}; // normalized autocorrelation peak, 0..1

  // diagnostics
  float noiseFloorDb{-90.f};      // median in-band spectral noise floor
  float spacingHz{0.f};           // dominant partial spacing (spectral ACF)
  float minSpacingHz{0.f};        // 10th-percentile adjacent peak spacing
  float frameRateHz{0.f};         // actual frame rate after hop truncation
  int probedSimultaneousP90{0};   // p90 kept peaks per frame at final params
  int budget{0};                  // the requested simultaneous-partial budget
  bool budgetLimited{false};      // true: quality ascent stopped at the budget;
                                  // false: every quality ladder was exhausted
  float activeDuration{0.f};      // seconds of non-silent material measured

  // regime diagnostics: dense material (measured beating / dispersed
  // spacings) has its window chosen first, wide enough to merge the beat
  // cluster, with resolution following as half the main lobe
  bool dense{false};
  float beatIndexDb{-120.f};  // absolute beat-band modulation index of the
                              // tonal band envelopes, dB
  float beatFraction{0.f};    // beat-band share of all modulation from 2 Hz up
  float beatRateHz{0.f};      // p90 beat rate
  float mergeWindowHz{0.f};   // minimum window width demanded by the beats
};

// Estimate analysis parameters likely to produce a faithful reconstruction
// of the given sound with at most maxSimultaneousPartials concurrent
// bandwidth-enhanced partials. Offline and noncausal: the whole file is
// examined. Pure DSP and order statistics; every rule is commented with its
// justification in utuAutoParams.cpp.
AutoAnalyzerParams computeAnalyzerParams(const float* samples, size_t n, float sampleRate,
                                         int maxSimultaneousPartials = 64);

}  // namespace ml::utu
