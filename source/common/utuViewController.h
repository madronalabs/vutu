// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLFiles.h"
#include "MLPropertyTree.h"
#include "MLAppController.h"

#include "utuViewParameters.h"
#include "utuViewProcessor.h"
#include "utuViewView.h"
#include "sumuDSP.h"

#include "sndfile.hh"

using namespace ml;

constexpr int kChangeQueueSize{128};

//-----------------------------------------------------------------------------
class UtuViewController final:
  public AppController
{
public:
  
  UtuViewController(TextFragment appName, const ParameterDescriptionList& pdl);
	~UtuViewController();

  // Actor interface
  void onMessage(Message m) override;

  // send a ml::Message directly to the Processor.
  void sendMessageToProcessor(Message m);
  
  // update the named collection of files and return a pointer to it.
  FileTree* updateCollection(Path which);
  
  void analyze();
  
private:
  
  utu::Sample _sample;

  void _loadFileFromDialog();
  
  // the state to which we can revert, stored as normalized values.
  Tree< Value > _revertState;
  bool _changedFromRevertValues{true};


  
  Timer _debugTimer;
  void _debug();
  void _printToConsole(TextFragment t);

};
