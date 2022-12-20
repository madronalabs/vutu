// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "vutuController.h"
#include "vutuView.h"
#include "vutu.h"

// param definitions for this plugin
#include "vutuParameters.h"

#include <cmath>
#include <iostream>
#include <chrono>

#include "MLSerialization.h"
#include "sumuPartials.h"

#include "mlvg.h"
#include "miniz.h"
#include "nfd.h"

// Loris includes
#include "loris.h"
#include "PartialList.h"
#include "Synthesizer.h"

using namespace ml;
using namespace sumu;


void _lorisToSumuPartials(const Loris::PartialList* pLoris, SumuPartialsData* pSumu, float maxTimeInSeconds)
{
  for (const auto& partial : *pLoris) {

    SumuPartial sp;
    for (auto it = partial.begin(); it != partial.end(); it++) {
      sp.time.push_back(it.time());
      sp.freq.push_back(it->frequency());
      sp.amp.push_back(it->amplitude());
      sp.bandwidth.push_back(it->bandwidth());
      sp.phase.push_back(it->phase());
    }
    
    pSumu->partials.push_back(sp);
    
  }
  
  std::cout << "_lorisToSumuPartials: " << pSumu->partials.size() << " partials. \n ";
  
  pSumu->calcStats(maxTimeInSeconds);// = getPartialsStats(*_partialsData);

}


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

void VutuController::_clearPartialsData()
{
  // clear data
  _sumuPartials = std::make_unique< SumuPartialsData >();
  _lorisPartials = std::make_unique< Loris::PartialList >();

  // send empty Partials to View and Processor
  SumuPartialsData* pPartials = _sumuPartials.get();
  Value partialsPtrValue(&pPartials, sizeof(SumuPartialsData*));
  sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
  sendMessageToActor(_viewName, {"do/set_partials_data", partialsPtrValue});
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
    
    constexpr size_t kMaxSeconds = 8;
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

      _clearPartialsData();
    }
  }
  return OK;
}

int VutuController::analyzeSample()
{
  int status{ false };
  std::cout << "VutuController::analyzing...";
  
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
  analyzer_setFreqDrift(drift);
  analyzer_setAmpFloor(floor);
    
  // make new partial list and give ownership to _lorisPartials
  Loris::PartialList* newPartials = createPartialList();
  _lorisPartials = std::make_unique< Loris::PartialList >(*newPartials);
  
  analyze( vx.data(), totalFrames, sr, _lorisPartials.get() );
  
  float maxTimeInSeconds = totalFrames / float(sr);
  
  // loris channelize and distill
  LinearEnvelope * reference = 0;
  float minFreq = res;
  float maxFreq = res*1.5f;
  reference = createFreqReference( _lorisPartials.get(), minFreq, maxFreq, 0 );
  channelize( _lorisPartials.get(), reference, 1 );
  distill( _lorisPartials.get() );
  destroyLinearEnvelope( reference );
  reference = nullptr;
  
  if(partialList_size(_lorisPartials.get()) > 0)
  {
    status = true;
    _sumuPartials = std::make_unique< SumuPartialsData >();
    _lorisToSumuPartials(_lorisPartials.get(), _sumuPartials.get(), maxTimeInSeconds);
  }

  return status;
}

