

#include "sampleDisplay.h"

using namespace ml;

constexpr float kMarginSize{1/8.};

void SampleDisplay::resize(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);

  // constant background layer size, for now.
  int w = 2000;
  int h = 200;
  int bw = 0;
  int bh = 0;
  
  // create new backing layer if needed
  if(_backingLayer.get())
  {
    bw = _backingLayer->width;
    bh = _backingLayer->height;
  }
  
  // if we do add a variable background layer size, this will repaint it when it changes.
  if((w != bw) || (h != bh))
  {
    std::cout << "SampleDisplay::resize: " << w << " x " << h << "\n";
    _backingLayer = ml::make_unique< Layer >(nvg, w, h);
    sampleDirty_ = true;
  }
}

template<class T>
bool updateState(T& currentState, T newValue)
{
  T oldValue = currentState;
  currentState = newValue;
  return(oldValue != newValue);
}

MessageList SampleDisplay::animate(int elapsedTimeInMs, ml::DrawContext dc)
{
  Path pname{getTextProperty("param")};

  // we can paint into the backing layer in animate()
  if(sampleDirty_)
  {
    paintSample(dc);
    sampleDirty_ = false;
    _dirty = true;
  }

  float t = getFloatPropertyWithDefault("playback_time", 0.f);
  if(t != _playbackTime)
  {
    _playbackTime = t;
    _dirty = true;
  }
  
  return MessageList{};
}

Rect SampleDisplay::getControlRect(int cIdx)
{
  auto currentValue = getParamValue(getTextProperty("param")).getIntervalValue();

  Rect r;
  Rect bounds = getBounds();
  auto unityToX = projections::linear({0, 1}, {0.f, bounds.width()});

  switch(cIdx)
  {
    case leftControl:
    {
      float x1 = unityToX(currentValue.mX1);
      r = Rect{x1, 0, kBracketWidth, bounds.height()};
      break;
    }
    case rightControl:
    {
      float x2 = unityToX(currentValue.mX2) - kBracketWidth;
      r = Rect{x2, 0, kBracketWidth, bounds.height()};
      break;
    }
    case none:
    {
      r = Rect{};
      break;
    }
  }
  
  return r;
}

