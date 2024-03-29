// vutu
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "mldsp.h"
#include "madronalib.h"
#include "MLPlatform.h"
#include "MLSignalProcessor.h"
#include "MLDSPUtils.h"
#include "vutuParameters.h"
#include "MLRtAudioProcessor.h"
#include "MLActor.h"
#include "MLMath2D.h"
#include "MLDSPSample.h"

#include "loris.h"

using namespace ml;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;

void readParameterDescriptions(ParameterDescriptionList& params);


class VutuProcessor final :
public RtAudioProcessor
{
  // sine generators.
  SineGen testSine;
  
public:
  VutuProcessor(TextFragment appName, size_t instanceNum,
                   size_t nInputs, size_t nOutputs,
                   int sampleRate, const ParameterDescriptionList& pdl);
  ~VutuProcessor() = default;
  
  void processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused) override;

  void onMessage(Message msg) override;
  
private:
  
  Path _controllerName;
  
  int testCounter{0};
  
  Symbol playbackState{"off"};
  size_t playbackSampleIdx{0};
  //size_t playbackCounter{0};
  
  Interval analysisInterval{0, 0};
  
  ml::Sample* _pSourceSampleInController{nullptr};
  ml::Sample _sourceSample;
  ml::Sample* _pSynthesizedSample{nullptr};


  Loris::PartialList* _pLorisPartials{ nullptr };

  void togglePlaybackState(Symbol whichSample);

};
