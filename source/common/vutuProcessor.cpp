// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "vutuProcessor.h"
#include "vutuController.h"
#include "vutuParameters.h"

#include <cmath>
#include <cstdlib>
#include <math.h>
#include <iostream>

#include "libresample.h"

// Loris includes
#include "loris.h"
#include "PartialList.h"
#include "Synthesizer.h"

using namespace ml;

constexpr float kSizeLo = 0, kSizeHi = 40;
constexpr float kToneLo = 250, kToneHi = 4000;
constexpr float kDecayLo = 0.8, kDecayHi = 20;
constexpr float kLevelLo = 0.5f, kLevelHi = 2.f;

size_t getStartFrame(const ml::Sample& sample, Interval srcInterval)
{
  auto interval = srcInterval * getFrames(sample);
  return interval.x1;
}

size_t getEndFrame(const ml::Sample& sample, Interval srcInterval)
{
  auto interval = srcInterval * getFrames(sample);
  return interval.x2;
}


void readParameterDescriptions(ParameterDescriptionList& params)
{
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "resolution" },
    { "range", { 8, 1024 } },
    { "plaindefault", 40 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "window_width" },
    { "range", {16, 768} },
    { "plaindefault", 80 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "amp_floor" },
    { "range", {-90, -20} },
    { "plaindefault", -60 },
    { "units", "dB" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "freq_drift" },
    { "range", {2, 80} },
    { "plaindefault", 40 },
    { "log", false },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "lo_cut" },
    { "range", {20, 2000} },
    { "plaindefault", 20 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "hi_cut" },
    { "range", {200, 20000} },
    { "plaindefault", 20000 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "noise_width" },
    { "range", {10, 5000} },
    { "plaindefault", 500 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "fundamental" },
    { "range", {22, 2200} },
    { "plaindefault", 220 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "test_volume" },
    { "range", {0, 0.5f} },
    { "log", false },
    { "plaindefault", 0.f }
    
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "output_volume" },
    { "range", {-60, 0} },
    { "log", false },
    { "plaindefault", -6 },
    { "units", "dB" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "sample/start_time" },
    { "range", {0, 1} },
    { "default", 0.f }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "sample/end_time" },
    { "range", {0, 1} },
    { "default", 1.f }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "sample_duration" },
    { "range", {0, 1} },
    { "default", 1.f }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "analysis_interval" },
    { "default", Value{0.f, 1.f} }
  } ) );
}

// TODO sample utilities
// TODO channels > 1
void resample(const ml::Sample* pSrc, ml::Sample* pDest)
{
  if(!usable(pSrc)) return;
  
  int srcLen = getFrames(*pSrc);
  double factor = getRate(*pDest) / getRate(*pSrc);
    
  // make mono sample
  int expectedLen = (int)(srcLen * factor);
  resize(*pDest, expectedLen, 1);

  constexpr int srcBlockSize{1024};
  constexpr int destBlockSize{1024};
  int srcSamplesUsed{0};
  int resampled{0};

  void *resamplerHandle = resample_open(1, factor, factor);
  int fwidth = resample_get_filter_width(resamplerHandle);
  int destIdx = 0;
  int srcIdx = 0;
  for(;;) {
    int srcBlock = std::min(srcLen-srcIdx, srcBlockSize);
    int lastFlag = (srcBlock == srcLen-srcIdx);
    
    resampled = resample_process(resamplerHandle, factor,
                         getConstFramePtr(*pSrc, srcIdx), srcBlock,
                         lastFlag, &srcSamplesUsed,
                         getFramePtr(*pDest, destIdx), std::min(expectedLen-destIdx, destBlockSize));
    srcIdx += srcSamplesUsed;
    if (resampled >= 0)
      destIdx += resampled;
    if (resampled < 0 || (resampled == 0 && srcIdx == srcLen))
      break;
  }
  resample_close(resamplerHandle);
  
  if (resampled < 0) {
    printf("Error: resample_process returned an error: %d\n", resampled);
  }
  
  if (destIdx <= 0) {
    printf("Error: resample_process returned %d samples\n", destIdx);
    return;
  }
  
  int lendiff = abs(destIdx - expectedLen);
  if (lendiff > (int)(2*factor + 1.0)) {
    std::cout << "   Expected " << expectedLen << " samples, got " << destIdx << " out\n";
  }
  
  std::cout << "resampled: " << getSize(*pSrc) << " -> " << getSize(*pDest) << "\n";
  
}

VutuProcessor::VutuProcessor(TextFragment appName, size_t instanceNum,
                             const ParameterDescriptionList& pdl)
{
  // get names of other Actors we might communicate with
  _controllerName = TextFragment(appName, "controller", ml::textUtils::naturalNumberToText(instanceNum));

  // register ourself
  auto myName = TextFragment(appName, "processor", ml::textUtils::naturalNumberToText(instanceNum));
  registerActor(myName, this);

  buildParams(pdl);
  setDefaultParams();
}

