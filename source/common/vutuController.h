// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLDSPSample.h"
#include "MZFiles.h"
#include "MLPropertyTree.h"
#include "MZAppController.h"

#include "vutuParameters.h"
#include "vutuProcessor.h"
#include "vutuView.h"

#include "vutuPartials.h"

#include "sndfile.hh"

#include "loris.h"

using namespace ml;


//-----------------------------------------------------------------------------
class VutuController final:
  public AppController
{
public:
  
  VutuController(TextFragment appName, const ParameterDescriptionList& pdl);
	~VutuController();

  // Actor interface
  void onMessage(Message m) override;

  // update the named collection of files and return a pointer to it.
  FileTree* updateCollection(Path which);

  // enable / disable the right buttons on the View
  void setButtonEnableStates();
  
private:

  ml::Sample _sourceSample;
  ml::Sample _synthesizedSample;

  std::unique_ptr< Loris::PartialList > _lorisPartials;
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
