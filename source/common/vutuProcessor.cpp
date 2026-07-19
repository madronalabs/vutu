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
  // NB: the current madronalib honors "real_default" (a value in real units) or
  // "default" (normalized), but NOT the old mlvg "plaindefault" key. A param that
  // supplies only a range and an unrecognized default key silently falls back to
  // the range midpoint (normalized 0.5), so use "real_default" for all dials.
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "resolution" },
    { "range", { 8, 1024 } },
    { "real_default", 40 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "window_width" },
    { "range", {16, 768} },
    { "real_default", 80 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "amp_floor" },
    { "range", {-90, -20} },
    { "real_default", -60 },
    { "units", "dB" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "freq_drift" },
    { "range", {2, 80} },
    { "real_default", 40 },
    { "log", false },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "lo_cut" },
    { "range", {20, 2000} },
    { "real_default", 20 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "hi_cut" },
    { "range", {200, 20000} },
    { "real_default", 20000 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "noise_width" },
    { "range", {10, 5000} },
    { "real_default", 500 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "fundamental" },
    { "range", {22, 2200} },
    { "real_default", 220 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "test_volume" },
    { "range", {0, 0.5f} },
    { "log", false },
    { "real_default", 0.f }

  } ) );
  
  params.push_back( std::make_unique< ParameterDescription >(WithValues{
    { "name", "output_volume" },
    { "range", {-60, 0} },
    { "log", false },
    { "real_default", -6 },
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
    { "default", intervalToValue(Interval{0, 1}) }
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

// audio process function for the AudioTask. Forwards to the processor's DSP method.
void processVutu(AudioContext* ctx, VutuProcessor* state)
{
  state->processAudioVectors(ctx);
}

// run our DSP in vectors of size kFramesPerBlock for one AudioContext block.
void VutuProcessor::processAudioVectors(AudioContext* ctx)
{
  // TEST
  int sr = kSampleRate;
  testCounter += kFramesPerBlock;
  bool test{false};
  if(testCounter >= sr)
  {
    test = true;
    testCounter -= sr;
  }
  if(test)
  {
    //std::cout << "playbackState: " << playbackState << "\n";
    //std::cout << "playbackSampleIdx: " << playbackSampleIdx << "\n";
    //std::cout << "analysis interval: " << valueToInterval(params_.getRealValue("analysis_interval")) << "\n";
  }
  
  // get params from the SignalProcessor.
  float gain = params_.getRealFloatValue("output_volume");
  float amp = dBToAmp(gain);
  
  // test amp is not in dB so it can go to 0. TODO -inf dB setting
  float testAmp = params_.getRealFloatValue("test_volume");
  float testFreq = params_.getRealFloatValue("fundamental");

  auto sineVec = testSine(testFreq / sr)*SignalBlock(testAmp);

  SignalBlock sampleVec;
  
  ml::Sample* samplePlaying{ nullptr };
  Path viewProperty;


  size_t frameEnd;
  
  if(playbackState == "source")
  {
    // source: play analysis interval portion
    samplePlaying = &_sourceSample;
    auto interval = valueToInterval(params_.getRealValue("analysis_interval"));
    frameEnd = getEndFrame(*samplePlaying, interval);

    viewProperty = "source_time";
  }
  else if(playbackState == "synth")
  {
    // synthesized: play entire length
    samplePlaying = _pSynthesizedSample;
    frameEnd = getEndFrame(*samplePlaying, {0, 1});
    viewProperty = "synth_time";
  }

  if(samplePlaying)
  {
    if(getFrames(*samplePlaying) > 0)
    {
      load(sampleVec, getFramePtr(*samplePlaying, playbackSampleIdx));
      playbackSampleIdx += kFramesPerBlock;
    }
    
    if(playbackSampleIdx >= frameEnd - kFramesPerBlock)
    {
      playbackState = "off";
      playbackSampleIdx = 0;
      sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
    }
    
    float playbackTime = float(playbackSampleIdx) / float(sr);

    sendMessageToActor(_controllerName, Message{Path{"set_prop", viewProperty}, playbackTime});
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
        auto interval = valueToInterval(params_.getRealValue("analysis_interval"));
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
      params_.setFromRealValue(tail(msg.address), msg.value);
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
