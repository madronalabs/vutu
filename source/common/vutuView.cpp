// mlvg: GUI library for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "vutuView.h"

#include "madronalib.h"
#include "manzanita.h"

#include "MLParameters.h"
#include "MLSerialization.h"

#include "vutuPartialsDisplay.h"
#include "sampleDisplay.h"

#include "vutuProcessor.h"

#include "../build/resources/vutu/resources.c"

#include "SDL.h"
#include "SDL_syswm.h"

using namespace ml;

static int targetFPS{ 60 };

ml::Rect smallDialRect{0, 0, 1.0, 1.0};
ml::Rect mediumDialRect{0, 0, 2.0, 1.5};
ml::Rect largeDialRect{0, 0, 3, 2};
float mediumDialSize{0.625f};
float largeDialSize{0.875f};
ml::Rect labelRect(0, 0, 3, 1.0);

VutuView::VutuView() : AppView()
{
}

VutuView::~VutuView ()
{
  if(window)
  {
    SDL_DestroyWindow(window);
  }
  SDL_Quit();
}

#pragma mark windowing

bool VutuView::createWindow(ml::Rect defaultSize)
{
  int windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;

  window = ml::newSDLWindow(defaultSize, "vutu", windowFlags);
  if(!window)
  {
    std::cout << "newSDLWindow failed!\n";
    return false;
  }

  // this is not a fixed-ratio app: the window sizes freely and the grid unit
  // size remains constant.
  setGridSizeDefault(kDefaultGridUnitSize);

  ParentWindowInfo windowInfo = ml::getParentWindowInfo(window);
  platformView = std::make_unique< PlatformView >("vutu", windowInfo.windowPtr, this, nullptr, windowInfo.flags, targetFPS, windowInfo.sdlWindow);

  // watch for window resize events during drag
  watcherData = ResizingEventWatcherData{ window, platformView.get() };
  SDL_AddEventWatch(resizingEventWatcher, &watcherData);

  return true;
}

void VutuView::attachPlatformViewToParent()
{
  platformView->attachViewToParent();
}

void VutuView::initializePlatformResources()
{
  initializeResources(platformView->getNativeDrawContext());
}

void VutuView::runAppLoop()
{
  bool doneFlag{ false };
  while (!doneFlag)
  {
    SDLAppLoop(window, &doneFlag);
  }
}

void VutuView::stop()
{
  stopTimers();
  clearResources();
}

void VutuView::requestResize(Vec2 newSize)
{
  if (window)
  {
    SDL_SetWindowSize(window, newSize[0], newSize[1]);
  }
}

void VutuView::onResize(Vec2 newSize)
{
  setDirty(true);
}

#pragma mark from ml::AppView

