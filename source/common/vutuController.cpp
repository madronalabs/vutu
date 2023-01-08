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

void _lorisToSumuPartials(const Loris::PartialList* pLoris, SumuPartialsData* pSumu)
{
  pSumu->partials.clear();
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
  
  pSumu->type = Symbol(kSumuPartialsFileType);
  pSumu->version = kSumuPartialsFileVersion;
}

void _sumuToLorisPartials(const SumuPartialsData* pSumu, Loris::PartialList* pLoris)
{
  pLoris->clear();
    
  for (auto it = pSumu->partials.begin(); it != pSumu->partials.end(); it++) {
    const SumuPartial& sp = *it;
    Loris::Partial lp;

    size_t nBreakpoints = sp.time.size();
    for(int i=0; i<nBreakpoints; ++i)
    {
      Loris::Breakpoint b(sp.freq[i], sp.amp[i], sp.bandwidth[i], sp.phase[i]);
      lp.insert(sp.time[i], b);
    }
    pLoris->push_back(lp);
  }
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

void VutuController::setButtonEnableStates()
{
  bool sourceOK = (_sourceSample.data.size() > 0);
  sendMessageToActor(_viewName, {"widget/play_source/set_prop/enabled", sourceOK});
  sendMessageToActor(_viewName, {"widget/analyze/set_prop/enabled", sourceOK});
  
  Loris::PartialList* pLorisPartials = _lorisPartials.get();
  bool partialsOK = pLorisPartials && (pLorisPartials->size() > 0);
  sendMessageToActor(_viewName, {"widget/synthesize/set_prop/enabled", partialsOK});
  sendMessageToActor(_viewName, {"widget/export/set_prop/enabled", partialsOK});
  
  sendMessageToActor(_viewName, {"widget/play_synth/set_prop/enabled", (_synthesizedSample.data.size() > 0)});
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


void VutuController::clearSourceSample()
{
  _sourceSample.clear();
}


void VutuController::broadcastSourceSample()
{
  // send synthesized audio to View and Processor
  sumu::Sample* pSample = &_sourceSample;
  Value samplePtrValue(&pSample, sizeof(sumu::Sample*));
  sendMessageToActor(_processorName, {"do/set_source_data", samplePtrValue});
  sendMessageToActor(_viewName, {"do/set_source_data", samplePtrValue});
}

void VutuController::_clearPartialsData()
{
  // clear data
  _sumuPartials = std::make_unique< SumuPartialsData >();
  _lorisPartials = std::make_unique< Loris::PartialList >();
}

void VutuController::broadcastPartialsData()
{
  // send Partials to View and Processor
  SumuPartialsData* pPartials = _sumuPartials.get();
  Value partialsPtrValue(&pPartials, sizeof(SumuPartialsData*));
  sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
  sendMessageToActor(_viewName, {"do/set_partials_data", partialsPtrValue});
}

void VutuController::_clearSynthesizedSample()
{
  _synthesizedSample.clear();
}

void VutuController::broadcastSynthesizedSample()
{
  // send synthesized audio to View and Processor
  sumu::Sample* pSample = &_synthesizedSample;
  Value samplePtrValue(&pSample, sizeof(sumu::Sample*));
  sendMessageToActor(_processorName, {"do/set_synth_data", samplePtrValue});
  sendMessageToActor(_viewName, {"do/set_synth_data", samplePtrValue});
}

void VutuController::syncIntervals()
{
  sendMessageToActor(_processorName, {"do/set_interval_start", analysisInterval.mX1});
  sendMessageToActor(_processorName, {"do/set_interval_end", analysisInterval.mX2});
  sendMessageToActor(_viewName, {"do/set_interval_start", analysisInterval.mX1});
  sendMessageToActor(_viewName, {"do/set_interval_end", analysisInterval.mX2});
  
  sendMessageToActor(_viewName, {"do/set_source_duration", sourceDuration});
}

int VutuController::loadSampleFromPath(Path samplePath)
{
  int OK{ false };
  File fileToLoad(samplePath);
  if(fileToLoad)
  {
    std::cout << "file to load: " << samplePath << "\n";
    std::cout << "init sr: " << _sourceSample.sampleRate << "\n";
    
    // load the file
    auto filePathText = fileToLoad.getFullPathAsText();
    SF_INFO fileInfo{};
    sf_count_t framesRead{0};
    float* pData{nullptr};
    auto file = sf_open(filePathText.getText(), SFM_READ, &fileInfo);
    if(file)
    {
      constexpr size_t kMaxSeconds = 8;
      size_t fileSizeInFrames = fileInfo.frames;
      size_t kMaxFrames = kMaxSeconds*fileInfo.samplerate;
      
      size_t framesToRead = std::min(fileSizeInFrames, kMaxFrames);
      size_t samplesToRead = framesToRead*fileInfo.channels;
      
      _printToConsole(TextFragment("loading ", filePathText, "..."));
      
      _sourceSample.data.resize(samplesToRead);
      pData = _sourceSample.data.data();
      _sourceSample.sampleRate = fileInfo.samplerate;
      
      framesRead = sf_readf_float(file, pData, static_cast<sf_count_t>(framesToRead));
      
      TextFragment readStatus;
      if(framesRead != framesToRead)
      {
        readStatus = "file read failed!";
      }
      else
      {
        TextFragment truncatedMsg = (framesToRead == kMaxFrames) ? "(truncated)" : "";
        TextFragment sampleRate(" sr = ", textUtils::naturalNumberToText(_sourceSample.sampleRate));
        readStatus = (TextFragment(textUtils::naturalNumberToText(framesRead), " frames read ", truncatedMsg, sampleRate ));
        OK = true;
      }
      
      _printToConsole(readStatus);
      sf_close(file);
    }
    
    sourceFileLoaded = fileToLoad;

    // deinterleave to extract first channel if needed
    if(pData && fileInfo.channels > 1)
    {
      for(int i=0; i < framesRead; ++i)
      {
        pData[i] = pData[i*fileInfo.channels];
      }
      _sourceSample.data.resize(framesRead);
    }
    
    _sourceSample.normalize();
    
    // set source duration and reset analysis interval to whole source length
    sourceDuration = framesRead/float(_sourceSample.sampleRate);
    analysisInterval = {0.f, sourceDuration};
  }
  return OK;
}

TextFragment intToText(int i) { return textUtils::naturalNumberToText(i); }
TextFragment floatToText(float f) { return textUtils::floatNumberToText(f); }

void VutuController::showAnalysisInfo()
{
  SumuPartialsData* p = _sumuPartials.get();
  
  //TextFragment a(analysisInterval);
  
  //TextFragment a("analyzed ", p->stats.nPartials, ");
                   
  Path pathLoaded = sourceFileLoaded.getFullPath();
  Path shortName = last(pathLoaded);
  
  TextFragment a(pathToText(shortName));
  TextFragment b(" [", floatToText(analysisInterval.mX1), " -- " ,floatToText(analysisInterval.mX2), "] " );
  TextFragment c("partials: ", intToText(p->stats.nPartials));
  TextFragment d(" max freq: ", intToText(p->stats.freqRange.mX2));
  TextFragment e(" max active: ", intToText(p->stats.maxActivePartials));

  TextFragment out(a, b, c, d, e);
  _printToConsole(out);
}

int VutuController::loadPartialsFromPath(Path partialsPath)
{
  int OK{ false };
  File fileToLoad(partialsPath);
  if(fileToLoad)
  {
    TextFragment partialsText;
    fileToLoad.loadAsText(partialsText);
    
    // to implement!
    auto partialsJSON = textToJSON(partialsText);
    if(SumuPartialsData* newPartials = jsonToSumuPartials(partialsJSON))
    {
      // transfer ownership of new partials to _sumuPartials and delete previous
      _sumuPartials = std::unique_ptr<SumuPartialsData>(newPartials);
      OK = true;
    }
    
    _sumuPartials->calcStats();
    showAnalysisInfo();
     
    //std::cout << "text: " << partialsText << "\n";
  }
  
  return OK;
}


Path VutuController::showLoadDialog(Symbol fileType)
{
  Path returnVal{};
  nfdchar_t *outPath;
  std::string sourcePath;
  
  nfdresult_t result;
  File defaultLocation;
  switch(hash(fileType))
  {
    case(hash("samples")):
    {
      if(recentSamplesPath)
      {
        defaultLocation = File(recentSamplesPath);
      }
      else
      {
        defaultLocation = File(getApplicationDataRoot(getMakerName(), getAppName(), "samples"));
      }
      auto defaultPathText = defaultLocation.getFullPathAsText();

      nfdfilteritem_t filterItem[2] = { { "WAV audio", "wav" }, { "AIFF audio", "aiff,aif,aifc" } };
      result = NFD_OpenDialog(&outPath, filterItem, 2, defaultPathText.getText());
      break;
    }
    case(hash("partials")):
    {
      if(recentPartialsPath)
      {
        defaultLocation = File(recentPartialsPath);
      }
      else
      {
        defaultLocation = File(getApplicationDataRoot(getMakerName(), getAppName(), "partials"));
      }
      auto defaultPathText = defaultLocation.getFullPathAsText();
      nfdfilteritem_t filterItem[1] = { { "JSON data", "json" } }; // can add compressed JSON here
      result = NFD_OpenDialog(&outPath, filterItem, 1, defaultPathText.getText());
      break;
    }
  }
  
  if (result == NFD_OKAY)
  {
    returnVal = Path(outPath);
    
    switch(hash(fileType))
    {
      case(hash("samples")):
      {
        recentSamplesPath = returnVal;
        break;
      }
      case(hash("partials")):
      {
        recentPartialsPath = returnVal;
        break;
      }
    }
      
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
  
  return returnVal;
}

Path VutuController::showSaveDialog()
{
  Path returnVal{};
  File partialsRoot(getApplicationDataRoot(getMakerName(), getAppName(), "partials"));
  
  std::cout << "partials dir: " << partialsRoot.getFullPathAsText() << "\n";
  
  if(!partialsRoot.exists())
  {
    // make directory
    Symbol r = partialsRoot.createDirectory();
    if(r != "OK")
    {
      // TODO present error
      std::cout << "create directory failed: " << r << "\n";
    }
  }
  
  if(partialsRoot.exists())
  {
    auto partialsRootText = partialsRoot.getFullPathAsText();
    
    nfdchar_t* savePathAsString;
    Path savePath;
    
    Path currentPath = "default";//textToPath(_params["current_patch"].getTextValue());
    Symbol currentName = last(currentPath);
    TextFragment defaultName (currentName.getTextFragment(), ".json");
    
    // prepare filters for the dialog
    const int nFilters = 1;
    nfdfilteritem_t filterItem[nFilters] = {{"Sumu partials", "json"}};
    
    // show the dialog
    nfdresult_t result = NFD_SaveDialog(&savePathAsString, filterItem, nFilters, partialsRootText.getText(), defaultName.getText());
    if (result == NFD_OKAY)
    {
      puts(savePathAsString);
      savePath = Path(savePathAsString);
      NFD_FreePath(savePathAsString);
      returnVal = savePath;
    }
    else if (result == NFD_CANCEL)
    {
      puts("User pressed cancel.");
    }
    else
    {
      printf("Error: %s\n", NFD_GetError());
    }
  }
  return returnVal;
}

void VutuController::saveTextToPath(const TextFragment& text, Path savePath)
{
  if(!savePath) return;
  
  File saveFile (savePath);
  
  if(saveFile.hasWriteAccess())
  {
    if(saveFile.replaceWithText(text))
    {
      std::cout << "saved text to " << savePath << "\n";
    }
    else
    {
      // TODO other save errors
    }
  }
  else
  {
    std::cout << "save to file: no write access!\n";
    // TODO
  }
  
}

int VutuController::analyzeSample()
{
  int status{ false };
  
  auto totalFrames = _sourceSample.data.size();
  if(!totalFrames) return status;
  
  // make double-precision version of input
  std::vector< double > vx;
  vx.resize(totalFrames);
  for(int i=0; i<totalFrames; ++i)
  {
    vx[i] = _sourceSample.data[i];
  }
  
  int sr = _sourceSample.sampleRate;
  
  auto res = getPlainValue(params, "resolution");
  auto width = getPlainValue(params, "window_width");
  auto drift = getPlainValue(params, "freq_drift");
  auto floor = getPlainValue(params, "amp_floor");
  auto loCut = getPlainValue(params, "lo_cut");
  auto hiCut = getPlainValue(params, "hi_cut");

  analyzer_configure(res, width);
  analyzer_setFreqDrift(drift);
  analyzer_setAmpFloor(floor);
  analyzer_setFreqFloor(loCut);
    
  // make new partial list and give ownership to _lorisPartials
  Loris::PartialList* newPartials = createPartialList();
  _lorisPartials = std::make_unique< Loris::PartialList >(*newPartials);
  
  analyze( vx.data(), totalFrames, sr, _lorisPartials.get() );
  
  if(partialList_size(_lorisPartials.get()) > 0)
  {
    status = true;
    
    // convert loris partials to Sumu format and calculate stats
    _sumuPartials = std::make_unique< SumuPartialsData >();
    _lorisToSumuPartials(_lorisPartials.get(), _sumuPartials.get());
    _sumuPartials->cutHighs(hiCut);
    _sumuPartials->cleanOutliers();
    _sumuPartials->calcStats();
    showAnalysisInfo();
    
    // store analysis params used
    _sumuPartials->resolution = res;
    _sumuPartials->windowWidth = width;
    _sumuPartials->ampFloor = floor;
    _sumuPartials->freqDrift = drift;
    _sumuPartials->loCut = loCut;
    _sumuPartials->hiCut = hiCut;
    
    // convert back to loris partials after cutHighs (hack-ish)
    _sumuToLorisPartials(_sumuPartials.get(), _lorisPartials.get());
    
    // add info
    
    Path pathLoaded = sourceFileLoaded.getFullPath();
    Path shortName = last(pathLoaded);
    _sumuPartials->sourceFile = pathToText(shortName);
  }
  return status;
}

// generate the synthesized audio from the Loris partials.
// note output sample may be a different sample rate!
void VutuController::synthesize()
{
  std::vector<double> samples;
  Loris::Synthesizer::Parameters params;
  params.sampleRate = kSampleRate;
  
  std::cout << "VutuController: synthesize: sr = " << params.sampleRate << "\n";
  
  if(!_lorisPartials.get()) return;
  
  Loris::Synthesizer synth(params, samples);
  synth.setFadeTime(0.001f);
  synth.synthesize(_lorisPartials->begin(), _lorisPartials->end());
  
  // convert samples to floats
  std::cout << "VutuController: synthesize: " << samples.size() << "samples synthesized. \n";
  if(!samples.size()) return;
  
  // if there is a source, make buffer big enough to hold entire source at new sample rate
  size_t synthSamples;
  if(_sourceSample.data.size() > 0)
  {
    size_t sourceSamples = _sourceSample.data.size();
    size_t sourceSr = _sourceSample.sampleRate;
    size_t synthSr = params.sampleRate;
    synthSamples = sourceSamples*double(synthSr)/double(sourceSr);
  }
  else
  {
    synthSamples = samples.size();
  }
  
  _synthesizedSample.data.resize(synthSamples);
  for(int i=0; i<samples.size(); ++i)
  {
    _synthesizedSample.data[i] = samples[i];
  }
  for(int i=samples.size(); i < _synthesizedSample.data.size(); ++i)
  {
    _synthesizedSample.data[i] = 0.f;
  }

  _synthesizedSample.normalize();
  _synthesizedSample.sampleRate = params.sampleRate;
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
        case(hash("source_time")):
        {
          sendMessageToActor(_viewName, {"widget/source/set_prop/playback_time", m.value});
          break;
        }
        case(hash("synth_time")):
        {
          sendMessageToActor(_viewName, {"widget/synth/set_prop/playback_time", m.value});
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
          if(auto pathToLoad = showLoadDialog("samples"))
          {
            if(loadSampleFromPath(pathToLoad))
            {
              _clearPartialsData();
              _clearSynthesizedSample();
            }
          }
          broadcastSourceSample();
          broadcastPartialsData();
          broadcastSynthesizedSample();
          setButtonEnableStates();
          syncIntervals();
          
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
          _clearPartialsData();
          _clearSynthesizedSample();
          if(_sourceSample.data.size() > 0)
          {
            analyzeSample();
          }
          broadcastPartialsData();
          broadcastSynthesizedSample();
          setButtonEnableStates();
          messageHandled = true;
          break;
        }
        case(hash("synthesize")):
        {
          _clearSynthesizedSample();
          Loris::PartialList* pLorisPartials = _lorisPartials.get();
          if(pLorisPartials)
          if(pLorisPartials->size() > 0)
          {
            synthesize();
          }

          broadcastSynthesizedSample();
          setButtonEnableStates();
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
          sendMessageToActor(_viewName, {"widget/sample/set_prop/playback_time", 0.f});
          messageHandled = true;
          break;
        }
        case(hash("export")):
        {
          SumuPartialsData* pPartials = _sumuPartials.get();
          bool partialsOK = pPartials && (pPartials->partials.size() > 0);
          if(partialsOK)
          {
            if(auto savePath = showSaveDialog())
            {
              auto partialsJson = sumuPartialsToJSON(*pPartials);
              auto partialsText = JSONToText(partialsJson);
              File saveFile (savePath);
              saveTextToPath(partialsText, savePath);
            }
          }
          messageHandled = true;
          break;
        }
        case(hash("import")):
        {
          if(auto pathToLoad = showLoadDialog("partials"))
          {
            // load Sumu partials from JSON
            if(loadPartialsFromPath(pathToLoad))
            {
              // convert to Loris partials so we can use Loris to synthesize output
              _lorisPartials = std::make_unique< Loris::PartialList >();
              _sumuToLorisPartials(_sumuPartials.get(), _lorisPartials.get() );
              
              // clear source sample so all data is consistent
              clearSourceSample();
              broadcastSourceSample();
              
              // clear synthesized sample and sync UI
              _clearSynthesizedSample();
              broadcastPartialsData();
              broadcastSynthesizedSample();
              setButtonEnableStates();
              
              // set interval to whole partials file and broadcast
              analysisInterval = {_sumuPartials->stats.timeRange.mX1, _sumuPartials->stats.timeRange.mX2};
              syncIntervals();
            }
          }
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

