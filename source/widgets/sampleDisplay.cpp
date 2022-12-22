

#include "sampleDisplay.h"

using namespace ml;

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
    _partialsDirty = true;
  }
}


MessageList SampleDisplay::animate(int elapsedTimeInMs, ml::DrawContext dc)
{
  if(_partialsDirty)
  {
    paintSample(dc);
    _partialsDirty = false;
    _dirty = true;
  }
  
  float p = getFloatPropertyWithDefault("progress", 0);
  //std::cout << "progress: " << progress << "\n";
  if(p != _progress)
  {
    _progress = p;
    _dirty = true;
  }
  
  return MessageList{};
}


void SampleDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("sample")):
      _pSample = static_cast< const sumu::Sample* > (ptr);
      
      std::cout << "receiveNamedRawPointer: " << _pSample << "\n";
      
      std::cout << _pSample->data.size() << " frames.\n";
      _partialsDirty = true;
      break;
    default:
      break;
  }
}


// Repaint the backing layer with an image of the partials.
// Since we draw to a backing layer here, this must be called only
// from animate(), not from draw().
//
void SampleDisplay::paintSample(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;

  if(!_backingLayer) return;
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
  
  
  bool sampleOK = _pSample && _pSample->data.size();
  
  if(sampleOK)
  {
    size_t frames = _pSample->data.size();
    
    Interval xRange{0.f, w - 1.f};
    Interval yRange{h - 1.f, 0.f};
    float yCenter{(h - 1.f)/2.f};
    
    constexpr float kMinLineLength{2.f};
    auto xToTime = projections::linear({0.f, w - 1.f}, {0, frames - 1.f});
    auto timeToX = projections::linear({0, frames - 1.f}, {0.f, w - 1.f});
    
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
    
    // TEMP no smoothing
    for(int x=0; x<w; ++x)
    {
      int frame = clamp(size_t(xToTime(x)), 0UL, frames);
      float amp = _pSample->data[frame];
      float thickness = ampToThickness(amp);
      nvgMoveTo(nvg, x, yCenter - thickness);
      nvgLineTo(nvg, x, yCenter + thickness);
    }
    nvgStroke(nvg);
    
    auto roughEnd = high_resolution_clock::now();
    auto roughMillisTotal = duration_cast<milliseconds>(roughEnd - roughStart).count();
    std::cout << " sample painting time rough millis: " << roughMillisTotal << "\n";
  }
  
  
  // end backing layer update
  nvgEndFrame(nvg);
}
 

void SampleDisplay::draw(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;
  
  int margin = gridSizeInPixels/8.f;
  Rect marginBounds = shrink(bounds, margin);

  float strokeWidthMul = getFloatPropertyWithDefault("stroke_width", getFloat(dc, "common_stroke_width"));
  float strokeWidth = gridSizeInPixels*strokeWidthMul;
  
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
  
  bool sampleOK = _pSample && _pSample->data.size();
  
  if(sampleOK)
  {
    auto nativeImage = getNativeImageHandle(*_backingLayer);
    
    // make an image pattern. The entire source image maps to the specified rect of the destination.
    NVGpaint img = nvgImagePattern(nvg, margin, margin, w - margin*2, h - margin*2, 0, nativeImage, 1.0f);
    
    // draw image
    {
      nvgSave(nvg);
      nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
      
      // paint image lighten over bg
      nvgBeginPath(nvg);
      nvgRect(nvg, margin, margin, w - margin*2, h - margin*2);
      nvgFillPaint(nvg, img);
      nvgFill(nvg);
      
      nvgRestore(nvg);
    }
    
    // draw progress
    if(_progress > 0.f)
    {
      Interval xRange{marginBounds.left(), marginBounds.right()};
      auto xToProgress = projections::linear({0.f, 1.f}, xRange);
      
      float px = xToProgress(_progress);
      nvgStrokeWidth(nvg, strokeWidth);
      nvgBeginPath(nvg);
      nvgStrokeColor(nvg, markColor);
      nvgMoveTo(nvg, px, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.bottom());
      nvgStroke(nvg);
    }
  }
  
  // draw border
  nvgBeginPath(nvg);
  nvgRoundedRect(nvg, marginBounds, strokeWidth*2);
  nvgStrokeWidth(nvg, strokeWidth);
  nvgStrokeColor(nvg, markColor);
  nvgStroke(nvg);
  
}