void VutuView::layoutView(DrawContext dc)
{
  // this app sizes freely, so derive the grid dimensions from the current
  // view size rather than a fixed grid-units property.
  Vec2 pixelSize = dc.coords.viewSizeInPixels;
  float gridSize = dc.coords.gridSizeInPixels;
  int gx = pixelSize.x() / gridSize;
  int gy = pixelSize.y() / gridSize;

  // set grid size of entire view, for background and other drawing
  _view->setProperty("grid_units_x", gx);
  _view->setProperty("grid_units_y", gy);
  
  if(!_view->_widgets.size()) return;
  
  const int bottomHeight{7};
  int bottomY = gy - bottomHeight;
  float dialsY1 = bottomY + 2;
  float dialsY2 = bottomY + 5;
  
  // left dials
  _view->_widgets["resolution"]->setBounds(alignCenterToPoint(largeDialRect, {2.0, dialsY1}));
  _view->_widgets["amp_floor"]->setBounds(alignCenterToPoint(largeDialRect, {3.5, dialsY2}));
  _view->_widgets["window_width"]->setBounds(alignCenterToPoint(largeDialRect, {5.0, dialsY1}));
  
  _view->_widgets["lo_cut"]->setBounds(alignCenterToPoint(largeDialRect, {6.5, dialsY2}));
  _view->_widgets["hi_cut"]->setBounds(alignCenterToPoint(largeDialRect, {8, dialsY1}));
  
  _view->_widgets["freq_drift"]->setBounds(alignCenterToPoint(largeDialRect, {9.5, dialsY2}));
  _view->_widgets["noise_width"]->setBounds(alignCenterToPoint(largeDialRect, {11, dialsY1}));
  
  // right dials
  _view->_widgets["fundamental"]->setBounds(alignCenterToPoint(mediumDialRect, {gx - 2.f, bottomY + 1.5f}));
  _view->_widgets["test_volume"]->setBounds(alignCenterToPoint(mediumDialRect, {gx - 2.f, bottomY + 3.5f}));
  _view->_widgets["output_volume"]->setBounds(alignCenterToPoint(mediumDialRect, {gx - 2.f, bottomY + 5.5f}));

  // dial labels
  auto positionLabelUnderDial = [&](Path dialName)
  {
    Path labelName (TextFragment(pathToText(dialName), "_label"));
    ml::Rect dialRect = _view->_widgets[dialName]->getBounds();
    _view->_backgroundWidgets[labelName]->setBounds
    (alignTopCenterToPoint(labelRect, dialRect.bottomCenter() - Vec2(0, 0.5)));
  };
  for(auto dialName : {"resolution", "window_width", "amp_floor", "lo_cut", "hi_cut", "noise_width", "freq_drift", "fundamental", "test_volume", "output_volume"})
  {
    positionLabelUnderDial(dialName);
  }
  
  // auto-mode toggle + max-active chooser, in the gap between the analysis
  // dials and the button grid.
  float controlsX = 13.75f;
  ml::Rect toggleRect(0, 0, 0.6, 0.6);
  ml::Rect chooserRect(0, 0, 1.8, 1.8);
  _view->_widgets["auto_mode"]->setBounds(alignCenterToPoint(toggleRect, {controlsX, dialsY1 - 0.5f}));
  _view->_widgets["max_active"]->setBounds(alignCenterToPoint(chooserRect, {controlsX, dialsY2 - 0.25f}));
  _view->_backgroundWidgets["auto_mode_label"]->setBounds(alignTopCenterToPoint(labelRect, {controlsX, dialsY1 + 0.0f}));
  _view->_backgroundWidgets["max_active_label"]->setBounds(alignTopCenterToPoint(labelRect, {controlsX, dialsY2 - 1.6f}));

  // info: whole width
  _view->_widgets["info"]->setBounds(ml::Rect(0, bottomY, gx, 1));
  
  // audio display widgets
  int bigWidth = gx;
  _view->_widgets["source"]->setBounds(ml::Rect(0, 0, bigWidth, 2));
  _view->_widgets["partials"]->setBounds(ml::Rect(0, 2, bigWidth, bottomY - 4));
  _view->_widgets["synth"]->setBounds(ml::Rect(0, bottomY - 2, bigWidth, 2));

  // buttons
  int centerX = gx/2;
  float buttonWidth = 4;
  float halfButtonWidth = buttonWidth/2.f;

  ml::Rect textButtonRect(0, 0, buttonWidth, 1);

  
  float buttonsY1 = bottomY + 2.5;
  float buttonsY2 = bottomY + 4.0;
  float buttonsY3 = bottomY + 5.5;
  
  float buttonsR = gx - 4 - halfButtonWidth;
  float buttonsX1 = gx - 4 - halfButtonWidth - buttonWidth*2;
  float buttonsX2 = gx - 4 - halfButtonWidth - buttonWidth*1;
  float buttonsX3 = gx - 4 - halfButtonWidth - buttonWidth*0;
  
  _view->_widgets["open"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX1, buttonsY1}));
  _view->_widgets["analyze"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX2, buttonsY1}));
  _view->_widgets["play_source"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX3, buttonsY1}));
    
  _view->_widgets["import"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX1, buttonsY2}));
  _view->_widgets["synthesize"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX2, buttonsY2}));
  _view->_widgets["export"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX3, buttonsY2}));

  _view->_widgets["play_synth"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX1, buttonsY3}));
  _view->_widgets["export_synth"]->setBounds(alignCenterToPoint(textButtonRect, {buttonsX2, buttonsY3}));
  
  // other labels
  ml::Rect otherLabelsRect(0, 0, 2, 1);
  float labelsR = buttonsX1 - halfButtonWidth -  0.25;
  _view->_backgroundWidgets["source_label"]->setBounds(alignMiddleRightToPoint(otherLabelsRect, {labelsR, buttonsY1}));
  _view->_backgroundWidgets["partials_label"]->setBounds(alignMiddleRightToPoint(otherLabelsRect, {labelsR, buttonsY2}));
  _view->_backgroundWidgets["resynth_label"]->setBounds(alignMiddleRightToPoint(otherLabelsRect, {labelsR, buttonsY3}));

  
  
  // resize all widgets
  forEach< Widget >
  (_view->_widgets, [&](Widget& w)
   {
    w.resize(dc);
  }
   );
}

