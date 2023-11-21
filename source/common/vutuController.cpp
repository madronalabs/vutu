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
#include "vutuPartials.h"

#include "mlvg.h"
#include "miniz.h"
#include "nfd.h"

// Loris includes
#include "loris.h"
#include "PartialList.h"
#include "Synthesizer.h"

using namespace ml;

void _lorisToVutuPartials(const Loris::PartialList* pLoris, VutuPartialsData* pSumu)
{
  pSumu->partials.clear();
  for (const auto& partial : *pLoris) {
    VutuPartial sp;
    for (auto it = partial.begin(); it != partial.end(); it++) {
      sp.time.push_back(it.time());
      sp.freq.push_back(it->frequency());
      sp.amp.push_back(it->amplitude());
      sp.bandwidth.push_back(it->bandwidth());
      sp.phase.push_back(it->phase());
    }
    pSumu->partials.push_back(sp);
  }
  
  pSumu->type = Symbol(kVutuPartialsFileType);
  pSumu->version = kVutuPartialsFileVersion;
}

void _sumuToLorisPartials(const VutuPartialsData* pSumu, Loris::PartialList* pLoris)
{
  pLoris->clear();
    
  for (auto it = pSumu->partials.begin(); it != pSumu->partials.end(); it++) {
    const VutuPartial& sp = *it;
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
  sendMessageToActor(_viewName, {"widget/play_source/set_prop/enabled", usable(&_sourceSample)});
  sendMessageToActor(_viewName, {"widget/analyze/set_prop/enabled", usable(&_sourceSample)});
  
  Loris::PartialList* pLorisPartials = _lorisPartials.get();
  bool partialsOK = pLorisPartials && (pLorisPartials->size() > 0);
  sendMessageToActor(_viewName, {"widget/synthesize/set_prop/enabled", partialsOK});
  sendMessageToActor(_viewName, {"widget/export/set_prop/enabled", partialsOK});
  
  sendMessageToActor(_viewName, {"widget/play_synth/set_prop/enabled", getSize(_synthesizedSample) > 0});
  sendMessageToActor(_viewName, {"widget/export_synth/set_prop/enabled", getSize(_synthesizedSample) > 0});
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

void VutuController::broadcastSourceSample()
{
  // send synthesized audio to View and Processor
  ml::Sample* pSample = &_sourceSample;
  Value samplePtrValue(&pSample, sizeof(ml::Sample*));
  sendMessageToActor(_processorName, {"do/set_source_data", samplePtrValue});
  sendMessageToActor(_viewName, {"do/set_source_data", samplePtrValue});
}

void VutuController::_clearPartialsData()
{
  // clear data
  _vutuPartials = std::make_unique< VutuPartialsData >();
  _lorisPartials = std::make_unique< Loris::PartialList >();
}

void VutuController::broadcastPartialsData()
{
  // send Partials to View and Processor
  VutuPartialsData* pPartials = _vutuPartials.get();
  Value partialsPtrValue(&pPartials, sizeof(VutuPartialsData*));
  sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
  sendMessageToActor(_viewName, {"do/set_partials_data", partialsPtrValue});

}

void VutuController::_clearSynthesizedSample()
{
  clear(_synthesizedSample);
}

void VutuController::broadcastSynthesizedSample()
{
  // send synthesized audio to View and Processor
  ml::Sample* pSample = &_synthesizedSample;
  Value samplePtrValue(&pSample, sizeof(ml::Sample*));
  sendMessageToActor(_processorName, {"do/set_synth_data", samplePtrValue});
  sendMessageToActor(_viewName, {"do/set_synth_data", samplePtrValue});
}


int VutuController::saveSampleToWavFile(const Sample& sample, Path wavPath)
{
  int OK{ false };
  std::cout << "file to save: " << wavPath << "\n";
  
  SNDFILE* sndfile;
  SF_INFO* sf_info;

  
  sf_info = (SF_INFO *) malloc(sizeof(SF_INFO));
  sf_info->samplerate = sample.sampleRate;
  sf_info->channels = 1;
  sf_info->format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  
  sndfile = sf_open(pathToText(wavPath).getText(), SFM_WRITE, sf_info);

  // write samples
  // mono only, for now!
  size_t frames = getFrames(_synthesizedSample);
  auto writeResult = sf_writef_float(sndfile, getFramePtr(_synthesizedSample), frames);
  if(writeResult == frames)
  {
    OK = true;
  }
  
  /* Close sound file and return */
  sf_close(sndfile);

  
  return OK;
}

int VutuController::loadSampleFromPath(Path samplePath)
{
  int OK{ false };
  File fileToLoad(samplePath);
  if(fileToLoad)
  {
    std::cout << "file to load: " << samplePath << "\n";
    // load the file
    auto filePathText = fileToLoad.getFullPathAsText();
    SF_INFO fileInfo{};
    sf_count_t framesRead{0};
    float* pData{nullptr};
    auto file = sf_open(filePathText.getText(), SFM_READ, &fileInfo);
    if(file)
    {
      constexpr size_t kMaxSeconds = 60;
      size_t kMaxFrames = kMaxSeconds*fileInfo.samplerate;
      size_t framesToRead = std::min(size_t(fileInfo.frames), kMaxFrames);
      _printToConsole(TextFragment("loading ", filePathText, "..."));
      
      auto pData = resize(_sourceSample, framesToRead, fileInfo.channels);
      
      //_sourceSample.data.resize(samplesToRead);
      //pData = _sourceSample.data.data();
      
      _sourceSample.sampleRate = fileInfo.samplerate;
      
      std::cout << "  file sr: " << _sourceSample.sampleRate << "\n";
      
      if(pData)
      {
        framesRead = sf_readf_float(file, pData, static_cast<sf_count_t>(framesToRead));
      }
      
      TextFragment readStatus;
      if(framesRead != framesToRead)
      {
        readStatus = "file read failed!";
      }
      else
      {
        float sr = _sourceSample.sampleRate;
        TextFragment truncatedMsg = (framesToRead == kMaxFrames) ? "(truncated)" : "";
        TextFragment framesMsg (textUtils::naturalNumberToText(framesRead), " frames read ");
        TextFragment secondsMsg ("(", textUtils::floatNumberToText((framesRead + 0.f)/sr, 2), " seconds) ");
        TextFragment sampleRate(" sr = ", textUtils::naturalNumberToText(_sourceSample.sampleRate));
        TextFragment fileName = last(samplePath).getTextFragment();
        readStatus = TextFragment(fileName, ": ", framesMsg, secondsMsg, truncatedMsg, sampleRate );
        OK = true;
      }
      
      _printToConsole(readStatus);
      sf_close(file);
    }
    
    sourceFileLoaded = fileToLoad;

    // deinterleave in place to extract first channel if needed
    if(usable(&_sourceSample) && _sourceSample.channels > 1)
    {
      
      for(int i=0; i < framesRead; ++i)
      {
        _sourceSample[i] = _sourceSample[i*_sourceSample.channels];
      }
      resize(_sourceSample, framesRead, 1);
    }
    
    normalize(_sourceSample);

  }
  return OK;
}

TextFragment intToText(int i) { return textUtils::naturalNumberToText(i); }
TextFragment floatToText(float f) { return textUtils::floatNumberToText(f); }

void VutuController::showAnalysisInfo()
{
  VutuPartialsData* p = _vutuPartials.get();

  
  TextFragment a(p->sourceFile);
  TextFragment b(" [", floatToText(p->stats.timeRange.mX1), " -- " ,floatToText(p->stats.timeRange.mX2), "] " );
  TextFragment c("partials: ", intToText(p->stats.nPartials));
  TextFragment d(" max freq: ", intToText(p->stats.freqRange.mX2));
  TextFragment e(" max active: ", intToText(p->stats.maxActivePartials));

  TextFragment out(a, b, c, d, e);
  _printToConsole(out);
}

void VutuController::setAnalysisParamsFromPartials()
{
  VutuPartialsData* p = _vutuPartials.get();
  
  params.setFromRealValue("fundamental", p->fundamental);
  broadcastParam("fundamental", 0);

}

int VutuController::loadPartialsFromPath(Path partialsPath)
{
  int OK{ false };
  File fileToLoad(partialsPath);
  if(fileToLoad)
  {
    if(VutuPartialsData* newPartials = loadVutuPartialsFromFile(fileToLoad))
    {
      // transfer ownership of new partials to _vutuPartials and delete previous
      _vutuPartials = std::unique_ptr<VutuPartialsData>(newPartials);
      OK = true;
    }
    
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
      nfdfilteritem_t filterItem[1] = { { "JSON data", "utu" } };
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


std::vector<TextFragment> extensionsForFileType(Symbol fileType)
{
  std::vector<TextFragment> r;
  switch(hash(fileType))
  {
    case(hash("partials")):
      r.push_back("utu");
      break;
    case(hash("audio")):
      r.push_back("wav");
      break;
  }
  return r;
}


TextFragment getFileDescription(TextFragment extension)
{
  TextFragment desc;
  switch(hash(extension))
  {
    case(hash("utu")):
      desc = "Utu partials (JSON)";
      break;
    case(hash("wav")):
      desc = "WAV audio";
      break;
  }
  return desc;
}

Path VutuController::showSaveDialog(Symbol fileType)
{
  Path returnVal{};
  File fileTypeRoot(getApplicationDataRoot(getMakerName(), getAppName(), fileType));
  
  std::cout << "root dir: " << fileTypeRoot.getFullPathAsText() << "\n";
  
  if(!fileTypeRoot.exists())
  {
    // make directory
    Symbol r = fileTypeRoot.createDirectory();
    if(r != "OK")
    {
      // TODO present error
      std::cout << "create directory failed: " << r << "\n";
    }
  }
  
  if(fileTypeRoot.exists())
  {
    auto rootText = fileTypeRoot.getFullPathAsText();
    
    nfdchar_t* savePathAsString;
    Path savePath;
    
    Path currentPath = "default";//textToPath(_params["current_patch"].getTextValue());
    Symbol currentName = last(currentPath);
    auto ext = extensionsForFileType(fileType);
    TextFragment defaultName (currentName.getTextFragment());
    
    // prepare filters for the dialog
    const size_t kMaxFilters{4};
    const int nFilters = std::min(ext.size(), kMaxFilters);
    nfdfilteritem_t filterItems[kMaxFilters];
    
    for(int i=0; i<nFilters; ++i)
    {
      // set filter item description and extension
      auto extItem = ext[i].getText();
      filterItems[i] = {getFileDescription(extItem).getText(), extItem};
    }
    
    // show the dialog
    nfdresult_t result = NFD_SaveDialog(&savePathAsString, filterItems, nFilters, rootText.getText(), defaultName.getText());
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
  
  auto totalFrames = getFrames(_sourceSample);
  if(!totalFrames) return status;
  
  auto interval = params.getRealValue("analysis_interval").getIntervalValue();
  auto frameInterval = interval*float(totalFrames);
  
  int framesInInterval = frameInterval.mX2 - frameInterval.mX1;
  const float kFadeTime = 0.001f;
  int fadeSamples = kFadeTime*_sourceSample.sampleRate;
  
  // make double-precision version of input
  std::vector< double > vx;
  vx.resize(framesInInterval);
  int srcStart = frameInterval.mX1;
  for(int i=0; i<framesInInterval; ++i)
  {
    vx[i] = _sourceSample[srcStart + i];
  }
  
  // fade in
  for(int i=0; i<fadeSamples; ++i)
  {
    double gain = (double)i / (double)fadeSamples;
    vx[i] *= gain;
  }
  
  // fade out
  for(int i=0; i<fadeSamples; ++i)
  {
    int i2 = framesInInterval - 1 - i;
    double gain = (double)i / (double)fadeSamples;
    vx[i2] *= gain;
  }
  
  // set sample rate and configure analyzer
  int sr = _sourceSample.sampleRate;
  auto res = params.getRealFloatValue("resolution");
  auto width = params.getRealFloatValue("window_width");
  auto drift = params.getRealFloatValue("freq_drift");
  auto floor = params.getRealFloatValue("amp_floor");
  auto loCut = params.getRealFloatValue("lo_cut");
  auto hiCut = params.getRealFloatValue("hi_cut");
  auto noiseWidth = params.getRealFloatValue("noise_width");
  analyzer_configure(res, width);
  analyzer_setFreqDrift(drift);
  analyzer_setAmpFloor(floor);
  analyzer_setFreqFloor(loCut);
  analyzer_setBwRegionWidth(noiseWidth);
    
  // make new partial list and give ownership to _lorisPartials
  Loris::PartialList* newPartials = createPartialList();
  _lorisPartials = std::make_unique< Loris::PartialList >(*newPartials);
  
  
  //  if verbose, spew out the Analyzer state:
  if ( true )
  {
    std::cout << "* Loris Analyzer configuration:" << std::endl;
    std::cout << "*\tfrequency resolution: " << analyzer_getFreqResolution() << " Hz\n";
    std::cout << "*\tanalysis window width: " << analyzer_getWindowWidth() << " Hz\n";
    std::cout << "*\tanalysis window sidelobe attenuation: "
    << analyzer_getSidelobeLevel() << " dB\n";
    std::cout << "*\tspectral amplitude floor: " << analyzer_getAmpFloor() << " dB\n";
    std::cout << "*\tminimum partial frequecy: " << analyzer_getFreqFloor() << " Hz\n";
    std::cout << "*\thop time: " << 1000*analyzer_getHopTime() << " ms\n";
    std::cout << "*\tmaximum partial frequency drift: " << analyzer_getFreqDrift()
    << " Hz\n";
    std::cout << "*\tcrop time: " << 1000*analyzer_getCropTime() << " ms\n";
    
    if (1)//( gAnalyzer->associateBandwidth() )
    {
      if (1)// ( gAnalyzer->bandwidthIsResidue() )
      {
        std::cout << "*\tspectral residue bandwidth association region width: "
        << analyzer_getBwRegionWidth() << " Hz\n";
      }
      else
      {
        std::cout << "*\tsinusoidal convergence bandwidth tolerance: "
        << analyzer_getBwConvergenceTolerance() << "\n";
      }
    }
    else
    {
      std::cout << "*\tstoring no bandwidth\n";
    }

    
    std::cout << std::endl;
  }
  
  analyze( vx.data(), framesInInterval, sr, _lorisPartials.get() );
  
  if(partialList_size(_lorisPartials.get()) > 0)
  {
    status = true;
    
    // convert loris partials to Sumu format and calculate stats
    _vutuPartials = std::make_unique< VutuPartialsData >();
    _lorisToVutuPartials(_lorisPartials.get(), _vutuPartials.get());
    cutHighs(*_vutuPartials, hiCut);
    cleanOutliers(*_vutuPartials);
    calcStats(*_vutuPartials);
    showAnalysisInfo();
    
    // store analysis params used
    _vutuPartials->sourceFile = sourceFileLoaded.getShortName();
    _vutuPartials->sourceDuration = getDuration(_sourceSample);
    _vutuPartials->resolution = res;
    _vutuPartials->windowWidth = width;
    _vutuPartials->ampFloor = floor;
    _vutuPartials->freqDrift = drift;
    _vutuPartials->loCut = loCut;
    _vutuPartials->hiCut = hiCut;


    // convert back to loris partials after cutHighs (hack-ish)
    _sumuToLorisPartials(_vutuPartials.get(), _lorisPartials.get());
    
  }
  return status;
}

// generate the synthesized audio from the Loris partials.
// note output sample may be a different sample rate!
void VutuController::synthesize()
{
  if(!_lorisPartials.get()) return;

  std::vector<double> destSamples;
  Loris::Synthesizer::Parameters synthParams;
  synthParams.sampleRate = kSampleRate;
  const float kFadeTime = 0.001f;

  // get frames in analysis interval to use for output length. Length of synthesis will be shorter.
  Interval analysisInterval = params.getRealValue("analysis_interval").getIntervalValue();
  float duration = _vutuPartials->sourceDuration*(analysisInterval.mX2 -  analysisInterval.mX1);
  int framesAnalyzed = duration*synthParams.sampleRate;
  
  // run the Loris synthesizer
  Loris::Synthesizer synth(synthParams, destSamples);
  synth.setFadeTime(kFadeTime);
  synth.synthesize(_lorisPartials->begin(), _lorisPartials->end());
  
  std::cout << "VutuController: synthesize: " << destSamples.size() << "samples synthesized. " << framesAnalyzed << " frames analyzed. \n";

  // resize and zero-pad output to fill entire interval
  if(!destSamples.size()) return;
  destSamples.resize(framesAnalyzed);
  
  // convert to floats
  size_t outputFrames = destSamples.size();
  int fadeSamples = min(size_t(kFadeTime*kSampleRate), outputFrames/2);
  
  resize(_synthesizedSample, outputFrames, 1);
  
  for(int i=0; i<outputFrames; ++i)
  {
    _synthesizedSample[i] = destSamples[i];
  }

  // fade in
  for(int i=0; i<fadeSamples; ++i)
  {
    double gain = (double)i / (double)fadeSamples;
    _synthesizedSample[i] *= gain;
  }
  
  // fade out
  for(int i=0; i<fadeSamples; ++i)
  {
    int i2 = outputFrames - 1 - i;
    double gain = (double)i / (double)fadeSamples;
    _synthesizedSample[i2] *= gain;
  }

  normalize(_synthesizedSample);
  _synthesizedSample.sampleRate = synthParams.sampleRate;
}


void VutuController::onMessage(Message m)
{
  if(!m.address) return;
  
  //std::cout << "VutuController::onMessage:" << m.address << " " << m.value << " \n ";
  
  bool messageHandled{false};
  
  Path addr = m.address;
  switch(hash(head(addr)))
  {
    case(hash("set_param")):
    {
      Path whatParam = tail(addr);
      params.setFromNormalizedValue(whatParam, m.value);
      broadcastParam(whatParam, m.flags);
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

          params.setValue("analysis_interval", Interval{0, 1});
          broadcastParam("analysis_interval", 0);

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
        case(hash("export_synth")):
        {
          // save synthesized audio to a file
          if(getSize(_synthesizedSample))
          {
            if(auto savePath = showSaveDialog("audio"))
            {
              File saveFile (savePath);
              saveSampleToWavFile(_synthesizedSample, savePath);
            }
          }
          messageHandled = true;
          break;
        }
        case(hash("analyze")):
        {
          _clearPartialsData();
          _clearSynthesizedSample();
          if(getSize(_sourceSample))
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
          if(pLorisPartials && pLorisPartials->size() > 0)
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
          VutuPartialsData* pPartials = _vutuPartials.get();
          bool partialsOK = pPartials && (pPartials->partials.size() > 0);
          if(partialsOK)
          {
            if(auto savePath = showSaveDialog("partials"))
            {
              std::cout << "saving to path: " << savePath << "\n";
              auto ext = getExtensionFromPath(savePath);
              
              std::cout << "extension: " << ext << "\n";
              if(ext == "utu")
              {
                // tuck current fundamental param value into partials data
                pPartials->fundamental = params.getRealFloatValue("fundamental");
                
                auto partialsJson = vutuPartialsToJSON(*pPartials);
                auto partialsText = JSONToText(partialsJson);
                File saveFile (savePath);
                if(saveFile.hasWriteAccess())
                {
                  saveTextToPath(partialsText, savePath);
                }
                else
                {
                  std::cout << "export partials: no write access!\n";
                  // TODO error
                }
              }
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
              _sumuToLorisPartials(_vutuPartials.get(), _lorisPartials.get() );
              
              // clear source sample so all data is consistent
              clear(_sourceSample);
              broadcastSourceSample();
              
              // clear synthesized sample and sync UI and params
              _clearSynthesizedSample();
              broadcastPartialsData();
              broadcastSynthesizedSample();
              setButtonEnableStates();
              setAnalysisParamsFromPartials();
              
              
              // set interval to whole partials file and broadcast
              params.setValue("analysis_interval", Interval{0, 1});
              broadcastParam("analysis_interval", 0);
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

