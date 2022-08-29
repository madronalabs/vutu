// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details


// use clipboard code from VST3 SDK
//#include "public.sdk/source/common/systemclipboard.h"

#include "utuViewController.h"
#include "utuViewView.h"
#include "utuView.h"

// param definitions for this plugin
#include "utuViewParameters.h"

#include <cmath>
#include <iostream>
#include <chrono>

#include "MLSerialization.h"

#include "mlvg.h"

// includes in mlvg - TODO wrap
#include "miniz.h"
#include "nfd.h"

//#define JUCE_APP_CONFIG_HEADER "external/juce_core/JuceCoreConfig.h"
///#include "external/juce_core/juce_core.h"

using namespace ml;

Path _incrementVersion(Path currentPath)
{
  using namespace ml::textUtils;
  
  Symbol finalSym = last(currentPath);
  int currVersion = getFinalNumber(finalSym);
  
  // if there is no current version, currVersion = 0 and we start at 1
  int nextVersion = currVersion + 1;
  Symbol nextFinalSym = addFinalNumber(stripFinalNumber(finalSym), nextVersion);
  Path nextPath(butLast(currentPath), nextFinalSym);
  return nextPath;
}

//-----------------------------------------------------------------------------
// UtuViewController implementation

UtuViewController::UtuViewController()
{
#ifdef DEBUG
#ifdef ML_WINDOWS
  AllocConsole();
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
#endif
#endif

  // make parameters and projections
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);
  buildParameterTree(pdl, _params);
  setDefaults(_params);
  
  _params.dump();
  
  // store IDs by name and param names by ID
  for(size_t i=0; i < pdl.size(); ++i)
  {
    ParameterDescription& pd = *pdl[i];
    Path paramName = pd.getProperty("name").getTextValue();
    _paramIDsByName[paramName] = i;
    _paramNamesByID.push_back(paramName);
  }
  
  /*
  getProcessorStateParams(pdl, _processorStateParams);
  getControllerStateParams(pdl, _controllerStateParams);
*/
  
  // start timers in main thread.
  _timers->start(true);
  
  // register and start Actor
  _instanceNum = _controllerRegistry->getUniqueID();
  _instanceName = TextFragment(getAppName(), "controller", ml::textUtils::naturalNumberToText(_instanceNum));
  registerActor(_instanceName, this);
  Actor::start();
  
  _debugTimer.start([=]() { debug(); }, milliseconds(1000));

  Path patchesRoot = getApplicationDataRoot(getMakerName(), getAppName(), "patches");
  _fileTreeIndex["patches"] = ml::make_unique< FileTree >(patchesRoot, "mlpreset");
}

UtuViewController::~UtuViewController()
{
  // don't stop the master Timers-- there may be other plugin instances using it!
  // std::cout << "UtuViewController: BYE!\n";
}

#pragma mark mlvg

/*
UtuViewView* UtuViewController::createUtuViewView ()
{
    auto defaultSize = _params["view_size"].getMatrixValue();
    float w = defaultSize[0];
    float h = defaultSize[1];
    
    Rect size{0, 0, w, h};
  
    auto newView = new UtuViewView(size, _instanceName);
    
    _viewName = TextFragment(getAppName(), "view", ml::textUtils::naturalNumberToText(_instanceNum));
    registerActor(_viewName, newView);
    
    // send all collections to view
    for(auto it = _fileTreeIndex.begin(); it != _fileTreeIndex.end(); ++it)
    {
      const Path p = it.getCurrentNodePath();
      sendMessageToActor(_viewName, {"editor/do/update_collection", pathToText(p)});
    }
    
    sendAllParamsToView();

    return newView;
}

*/

void UtuViewController::sendMessageToView(Message msg)
{
  sendMessageToActor(_viewName, msg);
}

void UtuViewController::sendParamToView(Path pname)
{
  sendMessageToActor(_viewName, {Path("set_param", pname), _params[pname], kMsgFromController});
}

void UtuViewController::sendAllParamsToView()
{
  for(auto& pname : _paramNamesByID)
  {
    sendParamToView(pname);
  }
}

void UtuViewController::sendAllParamsToProcessor()
{
  for(auto& pname : _paramNamesByID)
  {
    sendParamToProcessor(pname, kMsgSequenceStart | kMsgSequenceEnd);
  }
}

