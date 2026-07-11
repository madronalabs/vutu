
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <cstdint>
#include <vector>

#include "vutuPartials.h"

namespace ml::utu
{

struct SynthParams
{
  float sampleRate{48000.f};
  float fadeTime{0.001f};  // seconds of onset/offset fade added around partials
};

// Bandwidth-enhanced sinusoidal synthesizer replacing Loris::Synthesizer.
//
// Each partial is turned into a schedule of linear segments with
// sample-quantized boundaries and a start phase precomputed (in double, at
// schedule build) by Loris's rules: phase is anchored to the stored
// breakpoint phase wherever the amplitude leaves zero, and evolves by
// midpoint-frequency integration elsewhere. Rendering is then block-based:
// per 64-sample block, per 4-voice group, signal-rate freq/amp/bandwidth
// rows drive a bank of accurate sine generators (madronalib TestSineGen)
// whose phases are re-anchored analytically every block, with a
// noise-through-lowpass modulator supplying the bandwidth component:
//   out += amp · (sqrt(1-bw) + nz·sqrt(2·bw)) · sin(phase)
//
// The noise modulator deviates from Loris (uniform noise through an SVF
// lowpass instead of Gaussian through a 3rd-order Chebyshev), gain-matched
// to the same modulation energy; the audible result is validated by
// listening.
class PartialSynthesizer
{
 public:
  void setParams(const SynthParams& p) { _params = p; }

  // render all partials, accumulating into out (resized to fit)
  void render(const VutuPartialsData& partials, std::vector<float>& out);

 private:
  SynthParams _params;
};

}  // namespace ml::utu
