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

#include "MZFiles.h"
//#include "miniz.h"

// vutu analysis library
#include "utuAnalyzer.h"
#include "utuSynth.h"
#include "utuAutoParams.h"

using namespace ml;

//-----------------------------------------------------------------------------
// VutuController implementation

VutuController::VutuController(TextFragment appName, size_t instanceNum, const ParameterDescriptionList& pdl)
{
  // set up names for message routing and register ourself as an Actor.
  _controllerName = TextFragment(appName, "controller", ml::textUtils::naturalNumberToText(instanceNum));
  _processorName = TextFragment(appName, "processor", ml::textUtils::naturalNumberToText(instanceNum));
  registerActor(_controllerName, this);

  buildParams(pdl);
  setDefaultParams();

  _debugTimer.start([=]() { _debug(); }, milliseconds(1000));
}

// send the current value of one param to the View (which the manzanita
// AppView expects as a real value) and to the audio Processor.
void VutuController::broadcastParam(Path pname, uint32_t flags)
{
  Value realVal = params_.getRealValueAtPath(pname);
  sendMessageToActor(_processorName, {Path("set_param", pname), realVal, flags});
  sendParamToView(*this, pname);
}

void VutuController::broadcastParams()
{
  for(const auto& paramDesc : params_.descriptions)
  {
    Path pname = textToPath(paramDesc->getTextProperty("name"));
    broadcastParam(pname, 0);
  }
}

bool VutuController::autoMode()
{
  return params_.getRealFloatValue("auto_mode") > 0.5f;
}

int VutuController::activeBudget()
{
  // the max_active param's real value is the list item index 0..5.
  int idx = (int)lround(params_.getRealFloatValue("max_active"));
  idx = (idx < 0) ? 0 : ((idx > 5) ? 5 : idx);
  return 16 << idx;  // 16, 32, 64, 128, 256, 512
}

VutuController::~VutuController()
{
  // don't stop the master Timers-- there may be other plugin instances using it!
  // std::cout << "VutuController: BYE!\n";

}

