// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details


// use clipboard code from VST3 SDK
//#include "public.sdk/source/common/systemclipboard.h"

#include "vutuController.h"
#include "vutuView.h"
#include "vutu.h"

// param definitions for this plugin
#include "vutuParameters.h"

#include <cmath>
#include <iostream>
#include <chrono>

#include "MLSerialization.h"

#include "mlvg.h"

// includes in mlvg - TODO wrap
#include "miniz.h"
#include "nfd.h"

#include "loris.h"

using namespace ml;
using namespace sumu;


//-----------------------------------------------------------------------------
// VutuController implementation

VutuController::VutuController(TextFragment appName, const ParameterDescriptionList& pdl)
  : AppController(appName, pdl)
{
  _debugTimer.start([=]() { _debug(); }, milliseconds(1000));
}

VutuController::~VutuController()
{
  // don't stop the master Timers-- there may be other plugin instances using it!
  // std::cout << "VutuController: BYE!\n";
  if(_partials)
  {
    destroyPartialList(_partials);
  }
}

void VutuController::_debug()
{
//  std::cout << "VutuController: " << getMessagesAvailable() << " messages in queue. \n";
//  std::cout << "VutuController @ " << std::hex << (this) << std::dec << " : \n";
//  std::cout << "        timers @ " << std::hex << (&_timers.get()) << std::dec << "\n";
}

void VutuController::_printToConsole(TextFragment t)
{
  sendMessageToActor(_viewName, {"info/set_prop/text", t});
}

int VutuController::_loadSampleFromDialog()
{
  int OK{ false };
  nfdchar_t *outPath;
  std::string sourcePath;
  nfdfilteritem_t filterItem[2] = { { "WAV audio", "wav" }, { "AIFF audio", "aiff,aif,aifc" } };
  nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 2, NULL);
  if (result == NFD_OKAY)
  {
    puts("Success!");
    puts(outPath);
    
    sourcePath = outPath;
    NFD_FreePath(outPath);
  }
  else if (result == NFD_CANCEL)
  {
    puts("User pressed cancel.");
  }
  else
  {
    printf("Error: %s\n", NFD_GetError());
  }
  
  Path filePath(sourcePath.c_str());
  File f(filePath);
  if(f)
  {
    std::cout << "file to load: " << filePath << "\n";
    std::cout << "init sr: " << _sample.sampleRate << "\n";

    // load the file

    SF_INFO fileInfo;
    
    auto file = sf_open(sourcePath.c_str(), SFM_READ, &fileInfo);
    
    std::cout << "        format: " << fileInfo.format << "\n";
    std::cout << "        frames: " << fileInfo.frames << "\n";
    std::cout << "        samplerate: " << fileInfo.samplerate << "\n";
    std::cout << "        channels: " << fileInfo.channels << "\n";
    
    constexpr size_t kMaxSeconds = 32;
    size_t fileSizeInFrames = fileInfo.frames;
    size_t kMaxFrames = kMaxSeconds*fileInfo.samplerate;

    size_t framesToRead = std::min(fileSizeInFrames, kMaxFrames);
    size_t samplesToRead = framesToRead*fileInfo.channels;
    
    _printToConsole(TextFragment("loading ", pathToText(filePath), "..."));
    
    _sample.data.resize(samplesToRead);
    float* pData = _sample.data.data();
    _sample.sampleRate = fileInfo.samplerate;
    
    sf_count_t framesRead = sf_readf_float(file, pData, static_cast<sf_count_t>(framesToRead));
    
    TextFragment readStatus;
    if(framesRead != framesToRead)
    {
      readStatus = "file read failed!";
    }
    else
    {
      TextFragment truncatedMsg = (framesToRead == kMaxFrames) ? "(truncated)" : "";
      readStatus = (TextFragment(textUtils::naturalNumberToText(framesRead), " frames read ", truncatedMsg ));
      OK = true;
    }
    
    _printToConsole(readStatus);
    sf_close(file);
        
    // deinterleave to extract first channel if needed
    if(fileInfo.channels > 1)
    {
      for(int i=0; i < framesRead; ++i)
      {
        pData[i] = pData[i*fileInfo.channels];
      }
      _sample.data.resize(framesRead);
    }

  }
  return OK;
}

