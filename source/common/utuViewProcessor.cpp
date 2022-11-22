// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "utuViewProcessor.h"
#include "utuViewController.h"
#include "utuViewParameters.h"

#include <cmath>
#include <cstdlib>
#include <math.h>
#include <iostream>

#include "libresample.h"

using namespace ml;
using namespace sumu;

constexpr float kSizeLo = 0, kSizeHi = 40;
constexpr float kToneLo = 250, kToneHi = 4000;
constexpr float kDecayLo = 0.8, kDecayHi = 20;
constexpr float kLevelLo = 0.5f, kLevelHi = 2.f;

void readParameterDescriptions(ParameterDescriptionList& params)
{
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "resolution" },
    { "range", { 20, 2000 } },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "window_width" },
    { "range", {40, 4000} },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "amp_floor" },
    { "range", {-100, -30} },
    { "plaindefault", -90 },
    { "units", "dB" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "freq_drift" },
    { "range", {3, 300} },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "master_volume" },
    { "range", {-60, 6} },
    { "log", false },
    { "plaindefault", -15 },
    { "units", "dB" }
  } ) );
}


void normalize(Sample* pSrc)
{
  if(!pSrc) return;
  
  // get max
  float xMax{0.f};
  for(int i=0; i<pSrc->data.size(); ++i)
  {
    float x = pSrc->data[i];
    xMax = std::max(xMax, x);
  }
  
  // multiply
  float ratio = 1.0f / xMax;
  for(int i=0; i<pSrc->data.size(); ++i)
  {
    pSrc->data[i] *= ratio;
  }
}

void resample(const Sample* pSrc, Sample* pDest)
  {
  int srcLen = pSrc->data.size();
  double factor = double(pDest->sampleRate) / double(pSrc->sampleRate);
    
  int expectedLen = (int)(srcLen * factor);
  int dstLen = expectedLen;
  pDest->data.resize(expectedLen);

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
                         &pSrc->data[srcIdx], srcBlock,
                         lastFlag, &srcSamplesUsed,
                         &pDest->data[destIdx], std::min(dstLen-destIdx, destBlockSize));
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
  
  std::cout << "resampled: " << pSrc->data.size() << " -> " << pDest->data.size() << "\n";
  
}

UtuViewProcessor::UtuViewProcessor(TextFragment appName, size_t instanceNum,
                 size_t nInputs, size_t nOutputs,
                 int sampleRate, const ParameterDescriptionList& pdl) :
RtAudioProcessor(nInputs, nOutputs, sampleRate)
{
  // get names of other Actors we might communicate with
  _controllerName = TextFragment(appName, "controller", ml::textUtils::naturalNumberToText(instanceNum));
  
  // register ourself
  auto myName = TextFragment(appName, "processor", ml::textUtils::naturalNumberToText(instanceNum));
  registerActor(myName, this);
  
  
  buildParameterTree(pdl, _params);
  setDefaults(_params);
}

// declare the processVector function that will run our DSP in vectors of size kFloatsPerDSPVector
// with the nullptr constructor argument above, RtAudioProcessor
void UtuViewProcessor::processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused)
{
  // TEST
  int sr = _processData.sampleRate;
  testCounter += kFloatsPerDSPVector;
  bool test{false};
  if(testCounter >= sr)
  {
    test = true;
    testCounter -= sr;
  }
  if(test)
  {
    std::cout << "playbackState: " << playbackState << "\n";
    std::cout << "playbackSampleIdx: " << playbackSampleIdx << "\n";
  }
  
  // get params from the SignalProcessor.
  float gain = getParam("master_volume");
  float amp = dBToAmp(gain);
    
 // std::cout << "gain: " << gain << "\n";
  DSPVector sampleVec;
  
  if(playbackState == "on")
  {
    if(_playbackSample.data.size() > 0)
    {
      load(sampleVec, &(_playbackSample.data[playbackSampleIdx]));
      playbackSampleIdx += kFloatsPerDSPVector;
    }
    
    if(playbackSampleIdx >= _playbackSample.data.size())
    {
      playbackState = "off";
      playbackSampleIdx = 0;
      sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
    }
    
    float progress = float(playbackSampleIdx) / float(_playbackSample.data.size());
    sendMessageToActor(_controllerName, Message{"set_prop/playback_progress", progress});
  }
  
  
  
  // Running the sine generators makes DSPVectors as output.
  // The input parameter is omega: the frequency in Hz divided by the sample rate.
  // The output sines are multiplied by the gain.
  outputs[0] = outputs[1] = sampleVec*amp;
}


// toggle current playback state and tell controller
void UtuViewProcessor::togglePlaybackState()
{
  if(playbackState == "off")
  {
    if(_playbackSample.data.size() > 0)
    {
      playbackState = "on";
      playbackSampleIdx = 0;
      sendMessageToActor(_controllerName, Message{"do/playback_started"});
    }
  }
  else
  {
    playbackState = "off";
    playbackSampleIdx = 0;
    sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
    sendMessageToActor(_controllerName, Message{"set_prop/playback_progress", 0});
  }
}


void UtuViewProcessor::onMessage(Message msg)
{
  std::cout << "UtuViewProcessor: " << msg.address << " -> " << msg.value << "\n";
  
  switch(hash(head(msg.address)))
  {
    case(hash("set_param")):
    {
      setParam(tail(msg.address), msg.value.getFloatValue());
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
        case(hash("set_audio_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          _pSourceSample = *reinterpret_cast<sumu::Sample**>(msg.value.getBlobValue());
          
          int currentSampleRate = _processData.sampleRate;
          std::cout << "UtuViewProcessor: sr = " << currentSampleRate << "\n";
          std::cout << "    sample input: sr = " << _pSourceSample->sampleRate << "\n";

          // resample to current system sample rate for playback
          _playbackSample.sampleRate = currentSampleRate;
          normalize(_pSourceSample);
          resample(_pSourceSample, &_playbackSample);
          break;
        }
    
        case(hash("stop_play")):
        {
          if(playbackState != "off")
          {
            togglePlaybackState();
          }
          break;
        }
          
        case(hash("toggle_play")):
        {
          togglePlaybackState();
          break;
        }
      }
      break;
    }
    default:
    {
      std::cout << " UtuViewProcessor: uncaught message " << msg << "! \n";
      break;
    }
  }
}
