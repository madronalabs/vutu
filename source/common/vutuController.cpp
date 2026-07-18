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
//#include "miniz.h"

// vutu analysis library
#include "utuAnalyzer.h"
#include "utuSynth.h"

using namespace ml;

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
  
  bool partialsOK = _vutuPartials && (_vutuPartials->partials.size() > 0);
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

int VutuController::loadSampleFromPath(Path samplePath)
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


void VutuController::saveTextToPath(const TextFragment& text, Path savePath)
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

  // make a faded copy of the analysis interval
  std::vector< float > vx;
  vx.resize(framesInInterval);
  int srcStart = frameInterval.mX1;
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

  // set sample rate and configure analyzer
  int sr = _sourceSample.sampleRate;
  auto res = params.getRealFloatValue("resolution");
  auto width = params.getRealFloatValue("window_width");
  auto drift = params.getRealFloatValue("freq_drift");
  auto floor = params.getRealFloatValue("amp_floor");
  auto loCut = params.getRealFloatValue("lo_cut");
  auto hiCut = params.getRealFloatValue("hi_cut");
  auto noiseWidth = params.getRealFloatValue("noise_width");

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

  if(newPartials && (newPartials->partials.size() > 0))
  {
    status = true;

    _vutuPartials = std::move(newPartials);
    cutHighs(*_vutuPartials, hiCut);
    cleanOutliers(*_vutuPartials);
    calcStats(*_vutuPartials);
    showAnalysisInfo();

    // store analysis params used
    _vutuPartials->type = Symbol(kVutuPartialsFileType);
    _vutuPartials->version = kVutuPartialsFileVersion;
    _vutuPartials->sourceFile = sourceFileLoaded.getShortName();
    _vutuPartials->sourceDuration = getDuration(_sourceSample);
    _vutuPartials->resolution = res;
    _vutuPartials->windowWidth = width;
    _vutuPartials->ampFloor = floor;
    _vutuPartials->freqDrift = drift;
    _vutuPartials->loCut = loCut;
    _vutuPartials->hiCut = hiCut;
  }
  return status;
}

// generate the synthesized audio from the partials.
// note output sample may be a different sample rate!
void VutuController::synthesize()
{
  if(!_vutuPartials.get()) return;

  std::vector<float> destSamples;
  const float kFadeTime = 0.001f;

  // get frames in analysis interval to use for output length. Length of synthesis will be shorter.
  Interval analysisInterval = params.getRealValue("analysis_interval").getIntervalValue();
  float duration = _vutuPartials->sourceDuration*(analysisInterval.mX2 -  analysisInterval.mX1);
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
          // load from saved origin or default
          // TODO make a function
          Path loadOriginDir(recentSamplesInPath);

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
            File exportOriginDir(recentSamplesOutPath);
            if(!exportOriginDir)
            {
              exportOriginDir = FileUtils::getUserDataPath();
            }

            auto shortName = textUtils::stripExtension(sourceFileLoaded.getShortName());
            auto savePath = FileDialog::getFilePathForSave(exportOriginDir.getFullPath(), TextFragment(shortName, ".wav"));
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
          // export .vutu partials
          VutuPartialsData* pPartials = _vutuPartials.get();
          bool partialsOK = pPartials && (pPartials->partials.size() > 0);
          if(partialsOK)
          {
            
            Path exportOriginDir(recentPartialsOutPath);
            if(!exportOriginDir)
            {
              exportOriginDir = FileUtils::getApplicationDataPath(getMakerName(), "Vutu", "");
            }
            auto shortName = textUtils::stripExtension(sourceFileLoaded.getShortName());
            if(!shortName) shortName = "partials-export";

            auto savePath = FileDialog::getFilePathForSave(exportOriginDir, TextFragment(shortName, ".utu"));
            if(savePath)
            {
              auto ext = getExtensionFromPath(savePath);

              if(ext == "utu")
              {
                // tuck current fundamental param value into partials data
                pPartials->fundamental = params.getRealFloatValue("fundamental");
                
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
          Path importOriginDir(recentPartialsInPath);
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