void VutuController::setButtonEnableStates()
{
  sendMessageToView({"widget/play_source/set_prop/enabled", usable(&_sourceSample)});
  sendMessageToView({"widget/analyze/set_prop/enabled", usable(&_sourceSample)});
  
  bool partialsOK = _vutuPartials && (_vutuPartials->partials.size() > 0);
  sendMessageToView({"widget/synthesize/set_prop/enabled", partialsOK});
  sendMessageToView({"widget/export/set_prop/enabled", partialsOK});
  
  sendMessageToView({"widget/play_synth/set_prop/enabled", getSize(_synthesizedSample) > 0});
  sendMessageToView({"widget/export_synth/set_prop/enabled", getSize(_synthesizedSample) > 0});

  // in auto mode the 7 analysis dials are shown but locked (dimmed, not editable).
  bool dialsEnabled = !autoMode();
  for(auto nm : {"resolution", "window_width", "amp_floor", "freq_drift", "lo_cut", "hi_cut", "noise_width"})
  {
    Path addr("widget", Path(nm), "set_prop", "enabled");
    sendMessageToView({addr, dialsEnabled});
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
  sendMessageToView({"info/set_prop/text", t});
}

void VutuController::broadcastSourceSample()
{
  // send synthesized audio to View and Processor
  ml::Sample* pSample = &_sourceSample;
  Value samplePtrValue(reinterpret_cast<const uint8_t*>(&pSample), sizeof(ml::Sample*));
  sendMessageToActor(_processorName, {"do/set_source_data", samplePtrValue});
  sendMessageToView({"do/set_source_data", samplePtrValue});
}

void VutuController::_clearPartialsData()
{
  // clear data
  _vutuPartials = std::make_unique< VutuPartialsData >();
}

void VutuController::broadcastPartialsData()
{
  // send Partials to View and Processor
  VutuPartialsData* pPartials = _vutuPartials.get();
  Value partialsPtrValue(reinterpret_cast<const uint8_t*>(&pPartials), sizeof(VutuPartialsData*));
  sendMessageToActor(_processorName, {"do/set_partials_data", partialsPtrValue});
  sendMessageToView({"do/set_partials_data", partialsPtrValue});

}

void VutuController::_clearSynthesizedSample()
{
  clear(_synthesizedSample);
}

void VutuController::broadcastSynthesizedSample()
{
  // send synthesized audio to View and Processor
  ml::Sample* pSample = &_synthesizedSample;
  Value samplePtrValue(reinterpret_cast<const uint8_t*>(&pSample), sizeof(ml::Sample*));
  sendMessageToActor(_processorName, {"do/set_synth_data", samplePtrValue});
  sendMessageToView({"do/set_synth_data", samplePtrValue});
}


int VutuController::saveSampleToWavFile(const Sample& sample, TextPath wavPath)
{
  int OK{ false };
  std::cout << "saveSampleToWavFile: " << wavPath << "\n";
  
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

int VutuController::loadSampleFromPath(TextPath samplePath)
{
  int OK{ false };
  File fileToLoad(samplePath);
  if(fileToLoad)
  {
    // load the file
    auto filePathText = fileToLoad.getFullPathAsText();
    SF_INFO fileInfo{};
    sf_count_t framesRead{0};
    float* pData{nullptr};

    auto fText = filePathText.getText();
    std::cout << "file as text: " << filePathText.getText() << "\n";

    SNDFILE* file = sf_open(filePathText.getText(), SFM_READ, &fileInfo);
    if(file)
    {
      //  auto p2 = file.getFullPath();
      //  std::cout << "loadSampleFromPath: file fullPath: " << p2 << " (length " << p2.getSize() << ")\n";



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
        TextFragment fileName = last(samplePath);
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
  TextFragment b(" [", floatToText(p->stats.timeRange.x1), " -- " ,floatToText(p->stats.timeRange.x2), "] " );
  TextFragment c("partials: ", intToText(p->stats.nPartials));
  TextFragment d(" max freq: ", intToText(p->stats.freqRange.x2));
  TextFragment e(" max active: ", intToText(p->stats.maxActivePartials));

  TextFragment out(a, b, c, d, e);

  // in manual mode, warn right after the max-active count if it exceeds the budget.
  if(!autoMode())
  {
    int budget = activeBudget();
    if((int)p->stats.maxActivePartials > budget)
    {
      out = TextFragment(out, " (over budget of ", intToText(budget), ")");
    }
  }

  _printToConsole(out);
}

void VutuController::setAnalysisParamsFromPartials()
{
  VutuPartialsData* p = _vutuPartials.get();
  
  params_.setFromRealValue("fundamental", p->fundamental);
  broadcastParam("fundamental", 0);

}

int VutuController::loadPartialsFromPath(TextPath partialsPath)
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


void VutuController::saveTextToPath(const TextFragment& text, TextPath savePath)
{
  if(!savePath) return;
  
  File saveFile (savePath);


  if (saveFile.replaceWithText(text))
  {
      std::cout << "saved text to " << savePath << "\n";
  }
  else
  {
      // TODO other save errors
  }

}

// make a faded copy of the current analysis interval of the source sample.
// returns false if there is no usable source. srOut is set to the sample rate.
bool VutuController::getAnalysisIntervalSamples(std::vector< float >& vx, int& srOut)
{
  auto totalFrames = getFrames(_sourceSample);
  if(!totalFrames) return false;

  auto interval = valueToInterval(params_.getRealValue("analysis_interval"));
  auto frameInterval = interval*float(totalFrames);

  int framesInInterval = frameInterval.x2 - frameInterval.x1;
  if(framesInInterval <= 0) return false;

  const float kFadeTime = 0.001f;
  int fadeSamples = kFadeTime*_sourceSample.sampleRate;

  vx.resize(framesInInterval);
  int srcStart = frameInterval.x1;
  for(int i=0; i<framesInInterval; ++i)
  {
    vx[i] = _sourceSample[srcStart + i];
  }

  // fade in
  for(int i=0; i<fadeSamples; ++i)
  {
    float gain = (float)i / (float)fadeSamples;
    vx[i] *= gain;
  }

  // fade out
  for(int i=0; i<fadeSamples; ++i)
  {
    int i2 = framesInInterval - 1 - i;
    float gain = (float)i / (float)fadeSamples;
    vx[i2] *= gain;
  }

  srOut = _sourceSample.sampleRate;
  return true;
}

// estimate the analysis parameters for the current source (constrained by the
// active-partials budget), write them to the dials, and run the analysis.
// fundamental stays manual and is not touched.
void VutuController::runAutoParams()
{
  std::vector< float > vx;
  int sr{0};
  if(!getAnalysisIntervalSamples(vx, sr)) return;

  auto r = utu::computeAnalyzerParams(vx.data(), vx.size(), float(sr), activeBudget());

  params_.setFromRealValue("resolution", r.params.resolution);
  params_.setFromRealValue("window_width", r.params.windowWidth);
  params_.setFromRealValue("amp_floor", r.params.ampFloor);
  params_.setFromRealValue("freq_drift", r.params.freqDrift);
  params_.setFromRealValue("lo_cut", r.params.freqFloor);
  params_.setFromRealValue("noise_width", r.params.noiseWidth);
  params_.setFromRealValue("hi_cut", r.hiCut);

  for(auto nm : {"resolution", "window_width", "amp_floor", "freq_drift", "lo_cut", "noise_width", "hi_cut"})
  {
    broadcastParam(Path(nm), 0);
  }

  // run the analysis with the FULL estimated params (r.params keeps
  // sidelobeLevel etc. that the dials don't carry, so the partial budget is
  // honored), not just the written-back dial values.
  auto newPartials = utu::analyzeToPartials(vx.data(), vx.size(), r.params);
  finishAnalysis(std::move(newPartials), r.params, r.hiCut);
  broadcastPartialsData();
  setButtonEnableStates();
}

// store an analysis result: trim, clean, compute stats, show info, and record
// the params used. returns true if partials were produced.
int VutuController::finishAnalysis(std::unique_ptr< VutuPartialsData > newPartials,
                                   const utu::AnalyzerParams& p, float hiCut)
{
  if(!newPartials || (newPartials->partials.size() == 0)) return false;

  _vutuPartials = std::move(newPartials);
  cutHighs(*_vutuPartials, hiCut);
  cleanOutliers(*_vutuPartials);
  calcStats(*_vutuPartials);
  showAnalysisInfo();

  _vutuPartials->type = Symbol(kVutuPartialsFileType);
  _vutuPartials->version = kVutuPartialsFileVersion;
  _vutuPartials->sourceFile = sourceFileLoaded.getShortName();
  _vutuPartials->sourceDuration = getDuration(_sourceSample);
  _vutuPartials->resolution = p.resolution;
  _vutuPartials->windowWidth = p.windowWidth;
  _vutuPartials->ampFloor = p.ampFloor;
  _vutuPartials->freqDrift = p.freqDrift;
  _vutuPartials->loCut = p.freqFloor;
  _vutuPartials->hiCut = hiCut;
  return true;
}

int VutuController::analyzeSample()
{
  std::vector< float > vx;
  int sr{0};
  if(!getAnalysisIntervalSamples(vx, sr)) return false;
  int framesInInterval = (int)vx.size();

  // configure analyzer from the dial values
  auto res = params_.getRealFloatValue("resolution");
  auto width = params_.getRealFloatValue("window_width");
  auto drift = params_.getRealFloatValue("freq_drift");
  auto floor = params_.getRealFloatValue("amp_floor");
  auto loCut = params_.getRealFloatValue("lo_cut");
  auto hiCut = params_.getRealFloatValue("hi_cut");
  auto noiseWidth = params_.getRealFloatValue("noise_width");

  utu::AnalyzerParams analyzerParams;
  analyzerParams.sampleRate = sr;
  analyzerParams.resolution = res;
  // legacy presets may carry only a resolution; the analyzer is
  // window-primary now
  analyzerParams.windowWidth = (width > 0.f) ? width : 2.f * res;
  analyzerParams.freqDrift = drift;
  analyzerParams.ampFloor = floor;
  analyzerParams.freqFloor = loCut;
  analyzerParams.noiseWidth = noiseWidth;

  auto newPartials = utu::analyzeToPartials(vx.data(), framesInInterval, analyzerParams);
  return finishAnalysis(std::move(newPartials), analyzerParams, hiCut);
}

// generate the synthesized audio from the partials.
// note output sample may be a different sample rate!
void VutuController::synthesize()
{
  if(!_vutuPartials.get()) return;

  std::vector<float> destSamples;
  const float kFadeTime = 0.001f;

  // get frames in analysis interval to use for output length. Length of synthesis will be shorter.
  Interval analysisInterval = valueToInterval(params_.getRealValue("analysis_interval"));
  float duration = _vutuPartials->sourceDuration*(analysisInterval.x2 -  analysisInterval.x1);
  int framesAnalyzed = duration*kSampleRate;

  // run the synthesizer
  utu::SynthParams synthParams;
  synthParams.sampleRate = kSampleRate;
  synthParams.fadeTime = kFadeTime;
  utu::PartialSynthesizer synth;
  synth.setParams(synthParams);
  synth.render(*_vutuPartials, destSamples);

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
      params_.setFromRealValue(whatParam, m.value);
      broadcastParam(whatParam, m.flags);

      // react to the analysis-mode controls.
      switch(hash(head(whatParam)))
      {
        case(hash("auto_mode")):
        {
          // lock / unlock the analysis dials, and analyze now if switching to auto.
          setButtonEnableStates();
          if(autoMode() && getSize(_sourceSample)) runAutoParams();
          break;
        }
        case(hash("max_active")):
        {
          if(autoMode())
          {
            if(getSize(_sourceSample)) runAutoParams();
          }
          else if(_vutuPartials && (_vutuPartials->partials.size() > 0))
          {
            // manual: refresh the info line so the over-budget warning updates.
            showAnalysisInfo();
          }
          break;
        }
        default:
          break;
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
          sendMessageToView({"widget/source/set_prop/playback_time", m.value});
          break;
        }
        case(hash("synth_time")):
        {
          sendMessageToView({"widget/synth/set_prop/playback_time", m.value});
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
          // load from saved origin or default
          // TODO make a function
          TextPath loadOriginDir(recentSamplesInPath);

          if(!loadOriginDir)
          {

            loadOriginDir = FileUtils::getUserDataPath();
          }

          auto loadPath = FileDialog::getFilePathForLoad(loadOriginDir, "WAV audio:wav;AIFF audio:aiff,aif,aifc");

          if(loadPath)
          {
              recentSamplesInPath = loadPath;
            if(loadSampleFromPath(loadPath))
            {
              _clearPartialsData();
              _clearSynthesizedSample();
            }
          }
          broadcastSourceSample();
          broadcastPartialsData();
          broadcastSynthesizedSample();
          setButtonEnableStates();

          params_.setValue("analysis_interval", intervalToValue(Interval{0, 1}));
          broadcastParam("analysis_interval", 0);

          // in auto mode, analyze the newly loaded sample right away.
          if(autoMode() && getSize(_sourceSample)) runAutoParams();

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
            TextPath exportOriginDir(recentSamplesOutPath);
            if(!exportOriginDir)
            {
              exportOriginDir = FileUtils::getUserDataPath();
            }

            auto shortName = textUtils::stripExtension(sourceFileLoaded.getShortName());
            auto savePath = FileDialog::getFilePathForSave(exportOriginDir, TextFragment(shortName, ".wav"));
            if(savePath)
            {
              recentSamplesOutPath = savePath;
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
          if(_vutuPartials && (_vutuPartials->partials.size() > 0))
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
              sendMessageToView({"widget/play_source/set_prop/text", TextFragment("stop")});
              break;
            }
            case(hash("synth")):
            {
              sendMessageToView({"widget/play_synth/set_prop/text", TextFragment("stop")});
              break;
            }
          }
          messageHandled = true;
          break;
        }
        case(hash("playback_stopped")):
        {
          // switch play button texts
          sendMessageToView({"widget/play_source/set_prop/text", TextFragment("play")});
          sendMessageToView({"widget/play_synth/set_prop/text", TextFragment("play")});
          sendMessageToView({"widget/sample/set_prop/playback_time", 0.f});
          messageHandled = true;
          break;
        }
        case(hash("export")):
        {
          // export .vutu partials
          VutuPartialsData* pPartials = _vutuPartials.get();
          bool partialsOK = pPartials && (pPartials->partials.size() > 0);
          if(partialsOK)
          {
            
            TextPath exportOriginDir(recentPartialsOutPath);
            if(!exportOriginDir)
            {
              exportOriginDir = FileUtils::getApplicationDataPath(getMakerName(), "Vutu", "");
            }
            auto shortName = textUtils::stripExtension(sourceFileLoaded.getShortName());
            if(!shortName) shortName = "partials-export";

            auto savePath = FileDialog::getFilePathForSave(exportOriginDir, TextFragment(shortName, ".utu"));
            if(savePath)
            {
              auto ext = FileUtils::getExtensionFromPath(savePath);

              if(ext == "utu")
              {
                // tuck current fundamental param value into partials data
                pPartials->fundamental = params_.getRealFloatValue("fundamental");
                
                auto partialsJson = vutuPartialsToJSON(*pPartials);
                auto partialsText = JSONToText(partialsJson);
                File saveFile (savePath);

                recentPartialsOutPath = savePath;
                saveTextToPath(partialsText, savePath);

              }
            }
          }
          messageHandled = true;
          break;
        }
        case(hash("import")):
        {
          TextPath importOriginDir(recentPartialsInPath);
          if (!importOriginDir)
          {
              importOriginDir = FileUtils::getApplicationDataPath(getMakerName(), "Vutu", "");
          }

          auto loadPath = FileDialog::getFilePathForLoad(importOriginDir, "Partials:utu");

          if(loadPath)
          {
              recentPartialsInPath = loadPath;
            // load Sumu partials from JSON
            if(loadPartialsFromPath(loadPath))
            {
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
              params_.setValue("analysis_interval", intervalToValue(Interval{0, 1}));
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

  // messages we don't handle are simply ignored (there is no base-class
  // onMessage now that AppController is gone).
  (void)messageHandled;
}