/*
TextFragment UtuViewController::getPatchHeader(size_t bytes)
{
  TextFragment pn2(getAppName(), ":", _params["current_patch"].getTextValue());
  TextFragment pn3(pn2, ":", textUtils::naturalNumberToText(bytes), " bytes");
  TextFragment pn4(pn3, "\n");
  return pn4;
}

TextFragment UtuViewController::getPatchAsText()
{
  // TODO manage memory here for patches that might be
  // too big for stack
  
  // get params data in plain projections as binary
  auto procStateValueTree = keepNodesInList(getPlainValues(_params), _processorStateParams);
  auto t = JSONToText(valueTreeToJSON(procStateValueTree));

  
  return t;
}

void UtuViewController::setPatchFromText(TextFragment t)
{
  auto newParams = JSONToValueTree(textToJSON(t));
  _params.setFromPlainValues(newParams);
}

std::vector< uint8_t > UtuViewController::getPatchAsBinary()
{
  // TODO manage memory here for patches that might be
  // too big for stack
  
  // get params data in plain projections as binary
  auto procStateValueTree = keepNodesInList(getPlainValues(_params), _processorStateParams);
  auto binaryData = valueTreeToBinary(procStateValueTree);
  
  // compress it
  size_t srcLen = binaryData.size();
  size_t cmpEstimateLen = compressBound(srcLen);
  size_t uncompLen = srcLen;
  mz_ulong cmpActualLen = cmpEstimateLen;
  std::vector< uint8_t > compressedData;
  
  compressedData.resize(cmpEstimateLen);
  
  int cmpResult = compress(compressedData.data(), &cmpActualLen, binaryData.data(), srcLen);
  //printf("Compressed from %u to %u bytes\n", (mz_uint32)srcLen, (mz_uint32)cmpActualLen);
  compressedData.resize(cmpActualLen);
  
  return compressedData;
}

void UtuViewController::setPatchFromBinary(const std::vector< uint8_t >& p)
{
  mz_ulong uncompSize = p.size();
  std::vector< uint8_t > uncompressedData;
  size_t maxSize = 1024*1024;
  int cmpResult{MZ_BUF_ERROR};
  
  // with our best guess of buffer size, try to uncompress. if the result doesn't
  // fit, double the buffer size and try again.
  while((MZ_BUF_ERROR == cmpResult) && (uncompSize < maxSize))
  {
    uncompSize *= 2;
    uncompressedData.resize(uncompSize);
    cmpResult = uncompress(uncompressedData.data(), &uncompSize, p.data(), p.size());
  }
  
  if(MZ_BUF_ERROR != cmpResult)
  {
    auto newProcParams = binaryToValueTree(uncompressedData);
    _params.setFromPlainValues(newProcParams);
  }
  else
  {
    std::cout << "setPatchFromBinary: decompress failed!\n";
  }
}
*/

void UtuViewController::sendParamToProcessor(Path pname, uint32_t flags)
{
 
}
 
void UtuViewController::sendMessageToProcessor(Message msg)
{

}

void UtuViewController::onFullQueue()
{
  std::cout << "Controller: full queue! \n";
}

FileTree* UtuViewController::updateCollection(Path which)
{
  FileTree* pTree {_fileTreeIndex[which].get()};
  if(pTree)
  {
    pTree->scan();
  }
  
  return pTree;
}

void UtuViewController::debug()
{
  //std::cout << "UtuViewController: " << getMessagesAvailable() << " messages in queue. max: " << _maxQueueSize << "\n";
//  std::cout << "UtuViewController @ " << std::hex << (this) << std::dec << " : \n";
//  std::cout << "        timers @ " << std::hex << (&_timers.get()) << std::dec << "\n";
}

void UtuViewController::onMessage(Message m)
{
  if(!m.address) return;
  
  std::cout << "UtuViewController::onMessage:" << m.address << " " << m.value << " \n ";
  
  Path addr = m.address;
  switch(hash(head(addr)))
  {
    case(hash("set_param")):
    {
      // set a parameter from the normalized value in the message.
      Path whatParam = tail(addr);
      switch(hash(head(whatParam)))
      {

          
        case(hash("view_size")):
        {
          //std::cout << "UtuViewController::onMessage: view_size = " << m.value << " \n ";
          _params["view_size"] = m.value;
          break;
        }
          
        default:
        {
          // usual set_param messages
 
          // TODO
          // make undo-able value change here
          // then coalesce and add to history
          // undo will be another message
          
          // save in our ParameterTree
          _params.setParamFromNormalizedValue(whatParam, m.value);
          
          // send to processor
          sendParamToProcessor(whatParam, m.flags);
          
          break;
        }
      }
      break;
    }
      
    case(hash("set_prop")):
    {
      Path whatProp = tail(addr);
      switch(hash(head(whatProp)))
      {

      }
      break;
    }
      
    case(hash("do")):
    {
      Path whatAction = tail(addr);
      switch(hash(head(whatAction)))
      {
        case(hash("subscribe_to_signal")):
        {
          Path sigPath = textToPath(m.value.getTextValue());
          _signalsSubscribedByView[sigPath] = 1;
          break;
        }

      }
      break;
      
    }
      
    default:
    {
      // TODO forward?
      std::cout << "UtuViewController: unhandled message: " << m << " \n ";
      break;
    }
  }
}

