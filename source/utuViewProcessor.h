// utu-view
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "mldsp.h"
#include "madronalib.h"
#include "MLPlatform.h"
#include "MLSignalProcessor.h"
#include "MLDSPUtils.h"
#include "utuViewParameters.h"
#include "MLRtAudioProcessor.h"
#include "MLActor.h"
#include "MLMath2D.h"

#include "utuDSP.h"

using namespace ml;

constexpr int kMaxProcessBlockFrames = 4096;
constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;
constexpr float kOutputGain = 0.1f;
constexpr float kFreqLo = 40, kFreqHi = 4000;

void readParameterDescriptions(ParameterDescriptionList& params);


class UtuViewProcessor final :
public RtAudioProcessor
{
  // sine generators.
  SineGen s1, s2;
  
public:
  UtuViewProcessor(size_t nInputs, size_t nOutputs, int sampleRate, const ParameterDescriptionList& pdl) :
  RtAudioProcessor(nInputs, nOutputs, sampleRate)
  {
    buildParameterTree(pdl, _params);
    setDefaults(_params);
  }
  
  ~UtuViewProcessor() = default;
  
  void processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused) override;

  void onMessage(Message msg) override;

};
