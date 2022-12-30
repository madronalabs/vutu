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
using namespace sumu;

constexpr float kSizeLo = 0, kSizeHi = 40;
constexpr float kToneLo = 250, kToneHi = 4000;
constexpr float kDecayLo = 0.8, kDecayHi = 20;
constexpr float kLevelLo = 0.5f, kLevelHi = 2.f;

void readParameterDescriptions(ParameterDescriptionList& params)
{
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "resolution" },
    { "range", { 8, 1024 } },
    { "plaindefault", 20 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "window_width" },
    { "range", {16, 768} },
    { "plaindefault", 256 },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "amp_floor" },
    { "range", {-80, -20} },
    { "plaindefault", -60 },
    { "units", "dB" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "freq_drift" },
    { "range", {2, 200} },
    { "plaindefault", 30 },
    { "log", false },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "master_volume" },
    { "range", {-60, 0} },
    { "log", false },
    { "plaindefault", -6 },
    { "units", "dB" }
  } ) );
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

VutuProcessor::VutuProcessor(TextFragment appName, size_t instanceNum,
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
void VutuProcessor::processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused)
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
    //std::cout << "playbackState: " << playbackState << "\n";
    //std::cout << "playbackSampleIdx: " << playbackSampleIdx << "\n";
    //std::cout << "analysis interval: " << analysisInterval << "\n";
  }
  
  // get params from the SignalProcessor.
  float gain = getParam("master_volume");
  float amp = dBToAmp(gain);
    
 // std::cout << "gain: " << gain << "\n";
  DSPVector sampleVec;
  
  sumu::Sample* samplePlaying{ nullptr };
  Symbol viewProperty;
  if(playbackState == "source")
  {
    samplePlaying = &_sourceSample;
    viewProperty = "source_time";
  }
  else if(playbackState == "synth")
  {
    if(_pSynthesizedSample)
    {
      samplePlaying = _pSynthesizedSample;
      viewProperty = "synth_time";
    }
  }

  if(samplePlaying)
  {
    if(samplePlaying->data.size() > 0)
    {
      load(sampleVec, &(samplePlaying->data[playbackSampleIdx]));
      playbackSampleIdx += kFloatsPerDSPVector;
    }
    
    if(playbackSampleIdx >= samplePlaying->data.size())
    {
      playbackState = "off";
      playbackSampleIdx = 0;
      sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
    }
    
    float playbackTime = float(playbackSampleIdx) / float(sr);

    sendMessageToActor(_controllerName, Message{Path{"set_prop", viewProperty}, playbackTime});
  }
  
  // Running the sine generators makes DSPVectors as output.
  // The input parameter is omega: the frequency in Hz divided by the sample rate.
  // The output sines are multiplied by the gain.
  outputs[0] = outputs[1] = sampleVec*amp;
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
      if(_sourceSample.data.size() > 0)
      {
        playbackState = "source";
        playbackSampleIdx = 0;
        sendMessageToActor(_controllerName, Message{"do/playback_started/source"});
      }
    }
  }
  else if(whichSample == "synth")
  {
    if(prevState != "synth")
    {
      if(_pSynthesizedSample && _pSynthesizedSample->data.size() > 0)
      {
        playbackState = "synth";
        playbackSampleIdx = 0;
        sendMessageToActor(_controllerName, Message{"do/playback_started/synth"});
      }
    }
  }
}

void VutuProcessor::onMessage(Message msg)
{
  //std::cout << "VutuProcessor: " << msg.address << " -> " << msg.value << "\n";
  
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
        case(hash("set_source_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          _pSourceSampleInController = *reinterpret_cast<sumu::Sample**>(msg.value.getBlobValue());
          
          int currentSampleRate = _processData.sampleRate;
          std::cout << "VutuProcessor: sr = " << currentSampleRate << "\n";
          std::cout << "    sample input: sr = " << _pSourceSampleInController->sampleRate << "\n";
          
          // resample to current system sample rate for playback
          _sourceSample.sampleRate = currentSampleRate;
          
          // TODO normalize there
          // normalize(_pSourceSampleInController);
          resample(_pSourceSampleInController, &_sourceSample);
          break;
        }
          
        case(hash("set_loris_partials_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          Loris::PartialList* pPartials = *reinterpret_cast<Loris::PartialList**>(msg.value.getBlobValue());
          _pLorisPartials = *reinterpret_cast<Loris::PartialList**>(msg.value.getBlobValue());
          

          std::cout << "VutuProcessor: got new loris partials: n = " << _pLorisPartials->size() << "\n";

          break;
        }

        case(hash("set_synth_data")):
        {
          playbackState = "off";
          sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
          
          // get pointer from message
          _pSynthesizedSample = *reinterpret_cast<sumu::Sample**>(msg.value.getBlobValue());

          break;
        }
          
        case(hash("toggle_play")):
        {
          // play either source or synth
          togglePlaybackState(third(msg.address));
          break;
        }
          
        case(hash("set_interval_start")):
        {
          analysisInterval.mX1 = msg.value.getFloatValue();
          break;
        }
          
        case(hash("set_interval_end")):
        {
          analysisInterval.mX2 = msg.value.getFloatValue();
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
