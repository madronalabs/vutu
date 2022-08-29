// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLFiles.h"
#include "MLPropertyTree.h"
#include "MLActor.h"

#include "utuViewParameters.h"
#include "utuViewProcessor.h"
#include "utuViewView.h"



using namespace ml;

constexpr int kChangeQueueSize{128};

//-----------------------------------------------------------------------------
class UtuViewController :
  public Actor
{
public:
  
  UtuViewController();
	~UtuViewController();

  
  // UtuViewController interface
 // UtuViewView* createUtuViewView();
  void sendMessageToView(Message);
  void sendParamToView(Path pname);
  void sendAllParamsToView();
  void sendParamToProcessor(Path pname, uint32_t flags);
  void sendAllParamsToProcessor();

  // Actor interface
  void onMessage(Message m) override;
  void onFullQueue() override;
  
  // send a ml::Message directly to the Processor.
  void sendMessageToProcessor(Message m);
  
  // update the named collection of files and return a pointer to it.
  FileTree* updateCollection(Path which);
  
private:
  // Parameters, which can be exposed with Processor and host app through the VST interface.
  // in the Processor the parameter tree is in plain units. Everywhere else it is normalized.
  ParameterTreeNormalized _params;
  
  // the state to which we can revert, stored as normalized values.
  Tree< Value > _revertState;
  bool _changedFromRevertValues{true};
  
  std::vector< ml::Path > _paramNamesByID;
  Tree< size_t > _paramIDsByName;
  
  // signals that the View has subscribed to. For now, this just contains true if the
  // View subscribes to the signal.
  Tree< size_t > _signalsSubscribedByView;

  SharedResourcePointer< ml::Timers > _timers ;
  
  // class used for assigning each instance of our Controller a unique ID
  // TODO refactor w/ ProcessorRegistry etc.
  class ControllerRegistry
  {
    std::mutex _IDMutex;
    size_t _IDCounter{0};
  public:
    size_t getUniqueID()
    {
      std::unique_lock<std::mutex> lock(_IDMutex);
      return ++_IDCounter;
    }
  };
  
  SharedResourcePointer< ActorRegistry > _actorRegistry ;
  SharedResourcePointer< ControllerRegistry > _controllerRegistry ;
  int _instanceNum;
  Path _instanceName;
  Path _viewName;
  Path _processorName;
  Path _currentLearnParam;
  
  // an index to file trees
  Tree< std::unique_ptr< FileTree > > _fileTreeIndex;
  
  Timer _debugTimer;
  void debug();
  
  /*
  TextFragment getPatchHeader(size_t bytes);
  TextFragment getPatchAsText();
  void setPatchFromText(TextFragment t);
  std::vector< uint8_t > getPatchAsBinary();
  void setPatchFromBinary(const std::vector< uint8_t > & p);
*/
  

};