MessageList SampleDisplay::processGUIEvent(const GUICoordinates& gc, GUIEvent e)
{
  MessageList r{};
  if(!getBoolPropertyWithDefault("enabled", true)) return r;
  
  // if we're not editing an interval, there is no UI interaction
  if(!getBoolPropertyWithDefault("enable_interval", false)) return r;
  
  if(!_pSample) return r;

  constexpr float kScrollScale{-0.04f};
  constexpr float kFineDragScale{0.1f}; // TODO different fine drag behavior based on parameter values
  constexpr float kMaxGrabDist{2.f};
  
  Path pname{getTextProperty("param")};
  Path paramRequestPath = Path("editor/set_param", pname);
  
  // use top left relative coords
  Rect bounds = getBounds();
  Vec2 gridPosition = e.position - bounds.topLeft();
  bool doFineDrag = e.keyFlags & shiftModifier;
  
  Interval prevValue = getParamValue(pname).getIntervalValue();
  Interval newValue = prevValue;
  
  switch(hash(e.type))
  {
    case(hash("down")):
    {
      // get x distance to each control
      float gx = gridPosition.x();
      float dLeft = fabs(gx - getCenter(getControlRect(0)).x());
      float dRight = fabs(gx - getCenter(getControlRect(1)).x());
      
      // get min of distances
      float minDist = min(dLeft, dRight);
      
      // if min distance < k, start dragging the closest control
      if(minDist < kMaxGrabDist)
      {
        currentDragControl_ = (dLeft < dRight) ? leftControl : rightControl;
      }
      
      // always push a sequence start message
      if(currentDragControl_ != none)
      {
        r.push_back(Message{paramRequestPath, prevValue, kMsgSequenceStart});
        engaged = true;
        _dragX1 = gridPosition.x();
      }
      
      // redraw for brackets
      setDirty(true);
      break;
    }
    case(hash("drag")):
    {
      // change value
      float dragX0 = gridPosition.x();
      float delta = dragX0 - _dragX1;
      _dragX1 = dragX0;
      
      if(delta != 0.f && (currentDragControl_ != none))
      {
        // clamp results of drag, in normalized coordinates
        auto xToUnity = projections::linear({0, bounds.width()}, {0.f, 1.f});
        float marginU = xToUnity(kBracketWidth);

        delta = xToUnity(delta);
        if(doFineDrag) delta *= kFineDragScale;
        
        Rect controlRect = getControlRect(currentDragControl_);
        float crw = controlRect.width();
        float crwu = 0;// TEST xToUnity(crw);
        float newLeftX = newValue.mX1;
        float newRightX = newValue.mX2;
        
        // clip dragging control and bump other control if needed
        if(currentDragControl_ == leftControl)
        {
          newLeftX += delta;
          newValue.mX1 = newLeftX;
          
          if(newLeftX > newRightX - marginU*2)
          {
            newRightX = newLeftX + marginU*2;
          }
        }
        else if(currentDragControl_ == rightControl)
        {
          newRightX += delta;
          newValue.mX2 = newRightX;
          
          if(newRightX < newLeftX + marginU*2)
          {
            newLeftX = newRightX - marginU*2;
          }
        }
        newLeftX = clamp(newLeftX, 0.f, 1.0f - marginU*2);
        newRightX = clamp(newRightX, marginU*2, 1.0f);
        newValue.mX1 = newLeftX;
        newValue.mX2 = newRightX;
      }
      if(newValue != prevValue)
      {
        // if value changed, set value and mark the Widget dirty
        setParamValue(pname, newValue);
        r.push_back(Message{paramRequestPath, newValue, 0});
      }
      break;
    }
    case(hash("up")):
    {
      // if engaged, disengage and send a sequence end message
      if(engaged && (currentDragControl_ != none))
      {
        r.push_back(Message{paramRequestPath, newValue, kMsgSequenceEnd});
        engaged = false;
        currentDragControl_ = none;
      }
      break;
    }
    default:
      break;
  }
  return r;
}

void SampleDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("sample")):
      _pSample = static_cast< const ml::Sample* > (ptr);
      sampleDirty_ = true;
      break;
    default:
      break;
  }
}

// Repaint the backing layer with an image of the entire sample.
// Since we draw to a backing layer here, this must be called only
// from animate(), not from draw().
//
bool SampleDisplay::paintSample(ml::DrawContext dc)
{
  bool r{false};
  NativeDrawContext* nvg = getNativeContext(dc);
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;
  
  auto frameBuffer = _backingLayer.get();
  if(!_backingLayer)
  {
    std::cout << "no backing layer~!\n";
    return false;
  }
  
  int w = _backingLayer->width;
  int h = _backingLayer->height;
  
  // begin rendering to backing layer
  drawToLayer(_backingLayer.get());
  nvgBeginFrame(nvg, w, h, 1.0f);
  
  auto color = getColor(dc, "partials");

  // draw opaque black bg
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, ml::Rect{0.f, 0.f, float(_backingLayer->width), float(_backingLayer->height)});
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
  }
  
  if(usable(_pSample))
  {
    size_t frames = getFrames(*_pSample);
    size_t sr = _pSample->sampleRate;
    
    Interval frameInterval{0, float(frames - 1)};
    Interval xRange{0, w - 1.f};
    Interval yRange{h - 1.f, 0.f};
    float yCenter{(h - 1.f)/2.f};
    
    constexpr float kMinLineLength{2.f};
    auto xToFrame = projections::linear(xRange, frameInterval);
    
    Interval ampRange = {0.f, 1.f};
    
    Interval thicknessRange{0, h/2.f};
    auto ampToThickness = projections::linear(ampRange, thicknessRange);
    
    // time the partials bit
    auto roughStart = high_resolution_clock::now();
    size_t totalFramesRead{0};
    size_t totalFramesDrawn{0};
    
    nvgStrokeColor(nvg, color);
    nvgStrokeWidth(nvg, 1.0f);
    nvgBeginPath(nvg);
    
    for(int x=0; x<w; ++x)
    {
      int frame = clamp(size_t(xToFrame(x)), size_t(0), frames);
      float amp = (*_pSample)[frame];
      float thickness = ampToThickness(amp);
      nvgMoveTo(nvg, x, yCenter - thickness);
      nvgLineTo(nvg, x, yCenter + thickness);
    }
    nvgStroke(nvg);

    auto roughEnd = high_resolution_clock::now();
    auto roughMillisTotal = duration_cast<milliseconds>(roughEnd - roughStart).count();
    
    std::cout << " sample painting time rough millis: " << roughMillisTotal << "\n";
    std::cout << frames << " frames \n";
    r = true;
  }
  
  // end backing layer update
  nvgEndFrame(nvg);
  return r;
}
 