// audio process function: runs DSP in vectors of size kFramesPerBlock,
// invoked by AudioTask through the AudioContext.
void processVutu(AudioContext* ctx, VutuProcessor* state)
{
  auto& params = state->getParameterTree();
  int sr = ctx->getSampleRate();
  if(sr <= 0) sr = kSampleRate;

  state->testCounter += kFramesPerBlock;
  if(state->testCounter >= sr)
  {
    state->testCounter -= sr;
  }

  // Use the getRealFloatValue / getRealValue macros (not the *AtPath variants):
  // they expand to a compile-time HashPath lookup with zero runtime cost. The
  // *AtPath forms construct a Path from const char* via runtimePath(), which
  // takes the SymbolTable mutex — not safe to call from the audio thread.
  float gain = params.getRealFloatValue("output_volume");
  float amp = dBToAmp(gain);

  // test amp is not in dB so it can go to 0. TODO -inf dB setting
  float testAmp = params.getRealFloatValue("test_volume");
  float testFreq = params.getRealFloatValue("fundamental");

  auto sineVec = state->testSine(testFreq / sr)*SignalBlock(testAmp);

  SignalBlock sampleVec;

  ml::Sample* samplePlaying{ nullptr };
  Symbol viewProperty;

  size_t frameEnd{0};

  if(state->playbackState == "source")
  {
    // source: play analysis interval portion
    samplePlaying = &state->_sourceSample;
    auto arr = params.getRealValue("analysis_interval").getFloatArray<2>();
    Interval interval{arr[0], arr[1]};
    frameEnd = getEndFrame(*samplePlaying, interval);

    viewProperty = "source_time";
  }
  else if(state->playbackState == "synth")
  {
    // synthesized: play entire length
    samplePlaying = state->_pSynthesizedSample;
    frameEnd = getEndFrame(*samplePlaying, {0, 1});
    viewProperty = "synth_time";
  }

  if(samplePlaying)
  {
    if(getFrames(*samplePlaying) > 0)
    {
      load(sampleVec, getFramePtr(*samplePlaying, state->playbackSampleIdx));
      state->playbackSampleIdx += kFramesPerBlock;
    }

    if(state->playbackSampleIdx >= frameEnd - kFramesPerBlock)
    {
      state->playbackState = "off";
      state->playbackSampleIdx = 0;
      sendMessageToActor(state->_controllerName, Message{"do/playback_stopped"});
    }

    float playbackTime = float(state->playbackSampleIdx) / float(sr);

    sendMessageToActor(state->_controllerName, Message{Path{Path{"set_prop"}, runtimePath(viewProperty)}, playbackTime});
  }

  ctx->outputs[0] = ctx->outputs[1] = sampleVec*amp + sineVec;
}

// toggle current playback state and tell controller
void VutuProcessor::togglePlaybackState(Symbol whichSample)
{
  // if either sample is playing, stop both
  auto prevState = playbackState;
  if(playbackState != "off")
  {
    playbackState = "off";
    playbackSampleIdx = 0;
    sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
    sendMessageToActor(_controllerName, Message{"set_prop/source_time", 0});
    sendMessageToActor(_controllerName, Message{"set_prop/synth_time", 0});
  }
  
  if(whichSample == "source")
  {
    if(prevState != "source")
    {
      if(getFrames(_sourceSample) > 0)
      {
        // start playback at analysis interval start
        playbackState = "source";
        auto arr = params_.getRealValue("analysis_interval").getFloatArray<2>();
        Interval interval{arr[0], arr[1]};
        playbackSampleIdx = getStartFrame(_sourceSample, interval);
        sendMessageToActor(_controllerName, Message{"do/playback_started/source"});
      }
    }
  }
  else if(whichSample == "synth")
  {
    if(prevState != "synth")
    {
      if(usable(_pSynthesizedSample))
      {
        // synthesized sample always starts from beginning
        playbackState = "synth";
        playbackSampleIdx = 0;
        sendMessageToActor(_controllerName, Message{"do/playback_started/synth"});
      }
    }
  }
}

void VutuProcessor::onMessage(Message msg)
{
//  std::cout << "VutuProcessor: " << msg.address << " -> " << msg.value << "\n";
  
  switch(hash(head(msg.address)))
  {
    case(hash("set_param")):
    {
      params_.setFromNormalizedValue(tail(msg.address), msg.value);
      break;
    }
    case(hash("set_prop")):
    {
      break;
    }
    case(hash("do")):
    {
      switch(hash(second(msg.address)))
      {
        case(hash("set_source_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          _pSourceSampleInController = *reinterpret_cast<ml::Sample* const*>(msg.value.data());
          
          int currentSampleRate = kSampleRate;
//          std::cout << "VutuProcessor: sr = " << currentSampleRate << "\n";
  //        std::cout << "    sample input: sr = " << _pSourceSampleInController->sampleRate << "\n";
          
          // resample to current system sample rate for playback
          _sourceSample.sampleRate = currentSampleRate;
          
          resample(_pSourceSampleInController, &_sourceSample);
          break;
        }
          
        case(hash("set_loris_partials_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          Loris::PartialList* pPartials = *reinterpret_cast<Loris::PartialList* const*>(msg.value.data());
          _pLorisPartials = *reinterpret_cast<Loris::PartialList* const*>(msg.value.data());
          

 //         std::cout << "VutuProcessor: got new loris partials: n = " << _pLorisPartials->size() << "\n";

          break;
        }

        case(hash("set_synth_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          _pSynthesizedSample = *reinterpret_cast<ml::Sample* const*>(msg.value.data());

          break;
        }
          
        case(hash("toggle_play")):
        {
          // play either source or synth
          togglePlaybackState(third(msg.address));
          break;
        }

      }
      break;
    }
    default:
    {
      std::cout << " VutuProcessor: uncaught message " << msg << "! \n";
      break;
    }
  }
}