void VutuView::initializeResources(NativeDrawContext* nvg)
{
  if (!nvg) return;

  // initialize drawing properties before controls are made
  _drawingProperties.setProperty("mark", {0.01, 1.00, 0.01, 1.0});
  _drawingProperties.setProperty("background", {0.01, 0.01, 0.01, 1.0});
  _drawingProperties.setProperty("panel_bg", {0.01, 0.01, 0.01, 1.0});
  _drawingProperties.setProperty("common_stroke_width", 1/24.f);
  _drawingProperties.setProperty("partials", {0.01, 1.00, 0.01, 1.0});

  // DEBUG
  _drawingProperties.setProperty("draw_widget_bounds", false);
  _drawingProperties.setProperty("draw_widget_outlines", false);

  // fonts
  _resources.fonts["d_din"] = std::make_unique< FontResource >(nvg, "MZ_sans", resources::D_DIN_otf, resources::D_DIN_otf_size);
  _resources.fonts["d_din_oblique"] = std::make_unique< FontResource >(nvg, "MZ_italic", resources::D_DIN_Italic_otf, resources::D_DIN_Italic_otf_size);
}

void VutuView::clearResources()
{
  // release widget-owned framebuffers (backing layers) while the draw context
  // is still alive, before clearing our own resources.
  clearWidgetResources();

  _resources.fonts.clear();
  _resources.rasterImages.clear();
  _resources.vectorImages.clear();
  _resources.drawableImages.clear();
}


