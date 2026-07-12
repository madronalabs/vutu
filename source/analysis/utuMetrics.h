
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <cstddef>

namespace ml::utu
{

// Automatic comparison of a resynthesis against its source. Three scores,
// each targeting a distinct audible failure; all in dB-like units where
// smaller is better. Insensitive to phase and to the noise realization of
// bandwidth enhancement by construction (band envelopes, not waveforms).
struct ReconstructionScore
{
  // RMS of mel-band log-magnitude envelope differences over active frames,
  // after removing the mean level offset: spectral-shape fidelity
  float spectralRmsDb{0.f};

  // mean over bands of *excess* 2-30 Hz modulation energy in the render
  // relative to the source: the "watery" artifact measured directly
  // (spurious low-rate AM that the source does not have)
  float wateryDb{0.f};

  // mean loss of onset rise slope (dB/ms) at the source's transients
  float transientDeficit{0.f};

  // weighted combination for optimization
  float total{0.f};
};

ReconstructionScore scoreReconstruction(const float* src, size_t nSrc, const float* render,
                                        size_t nRender, double sampleRate);

}  // namespace ml::utu