// generate the synthesized audio from the Loris partials.
void VutuController::synthesize()
{
  std::vector<double> samples;
  Loris::Synthesizer::Parameters params;
  params.sampleRate = kSampleRate;
  
  std::cout << "VutuController: synthesize: sr = " << params.sampleRate << "\n";
  
//  playbackState = "off";
//  sendMessageToActor(_controllerName, Message{"do/playback_stopped"});
  
  if(!_lorisPartials.get()) return;
  
  Loris::Synthesizer synth(params, samples);
  synth.synthesize(_lorisPartials->begin(), _lorisPartials->end());
  
  // convert samples to floats
  std::cout << "VutuController: synthesize: " << samples.size() << "samples synthesized. \n";
  
  if(!samples.size()) return;
  
  _synthesizedSample.data.resize(samples.size());
  for(int i=0; i<samples.size(); ++i)
  {
    _synthesizedSample.data[i] = samples[i];
  }
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
        case(hash("source_progress")):
        {
          sendMessageToActor(_viewName, {"widget/source/set_prop/progress", m.value});
          break;
        }
        case(hash("synth_progress")):
        {
          sendMessageToActor(_viewName, {"widget/synth/set_prop/progress", m.value});
          break;
        }
      }
      break;
    }
    case(hash("do")):
    {
      switch(hash(second(addr)))
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
            
            sendMessageToActor(_viewName, {"widget/play_source/set_prop/enabled", true});

            
            // on success, send empty Partials to View and Processor
            _clearPartialsData();
            
            // SumuPartialsData* pPartials = _sumuPartials.get();
            // Value partialsPtrValue(&pPartials, sizeof(SumuPartialsData*));
            // sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
            // sendMessageToActor(_viewName, {"do/set_partials_data", partialsPtrValue});
          }
          else
          {
            sendMessageToActor(_viewName, {"widget/play/set_prop/enabled", false});
          }
          messageHandled = true;
          break;
        }
        case(hash("toggle_play_source")):
        {
          sendMessageToActor(_processorName, {"do/toggle_play/source"});
          messageHandled = true;
          break;
        }
        case(hash("toggle_play_synth")):
        {
          sendMessageToActor(_processorName, {"do/toggle_play/synth"});
          messageHandled = true;
          break;
        }
        case(hash("analyze")):
        {
          if(_sample.data.size() > 0)
          {
            if(analyzeSample())
            {
              // on success, send Sumu Partials to View and Processor
              SumuPartialsData* pPartials = _sumuPartials.get();
              Value partialsPtrValue(&pPartials, sizeof(SumuPartialsData*));
              sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
              sendMessageToActor(_viewName, {"do/set_partials_data", partialsPtrValue});
              
              // on success, send Loris Partials to View and Processor
              Loris::PartialList* pLorisPartials = _lorisPartials.get();
              Value lorisPartialsPtrValue(&pLorisPartials, sizeof(Loris::PartialList*));
              sendMessageToActor(_processorName, {"do/set_loris_partials_data", lorisPartialsPtrValue});
              sendMessageToActor(_viewName, {"do/set_loris_partials_data", lorisPartialsPtrValue});
            }
          }
          
          messageHandled = true;
          break;
        }
        case(hash("synthesize")):
        {
          Loris::PartialList* pLorisPartials = _lorisPartials.get();
          if(pLorisPartials)
          if(pLorisPartials->size() > 0)
          {
            synthesize();
            
            // send synthesized audio to View and Processor
            sumu::Sample* pSample = &_synthesizedSample;
            Value samplePtrValue(&pSample, sizeof(sumu::Sample*));
            sendMessageToActor(_processorName, {"do/set_synth_data", samplePtrValue});
            sendMessageToActor(_viewName, {"do/set_synth_data", samplePtrValue});
            
            sendMessageToActor(_viewName, {"widget/play_synth/set_prop/enabled", true});
          }
          
          messageHandled = true;
          break;
        }
        case(hash("playback_started")):
        {
          // switch play_source button or play_synth button text
          switch(hash(third(addr)))
          {
            case(hash("source")):
            {
              sendMessageToActor(_viewName, {"widget/play_source/set_prop/text", TextFragment("stop")});
              break;
            }
            case(hash("synth")):
            {
              sendMessageToActor(_viewName, {"widget/play_synth/set_prop/text", TextFragment("stop")});
              break;
            }
          }
          messageHandled = true;
          break;
        }
        case(hash("playback_stopped")):
        {
          // switch play button texts
          sendMessageToActor(_viewName, {"widget/play_source/set_prop/text", TextFragment("play")});
          sendMessageToActor(_viewName, {"widget/play_synth/set_prop/text", TextFragment("play")});
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

