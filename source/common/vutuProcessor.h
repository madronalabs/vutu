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
#include "MLAudioContext.h"
#include "MLAudioTask.h"
#include "MLActor.h"
#include "MZMath2D.h"
#include "MLDSPSample.h"

using namespace ml;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;

void readParameterDescriptions(ParameterDescriptionList& params);


class VutuProcessor final :
public SignalProcessor, public Actor
{
  // sine generators.
  SineGen< float > testSine;

public:
  VutuProcessor(TextFragment appName, size_t instanceNum,
                   const ParameterDescriptionList& pdl);
  ~VutuProcessor() = default;

  // run our DSP for one AudioContext block. Called by the processVutu() free function.
  void processAudioVectors(AudioContext* ctx);

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

  void togglePlaybackState(Symbol whichSample);

};

// audio process function for the AudioTask: runs VutuProcessor's DSP for one block.
void processVutu(AudioContext* ctx, VutuProcessor* state);
