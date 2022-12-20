// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLFiles.h"
#include "MLPropertyTree.h"
#include "MLAppController.h"

#include "vutuParameters.h"
#include "vutuProcessor.h"
#include "vutuView.h"

#include "sumuDSP.h"
#include "sumuPartials.h"

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

  // send a ml::Message directly to the Processor.
  void sendMessageToProcessor(Message m);
  
  // update the named collection of files and return a pointer to it.
  FileTree* updateCollection(Path which);

  
private:
  
  sumu::Sample _sample;
  std::unique_ptr< Loris::PartialList > _lorisPartials;
  std::unique_ptr< SumuPartialsData > _sumuPartials;

  sumu::Sample _synthesizedSample;
  
  void _clearPartialsData();
  int _loadSampleFromDialog();
  int analyzeSample();
  void synthesize();
  
  // the state to which we can revert, stored as normalized values.
  Tree< Value > _revertState;
  bool _changedFromRevertValues{true};


  
  Timer _debugTimer;
  void _debug();
  void _printToConsole(TextFragment t);
  
  Symbol playMode{ "off" };

};