void VutuView::makeWidgets()
{
  // add labels to background
  auto addControlLabel = [&](Path name, TextFragment t)
  {
    _view->_backgroundWidgets.add_unique< TextLabelBasic >(name, WithValues{
      { "h_align", "center" },
      { "v_align", "middle" },
      { "text", t },
      { "font", "d_din_oblique" },
      { "text_size", 0.50 },
      { "text_spacing", 0.0f }
    } );
  };
  addControlLabel("resolution_label", "resolution");
  addControlLabel("window_width_label", "window freq.");
  addControlLabel("amp_floor_label", "amp floor");
  addControlLabel("freq_drift_label", "freq. drift");
  addControlLabel("lo_cut_label", "lo cut");
  addControlLabel("hi_cut_label", "hi cut");
  addControlLabel("noise_width_label", "noise width");
  addControlLabel("fundamental_label", "fundamental");
  addControlLabel("test_volume_label", "fund. volume");
  addControlLabel("output_volume_label", "output volume");
  addControlLabel("auto_mode_label", "auto");
  addControlLabel("max_active_label", "max active");

  auto addOtherLabel = [&](Path name, TextFragment t)
  {
    _view->_backgroundWidgets.add_unique< TextLabelBasic >(name, WithValues{
      { "h_align", "right" },
      { "v_align", "middle" },
      { "text", t },
      { "font", "d_din_oblique" },
      { "text_size", 0.50 },
      { "text_spacing", 0.0f }
    } );
  };
  addOtherLabel("source_label", "source");
  addOtherLabel("partials_label", "partials");
  addOtherLabel("resynth_label", "resynth");

  
  // dials
  _view->_widgets.add_unique< DialBasic >("resolution", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "resolution" }
  } );
  _view->_widgets.add_unique< DialBasic >("window_width", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "window_width" }
  } );
  _view->_widgets.add_unique< DialBasic >("amp_floor", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "amp_floor" }
  } );
  
  _view->_widgets.add_unique< DialBasic >("freq_drift", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "freq_drift" }
  } );
   
  _view->_widgets.add_unique< DialBasic >("lo_cut", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "lo_cut" }
  } );
  
  _view->_widgets.add_unique< DialBasic >("hi_cut", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "hi_cut" }
  } );
  
  _view->_widgets.add_unique< DialBasic >("noise_width", WithValues{
    {"size", largeDialSize },
    {"feature_scale", 2.0 },
    {"param", "noise_width" }
  } );
  
  _view->_widgets.add_unique< DialBasic >("fundamental", WithValues{
    {"size", mediumDialSize },
    {"feature_scale", 2.0 },
    {"fine_drag_scale", 0.005f },
    {"param", "fundamental" }
  } );
  
  _view->_widgets.add_unique< DialBasic >("test_volume", WithValues{
    {"size", mediumDialSize },
    {"feature_scale", 2.0 },
    {"param", "test_volume" }
  } );
  _view->_widgets.add_unique< DialBasic >("output_volume", WithValues{
    {"size", mediumDialSize },
    {"feature_scale", 2.0 },
    {"param", "output_volume" }
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
  _view->_widgets.add_unique< TextButtonBasic >("play_source", WithValues{
    {"text", "play" },
    {"action", "toggle_play_source" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("synthesize", WithValues{
    {"text", "synthesize" },
    {"action", "synthesize" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("import", WithValues{
    {"text", "import" },
    {"action", "import" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("export", WithValues{
    {"text", "export .utu" },
    {"action", "export" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("play_synth", WithValues{
    {"text", "play" },
    {"action", "toggle_play_synth" }
  } );
  _view->_widgets.add_unique< TextButtonBasic >("export_synth", WithValues{
    {"text", "export .wav" },
    {"action", "export_synth" }
  } );

  // info label
  _view->_widgets.add_unique< TextLabelBasic >("info", WithValues{
    { "h_align", "center" },
    { "v_align", "middle" },
    { "text", "vutu" },
    { "font", "d_din_oblique" },
    { "text_size", 0.5 },
    { "text_spacing", 0.0f }
  } );
  
  // source
  _view->_widgets.add_unique< SampleDisplay >("source", WithValues{
    {"enable_interval", true },
    {"param", "analysis_interval" }
  } );
  
  // partials
  // note: see knowsParam() override where the widget requests multiple parameters.
  _view->_widgets.add_unique< VutuPartialsDisplay >("partials", WithValues{    
    {"param", "analysis_interval" }
  } );
  
  // synth (synthesized sample)
  _view->_widgets.add_unique< SampleDisplay >("synth", WithValues{
    //{"param", "analysis_interval" }
  } );

  // auto / manual analysis mode toggle
  _view->_widgets.add_unique< ToggleButtonBasic >("auto_mode", WithValues{
    {"param", "auto_mode" }
  } );

  // max active partials chooser (16..512)
  _view->_widgets.add_unique< ChooserBasic >("max_active", WithValues{
    {"param", "max_active" },
    {"font", "d_din" },
    {"text_size", 0.35f }
  } );


  // play buttons disabled until we have a sample
  _view->_widgets["play_source"]->setProperty("enabled", false);
  _view->_widgets["analyze"]->setProperty("enabled", false);
  _view->_widgets["export"]->setProperty("enabled", false);
  _view->_widgets["play_synth"]->setProperty("enabled", false);
  _view->_widgets["export_synth"]->setProperty("enabled", false);
}

// after Widgets are made and parameters connected, make everything visible.
void VutuView::prepareToDraw()
{
  showAllWidgets();
}


/*
void VutuView::debug()
{
  //std::cout << "VutuView: " << getMessagesAvailable() << " messages in queue. max: "
  //  << _maxQueueSize << " handled: " << _msgCounter << " \n";
  //_msgCounter = 0;
}
*/

// handle messages arriving from the controller (via the view message handler)
// and from our own Widgets (via processGUIEvent). Both flow through here.
void VutuView::onMessage(Message msg)
{
  switch(hash(head(msg.address)))
  {
    case(hash("set_param")):
    {
      // let the base AppView update the bound Widgets and forward
      // widget-originated changes on to the processor (the controller).
      AppView::onMessage(msg);
      break;
    }
    case(hash("do")):
    {
      switch(hash(second(msg.address)))
      {
        case(hash("set_source_data")):
        {
          Sample* pSample = *reinterpret_cast<Sample* const*>(msg.value.data());
          if(auto* w = dynamic_cast< SampleDisplay* >(_view->_widgets["source"].get()))
            w->receiveSample(pSample);
          break;
        }

        case(hash("set_partials_data")):
        {
          VutuPartialsData* pPartials = *reinterpret_cast<VutuPartialsData* const*>(msg.value.data());
          if(auto* w = dynamic_cast< VutuPartialsDisplay* >(_view->_widgets["partials"].get()))
            w->receivePartials(pPartials);
          break;
        }

        case(hash("set_synth_data")):
        {
          Sample* pSample = *reinterpret_cast<Sample* const*>(msg.value.data());
          if(auto* w = dynamic_cast< SampleDisplay* >(_view->_widgets["synth"].get()))
            w->receiveSample(pSample);
          break;
        }

        case(hash("set_source_duration")):
        {
          if(const auto& w = _view->_widgets["source"])
          {
            w->handleMessage(Message{"set_prop/interval_start", 0}, nullptr);
            w->handleMessage(Message{"set_prop/interval_end", msg.value.getFloatValue()}, nullptr);
          }
          break;
        }

        default:
        {
          // a button action (do/open, do/analyze, ...) originating from a
          // Widget: forward it to the processor (the controller).
          if(!(msg.flags & kMsgFromProcessor) && sendMessageToProcessor)
          {
            sendMessageToProcessor(msg);
          }
          break;
        }
      }
      break;
    }
    case(hash("info")):
    {
      msg.address = tail(msg.address);
      if(const auto& w = _view->_widgets["info"]) w->handleMessage(msg, nullptr);
      break;
    }
    case(hash("widget")):
    {
      msg.address = tail(msg.address);
      auto widgetName = head(msg.address);
      msg.address = tail(msg.address);
      if(const auto& w = _view->_widgets[Path(widgetName.getTextFragment())]) w->handleMessage(msg, nullptr);
      break;
    }
    default:
    {
      // uncaught
      break;
    }
  }
}





