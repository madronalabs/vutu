// vutu
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "mldsp.h"
#include "madronalib.h"
#include "MLPlatform.h"
#include "MLSignalProcessor.h"
#include "MLDSPUtils.h"
#include "MLAudioContext.h"
#include "MLAudioTask.h"
#include "MLActor.h"
#include "MLDSPSample.h"
#include "vutuParameters.h"

#include "loris.h"

using namespace ml;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;

void readParameterDescriptions(ParameterDescriptionList& params);

class VutuProcessor;

// audio process function, registered with AudioTask.
void processVutu(AudioContext* ctx, VutuProcessor* state);

class VutuProcessor final :
  public SignalProcessor,
  public Actor
{
public:
  VutuProcessor(TextFragment appName, size_t instanceNum,
                const ParameterDescriptionList& pdl);
  ~VutuProcessor() = default;

  void onMessage(Message msg) override;

private:
  friend void processVutu(AudioContext* ctx, VutuProcessor* state);

  // sine generator for test tone
  SineGen<float> testSine;

  Path _controllerName;

  int testCounter{0};

  Symbol playbackState{"off"};
  size_t playbackSampleIdx{0};

  Interval analysisInterval{0, 0};

  ml::Sample* _pSourceSampleInController{nullptr};
  ml::Sample _sourceSample;
  ml::Sample* _pSynthesizedSample{nullptr};

  Loris::PartialList* _pLorisPartials{ nullptr };

  void togglePlaybackState(Symbol whichSample);
};