void SampleDisplay::draw(ml::DrawContext dc)
{
  // get parameter value
  Path paramName{getTextProperty("param")};
  auto currentValue = getParamValue(paramName).getIntervalValue();
  
  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;

  int margin = gridSizeInPixels*kMarginSize;
  Rect marginBounds = shrink(bounds, margin);
  Interval xRange{marginBounds.left(), marginBounds.right()};

  auto unityToX = projections::linear({0, 1}, xRange);

  float strokeWidthMul = getFloatPropertyWithDefault("stroke_width", getFloat(dc, "common_stroke_width"));
  int strokeWidth = gridSizeInPixels*strokeWidthMul;
  
  auto bgColor = getColorPropertyWithDefault("color", getColor(dc, "panel_bg"));
  auto markColor = getColor(dc, "partials");

  // paint background
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, marginBounds);
    nvgFillColor(nvg, bgColor);
    nvgFill(nvg);
  }
  
  if(!_backingLayer) return;
  
  if(usable(_pSample))
  {
    auto sampleDuration = getDuration(*_pSample);
    auto timeToX = projections::linear({0, sampleDuration}, xRange);
    size_t sr = _pSample->sampleRate;
    Interval frameInterval = currentValue*sr;

    auto nativeImage = getNativeImageHandle(*_backingLayer);
    
    // make an image pattern. The entire source image maps to the specified rect of the destination.
    NVGpaint img = nvgImagePattern(nvg, margin*2, margin*2, w - margin*4, h - margin*4, 0, nativeImage, 1.0f);
    
    // draw image
    {
      nvgSave(nvg);
      nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
      
      // paint image lighten over bg
      nvgBeginPath(nvg);
      nvgRect(nvg, margin*2, margin*2, w - margin*4, h - margin*4);
      nvgFillPaint(nvg, img);
      nvgFill(nvg);
      nvgRestore(nvg);
    }
    
    // draw progress
    if(_playbackTime > 0.f)
    {
      float px = timeToX(_playbackTime);
      nvgStrokeWidth(nvg, strokeWidth);
      nvgBeginPath(nvg);
      nvgStrokeColor(nvg, markColor);
      nvgMoveTo(nvg, px, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.bottom());
      nvgStroke(nvg);
    }
    
    // draw interval
    if(getBoolPropertyWithDefault("enable_interval", false))
    {
      nvgStrokeColor(nvg, markColor);
      nvgStrokeWidth(nvg, strokeWidth*2);
      
      Rect loopRect(unityToX(currentValue.mX1), 0, unityToX(currentValue.mX2 - currentValue.mX1), h);
      loopRect = shrinkHeight(loopRect, margin*2);
      
      // tweak, shouldn't be needed
      loopRect.left() += margin;
      loopRect.width() -= margin*3;
      
      if (currentDragControl_ != none)
      {
        loopRect = shrinkHeight(loopRect, margin);
      }
      int bracketGlyphWidth = gridSizeInPixels*kBracketWidth;
      drawBrackets(nvg, loopRect, bracketGlyphWidth);
    }
    
    else // TEMP
    {
        
    }
    
  }
    
  // draw border
  nvgBeginPath(nvg);
  nvgRoundedRect(nvg, marginBounds, strokeWidth*2);
  nvgStrokeWidth(nvg, strokeWidth);
  nvgStrokeColor(nvg, markColor);
  nvgStroke(nvg);

}