int VutuController::analyzeSample()
{
  int status{ false };
  std::cout << "VutuController::analysing...";
  
  auto totalFrames = _sample.data.size();
  if(!totalFrames) return status;
  
  // make double-precision version of input
  std::vector< double > vx;
  vx.resize(totalFrames);
  for(int i=0; i<totalFrames; ++i)
  {
    vx[i] = _sample.data[i];
  }
  
  int sr = _sample.sampleRate;
  
  // TEST
  auto res = getPlainValue(params, "resolution");
  auto width = getPlainValue(params, "window_width");
  auto drift = getPlainValue(params, "freq_drift");
  auto floor = getPlainValue(params, "amp_floor");

  analyzer_configure(res, width);
  analyzer_setFreqDrift( drift );
  analyzer_setAmpFloor( floor );
  
  // loris analyze
  if(_partials)
  {
    destroyPartialList(_partials);
  }
  _partials = createPartialList();
  
  analyze( vx.data(), totalFrames, sr, _partials );
  
  // loris channelize and distill
  LinearEnvelope * reference = 0;
  float minFreq = res;
  float maxFreq = res*1.5f;
  reference = createFreqReference( _partials, minFreq, maxFreq, 0 );
  channelize( _partials, reference, 1 );
  distill( _partials );
  destroyLinearEnvelope( reference );
  reference = nullptr;
  
  if(partialList_size(_partials) > 0)
  {
    status = true;
  }

  return status;
}

void VutuController::onMessage(Message m)
{
  if(!m.address) return;
  
 // std::cout << "VutuController::onMessage:" << m.address << " " << m.value << " \n ";
  
  bool messageHandled{false};
  
  Path addr = m.address;
  switch(hash(head(addr)))
  {
    case(hash("set_param")):
    {
      Path whatParam = tail(addr);
      switch(hash(head(whatParam)))
      {
      }
      break;
    }
    case(hash("set_prop")):
    {
      Path whatProp = tail(addr);
      switch(hash(head(whatProp)))
      {
        case(hash("playback_progress")):
        {
          sendMessageToActor(_viewName, {"widget/sample/set_prop/progress", m.value});
          break;
        }
      }
      break;
    }
    case(hash("do")):
    {
      Path whatAction = tail(addr);
      switch(hash(head(whatAction)))
      {
        case(hash("open")):
        {
          if(_loadSampleFromDialog())
          {
            // on success, send to View and Processor
            sumu::Sample* pSample = &_sample;
            Value samplePtrValue(&pSample, sizeof(sumu::Sample*));
            sendMessageToActor(_processorName, {"do/set_audio_data", samplePtrValue});
            sendMessageToActor(_viewName, {"do/set_audio_data", samplePtrValue});
            
            sendMessageToActor(_viewName, {"widget/play/set_prop/enabled", true});
            //sendMessageToActor(_processorName, {"do/stop_play"});
          }
          else
          {
            sendMessageToActor(_viewName, {"widget/play/set_prop/enabled", false});
          }
          messageHandled = true;
          break;
        }
        case(hash("toggle_play")):
        {
          sendMessageToActor(_processorName, {"do/toggle_play"});
          messageHandled = true;
          break;
        }
        case(hash("analyze")):
        {
          if(_sample.data.size() > 0)
          {
            if(analyzeSample())
            {
              
              // on success, send to View and Processor
              sumu::Sample* pSample = &_sample;
              Value samplePtrValue(&pSample, sizeof(sumu::Sample*));
              sendMessageToActor(_processorName, {"do/set_partials_data", samplePtrValue});
              sendMessageToActor(_viewName, {"do/set_partials_data", samplePtrValue});
            }
          }

          messageHandled = true;
          break;
        }
        case(hash("playback_started")):
        {
          // switch play button text
          sendMessageToActor(_viewName, {"widget/play/set_prop/text", TextFragment("stop")});
          messageHandled = true;
          break;
        }
        case(hash("playback_stopped")):
        {
          // switch play button text
          sendMessageToActor(_viewName, {"widget/play/set_prop/text", TextFragment("play")});
          sendMessageToActor(_viewName, {"widget/sample/set_prop/progress", 0.f});
          messageHandled = true;
          break;
        }
        default:
        {
          break;
        }
        
      }
      break;
    }
    default:
    {
      break;
    }
  }
  
  if(!messageHandled)
  {
    AppController::onMessage(m);
  }
}

