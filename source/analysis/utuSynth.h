
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

// Bandwidth-enhanced sinusoidal synthesizer.
//
// Each partial is turned into a schedule of linear segments with
// sample-quantized boundaries (stored phases corrected for the
// quantization shift) wrapped in fadeTime onset/offset ramps. The
// oscillator integrates frequency per sample in double — arrival phase per
// segment is the π(f0+f1)·dt travel the phase-fix pass reconciled — and
// snaps to the stored breakpoint phase wherever amplitude leaves zero: the
// phase-correct rendering discipline of Fitz & Fulop Sec. 8. A
// noise-through-lowpass modulator supplies the bandwidth component:
//   out += amp · (sqrt(1-bw) + nz·sqrt(2·bw)) · cos(phase)
//
// The noise modulator is uniform noise through two lowpass SVFs,
// gain-matched to the model's modulation energy; the audible result is
// validated by listening.
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
