// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLDSPSample.h"
#include "MZFiles.h"
#include "MLPropertyTree.h"
#include "MLSignalProcessor.h"
#include "MLActor.h"
#include "MZProcessorUtils.h"

#include "vutuParameters.h"
#include "vutuProcessor.h"
#include "vutuView.h"

#include "vutuPartials.h"

#include "sndfile.hh"

using namespace ml;


//-----------------------------------------------------------------------------
// VutuController: owns the parameters and does all the non-realtime
// orchestration (file I/O, analysis, synthesis). Replaces the old
// mlvg AppController: SignalProcessor supplies the ParameterStore and the
// setViewMessageHandler; Actor supplies the message queue.
class VutuController final:
  public SignalProcessor, public Actor
{
public:

  VutuController(TextFragment appName, size_t instanceNum, const ParameterDescriptionList& pdl);
	~VutuController();

  // Actor interface
  void onMessage(Message m) override;

  // enable / disable the right buttons on the View
  void setButtonEnableStates();

  // send the current value of one / all params to the View and Processor.
  void broadcastParam(Path pname, uint32_t flags);
  void broadcastParams();

private:

  Path _controllerName;
  Path _processorName;


  ml::Sample _sourceSample;
  ml::Sample _synthesizedSample;

  std::unique_ptr< VutuPartialsData > _vutuPartials;

  int saveSampleToWavFile(const ml::Sample& signal, TextPath wavPath);

  int loadSampleFromPath(TextPath samplePath);
  int loadPartialsFromPath(TextPath samplePath);

  void saveTextToPath(const TextFragment& text, TextPath savePath);

  void showAnalysisInfo();
  void setAnalysisParamsFromPartials();

  int _loadSampleFromDialog();
  int analyzeSample();
  void broadcastSourceSample();

  void _clearPartialsData();
  void broadcastPartialsData();

  void synthesize();

  void _clearSynthesizedSample();
  void broadcastSynthesizedSample();
  void syncIntervals();

  // the state to which we can revert, stored as normalized values.
  Tree< Value > _revertState;
  bool _changedFromRevertValues{true};

  // file paths
  // TODO persist in app prefs
  TextPath recentSamplesInPath;
  TextPath recentSamplesOutPath;
  TextPath recentPartialsInPath;
  TextPath recentPartialsOutPath;

  File sourceFileLoaded;
  
  Timer _debugTimer;
  void _debug();
  void _printToConsole(TextFragment t);
  
  Symbol playMode{ "off" };

};
