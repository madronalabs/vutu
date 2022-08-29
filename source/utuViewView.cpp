// mlvg: GUI library for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "utuViewView.h"

#include "madronalib.h"

#include "MLDialBasic.h"
#include "MLTextButtonBasic.h"
#include "MLResizer.h"
#include "MLTextLabelBasic.h"
#include "MLSVGImage.h"
#include "MLSVGButton.h"

#include "MLParameters.h"
#include "MLSerialization.h"

#include "sumuPartialsDisplay.h"

#include "utuViewProcessor.h"

#include "../build/resources/utu-view/resources.c"

ml::Rect smallDialRect{0, 0, 1.0, 1.0};
ml::Rect mediumDialRect{0, 0, 1.5, 1.5};
ml::Rect largeDialRect{0, 0, 2.5, 2.5};

float mediumDialSize{0.625f};
float largeDialSize{0.875f};

ml::Rect labelRect(0, 0, 3, 0.5);

UtuViewView::UtuViewView(TextFragment appName, size_t instanceNum) :
  AppView(appName, instanceNum)
{

}

void UtuViewView::makeWidgets()
{
  // add labels to background
  auto addControlLabel = [&](Path name, TextFragment t)
  {
    _view->_backgroundWidgets.add_unique< TextLabelBasic >(name, WithValues{
      { "bounds", rectToMatrix(labelRect) },
      { "h_align", "center" },
      { "v_align", "middle" },
      { "text", t },
      { "font", "d_din_oblique" },
      { "text_size", 0.50 },
      { "text_spacing", 0.0f }
    } );
  };
  addControlLabel("resolution_label", "resolution");
  addControlLabel("window_width_label", "window width");
  addControlLabel("amp_floor_label", "amp floor");
  addControlLabel("freq_drift_label", "freq. drift");
  
  // dials
  _view->_widgets.add_unique< DialBasic >("resolution", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"ticks", 11 },
    {"color", {0.945, 0.498, 0.125, 1} },
    {"indicator", {1, 0.725, 0.498, 1} },
    {"param", "resolution" }
  } );
  _view->_widgets.add_unique< DialBasic >("window_width", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"ticks", 11 },
    {"color", {0.901, 0.784, 0.145, 1} },
    {"indicator", {0.980, 0.894, 0.454, 1} },
    {"param", "window_width" }
  } );
  _view->_widgets.add_unique< DialBasic >("amp_floor", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"ticks", 11 },
    {"color", {0.627, 0.819, 0.286, 1} },
    {"indicator", {0.815, 0.976, 0.501, 1} },
    {"param", "amp_floor" }
  } );
  _view->_widgets.add_unique< DialBasic >("freq_drift", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"ticks", 11 },
    {"color", {0.627, 0.819, 0.286, 1} },
    {"indicator", {0.815, 0.976, 0.501, 1} },
    {"param", "freq_drift" }
  } );
  
  // buttons
  _view->_widgets.add_unique< TextButtonBasic >("open", WithValues{
    {"text", "open" },
    {"action", "open" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("analyze", WithValues{
    {"text", "analyze" },
    {"action", "analyze" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("play", WithValues{
    {"text", "play" },
    {"action", "play" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("synthesize", WithValues{
    {"text", "synthesize" },
    {"action", "synthesize" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("export", WithValues{
    {"text", "export" },
    {"action", "export" }
  } );
  
  // info label
  _view->_widgets.add_unique< TextLabelBasic >("info", WithValues{
    { "h_align", "center" },
    { "v_align", "middle" },
    { "text", "utu-view" },
    { "font", "d_din_oblique" },
    { "text_size", 0.375 },
    { "text_spacing", 0.0f }
  } );
  
  // partials
  _view->_widgets.add_unique< SumuPartialsDisplay >("partials", WithValues{
  } );
  
  forEach< Widget >
  (_view->_widgets, [&](Widget& w)
   {
    w.setProperty("visible", true);
  }
   );
}

void UtuViewView::layoutView()
{
  Vec2 gridDims = getSizeInGridUnits();
  int gx = gridDims.x();
  int gy = gridDims.y();
  
  // set grid size of entire view, for background and other drawing
  _view->setProperty("grid_units_x", gx);
  _view->setProperty("grid_units_y", gy);
  
  const int bottomHeight{7};
  int bottomY = gy - bottomHeight;
  float dialsY1 = bottomY + 2;
  float dialsY2 = bottomY + 4.5;
  
  // dials
  _view->_widgets["resolution"]->setRectProperty("bounds", alignCenterToPoint(largeDialRect, {2.0, dialsY1}));
  _view->_widgets["window_width"]->setRectProperty("bounds", alignCenterToPoint(largeDialRect, {5.0, dialsY1}));
  _view->_widgets["amp_floor"]->setRectProperty("bounds", alignCenterToPoint(largeDialRect, {3.5, dialsY2}));
  _view->_widgets["freq_drift"]->setRectProperty("bounds", alignCenterToPoint(largeDialRect, {6.5, dialsY2}));
  
  // labels
  auto positionLabelUnderDial = [&](Path dialName)
  {
    Path labelName (TextFragment(pathToText(dialName), "_label"));
    ml::Rect dialRect = _view->_widgets[dialName]->getRectProperty("bounds");
    _view->_backgroundWidgets[labelName]->setRectProperty
    ("bounds", alignCenterToPoint(labelRect, dialRect.bottomCenter() - Vec2(0, 0.25)));
  };
  for(auto dialName : {"resolution", "window_width", "amp_floor", "freq_drift"})
  {
    positionLabelUnderDial(dialName);
  }
  
  // buttons
  int centerX = gx/2;
  float  buttonWidth = 4;
  float halfButtonWidth = buttonWidth/2.f;

  float buttonsY1 = bottomY + 1.5;
  float buttonsY2 = bottomY + 2.5;
  float buttonsY3 = bottomY + 3.5;
  ml::Rect textButtonRect(0, 0, buttonWidth, 1);
  
  _view->_widgets["open"]->setRectProperty("bounds", alignCenterToPoint(textButtonRect, {centerX + halfButtonWidth, buttonsY1}));
  _view->_widgets["analyze"]->setRectProperty("bounds", alignCenterToPoint(textButtonRect, {centerX + buttonWidth + halfButtonWidth, buttonsY1}));
  _view->_widgets["export"]->setRectProperty("bounds", alignCenterToPoint(textButtonRect, {centerX + buttonWidth*2 + halfButtonWidth, buttonsY1}));

  _view->_widgets["play"]->setRectProperty("bounds", alignCenterToPoint(textButtonRect, {centerX + buttonWidth, buttonsY2}));
  _view->_widgets["synthesize"]->setRectProperty("bounds", alignCenterToPoint(textButtonRect, {centerX + buttonWidth + buttonWidth, buttonsY2}));

  // info
  _view->_widgets["info"]->setRectProperty("bounds", ml::Rect(0, bottomY, gx, 1));
  
  // partials
  _view->_widgets["partials"]->setRectProperty("bounds", ml::Rect(0, 0, gx, bottomY));
}

UtuViewView::~UtuViewView ()
{
  _ioTimer.stop();
  Actor::stop();
  removeActor(this);
}

int UtuViewView::getElapsedTime()
{
  // return time elapsed since last render
  time_point<system_clock> now = system_clock::now();
  auto elapsedTime = duration_cast<milliseconds>(now - _previousFrameTime).count();
  _previousFrameTime = now;
  return elapsedTime;
}


#pragma mark from ml::AppView

void UtuViewView::initializeResources(NativeDrawContext* nvg)
{
  // initialize drawing properties before controls are made
  _drawingProperties.setProperty("mark", colorToMatrix({0.01, 0.01, 0.01, 1.0}));
  _drawingProperties.setProperty("background", colorToMatrix({0.8, 0.8, 0.8, 1.0}));
  _drawingProperties.setProperty("draw_background_grid", true);
  

  if (nvg)
  {
    // fonts
    int font1 = nvgCreateFontMem(nvg, "MLVG_sans", (unsigned char*)resources::D_DIN_otf, resources::D_DIN_otf_size, 0);
    const unsigned char* pFont1 = reinterpret_cast<const unsigned char *>(&font1);
    _resources["d_din"] = ml::make_unique< Resource >(pFont1, pFont1 + sizeof(int));

    int font2 = nvgCreateFontMem(nvg, "MLVG_italic", (unsigned char *)resources::D_DIN_Italic_otf, resources::D_DIN_Italic_otf_size, 0);
    const unsigned char* pFont2 = reinterpret_cast<const unsigned char *>(&font2);
    _resources["d_din_oblique"] = ml::make_unique< Resource >(pFont2, pFont2 + sizeof(int));
    
    // raster images
    int flags = 0;
    int img1 = nvgCreateImageMem(nvg, flags, (unsigned char *)resources::vignette_jpg, resources::vignette_jpg_size);
    const unsigned char* pImg1 = reinterpret_cast<const unsigned char *>(&img1);
    //_resources["background"] = ml::make_unique< Resource >(pImg1, pImg1 + sizeof(int));
    
    // SVG images    
    ml::AppView::createVectorImage("tesseract", resources::Tesseract_Mark_svg, resources::Tesseract_Mark_svg_size);
  }
}

void UtuViewView::_initializeParams()
{
  // read parameter descriptions from processorParameters.h
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);
  
  // TODO can we share the Controller's parameter tree?
  buildParameterTree(pdl, _params);
  
  // build index of widgets by parameter.
  // for each parameter, collect Widgets responding to it
  // and add the parameter to the Widget's param tree.
  for(auto& paramDesc : pdl)
  {
    Path paramName = paramDesc->getTextProperty("name");
    
    forEach< Widget >
    (_view->_widgets, [&](Widget& w)
     {
      if(w.knowsParam(paramName))
      {
        _widgetsByParameter[paramName].push_back(&w);
        w.setParameterDescription(paramName, *paramDesc);
      }
    }
     );
  }
  
  // build index of any widgets that refer to collections.
  forEach< Widget >
  (_view->_widgets, [&](Widget& w)
   {
    if(w.hasProperty("collection"))
    {
      const Path collName(w.getTextProperty("collection"));
      _widgetsByCollection[collName].push_back(&w);
    }
  });
  
  // build index of any widgets that need signals, and subscribe to those signals.
  for(auto& w : _view->_widgets)
  {
    if(w->hasProperty("signal_name"))
    {
      const Path sigName(w->getTextProperty("signal_name"));
      _widgetsBySignal[sigName].push_back(w.get());

      // message the controller to subscribe to the signal.
      sendMessageToActor(_controllerName, Message{"do/subscribe_to_signal", pathToText(sigName)});
    }
  }
  
  // give each Widget a chance to do setup now: after it has its
  // parameter description(s) and before it is animated or drawn.
  for(auto& w : _view->_widgets)
  {
    w->setupParams();
  }
}


void UtuViewView::_sendParameterToWidgets(const Message& msg)
{
  if(msg.value)
  {
    // get param name
    Path pname = tail(msg.address);
    
    // send to Widgets that care about it
    for(auto pw : _widgetsByParameter[pname])
    {
      // if Widget is not engaged, send it the new value.
      if(!pw->engaged)
      {
        sendMessage(*pw, msg);
      }
    }
  }
}

void UtuViewView::debug()
{
  //std::cout << "UtuViewView: " << getMessagesAvailable() << " messages in queue. max: "
  //  << _maxQueueSize << " handled: " << _msgCounter << " \n";
  //_msgCounter = 0;
}


// Actor implementation

void UtuViewView::onMessage(Message msg)
{
  // std::cout << "UtuViewView: handleMessage: " << msg.address << " : " << msg.value << "\n";
  
  if(head(msg.address) == "editor")
  {
    // we are the editor, so remove "editor" and handle message
    msg.address = tail(msg.address);
  }
  
  switch(hash(head(msg.address)))
  {
    case(hash("set_param")):
    {
      if(msg.address.getSize() > 4)
      {
        std::cout << "huh?\n";
      }
      Path a = tail(msg.address);
      Symbol b = head(a);
      uint32_t c = hash(b);
      
      switch(c)//(hash(head(tail(msg.address))))
      {
        case(hash("status")):
        case(hash("licensor")):
        {
          // current_patch can go both ways: sent from Widgets to
          // Controller, or Controller to widgets.
          
          // if the param change message is not from the controller,
          // forward it to the controller.
          if(!(msg.flags & kMsgFromController))
          {
            sendMessageToActor(_controllerName, msg);
          }
          
          // broadcast param to Widgets. If the message we are handling comes from
          // a Widget, that Widget is responsible for ignoring echoes
          // back to itself.
          _sendParameterToWidgets(msg);
          break;
        }
          
        default:
        {
          // no local parameter was found, set a plugin parameter
          
          // store param value in local tree.
          Path paramName = tail(msg.address);
          if(!paramName)
          {
            std::cout << "setting null param! \n";
          }
          
          _params.setParamFromNormalizedValue(paramName, msg.value);
          
          // if the parameter change message is not from the controller,
          // forward it to the controller.
          if(!(msg.flags & kMsgFromController))
          {
            sendMessageToActor(_controllerName, msg);
          }
          
          // if the message comes from a Widget, we do send the parameter back
          // to other Widgets so they can synchronize. It's up to individual
          // Widgets to filter out duplicate values.
          _sendParameterToWidgets(msg);
          break;
        }
          break;
      }
      break;
    }
      
    // this special verb for the editor receives signals on the way to the view.
    // a message will be sent for each channel of a multi-channel signal.
    case(hash("set_signal")):
    {
      auto signalName = butLast(tail(msg.address));
      
      if(_widgetsBySignal.getNode(signalName))
      {
        // get a DSPVector of signal from the message
        // TODO helpers
        auto blob = msg.value.getBlobValue();
        const float* pVectorData = reinterpret_cast<const float*>(blob._data);
        DSPVector signalVector(pVectorData);
        auto signalChannel = textUtils::textToNaturalNumber(last(msg.address).getTextFragment());

        for(auto& w : _widgetsBySignal[signalName])
        {
          w->processSignal(signalVector, signalChannel);
        }
      }
    }
      
  // TODO should editorview have a property tree for default behavior properties?
      
    case(hash("do")):
    {
      // do something!
      msg.address = tail(msg.address);
      switch(hash(head(msg.address)))
      {
          /*
        // TODO should simply be set view_size message? overload that simple param set
        case(hash("resize")):
        {
          // sent by resizer component - resize and tell Controller
          auto newSize = msg.value.getMatrixValue();
          doResize(_constrainSize(matrixToVec2(newSize)));
          sendMessageToActor(_controllerName, {"set_param/view_size", newSize});
          break;
        }
          */
          /*
        case(hash("update_collection")):
        {
          // a Widget or the Controller is requesting a collection be updated.
          Path collName (msg.value.getTextValue());
          
          // update collection and get a pointer to it from Controller
          // TODO we don't want to use the controller reference, this is the only place we need it
          // do this with messaging instead
          if(_controller)
          {
            FileTree* pTree = _controller->updateCollection(collName);
            
            // for each widget that references the collection, send the
            // collection pointer and parameter info and give the widget a chance
            // to setup any indexes
            for(auto pw : _widgetsByCollection[collName])
            {
              pw->receiveNamedRawPointer("file_tree", pTree);
              pw->setupParams();
            }
          }
          break;
        }*/
          
          /*
        case(hash("display_version")):
        {
          TextFragment nameAndVersion (getAppName(), " ", getPluginVersion());
          TextFragment wrapperAndArch(getPluginWrapperType(), ", ", getPluginArchitecture());
          TextFragment waa (" (", wrapperAndArch, ")");
          TextFragment info(nameAndVersion, waa);
          _popupPropertiesBuffer["message"] = info;
          break;
        }*/
          
        default:
        {
          break;
        }
      }
      break;
    }
    default:
    {
      // try to forward the message to another receiver
      switch(hash(head(msg.address)))
      {
        case(hash("controller")):
        {
          msg.address = tail(msg.address);
          sendMessageToActor(_controllerName, msg);
          break;
        }
        default:
        {
          // uncaught
          break;
        }
      }
      break;
    }
  }
}





