

#include "sampleDisplay.h"

using namespace ml;

void SampleDisplay::resize(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);

  // constant background layer size, for now.
  int w = 564;
  int h = 170;
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
  
  return MessageList{};
}


void SampleDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("sample")):
      _pSample = static_cast< const utu::Sample* > (ptr);
      
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
  const int gridSize = dc.coords.gridSizeInPixels;

  if(!_pSample)
  {
    /*
    Rect bounds = getLocalBounds(dc, *this);
    nvgBeginPath(nvg);
    nvgRect(nvg, bounds);
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
     */
    return;
  }
  
  if(!_backingLayer) return;

  int w = _backingLayer->width;
  int h = _backingLayer->height;
   
  // begin rendering to backing layer
  drawToLayer(_backingLayer.get());
  nvgBeginFrame(nvg, w, h, 1.0f);
  
  // draw opaque black bg
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, ml::Rect{0.f, 0.f, float(_backingLayer->width), float(_backingLayer->height)});
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
  }

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
  
  auto color = getColor(dc, "partials");
  
  // time the partials bit
  auto roughStart = high_resolution_clock::now();
  size_t totalFramesRead{0};
  size_t totalFramesDrawn{0};

  nvgStrokeColor(nvg, color);
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

  
  // end backing layer update
  nvgEndFrame(nvg);
}
 

void SampleDisplay::draw(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSize = dc.coords.gridSizeInPixels;
  
  auto bgColor = getColorPropertyWithDefault("color", getColor(dc, "panel_bg"));
  
  if(!_pSample)
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, 0, 0, w, h);
    nvgFillColor(nvg, bgColor);
    nvgFill(nvg);
    return;
  }
  if(!_backingLayer) return;
 
  
  Rect pBounds = getPixelBounds(dc, *this);
  int pw = pBounds.width();
  int ph = pBounds.height();
  
  int bw = _backingLayer->width;
  int bh = _backingLayer->height;
  
  int margin = gridSize/64.f;
  
  // blit backing layer to main layer
  auto nativeImage = getNativeImageHandle(*_backingLayer);
  
  // paint background color
  nvgBeginPath(nvg);
  nvgRect(nvg, 0, 0, w, h);
  nvgFillColor(nvg, bgColor);
  nvgFill(nvg);
  
  
  // make an image pattern. The entire source image maps to the specified rect of the destination.
  NVGpaint img = nvgImagePattern(nvg, margin, margin, w - margin*2, h - margin*2, 0, nativeImage, 1.0f);

  nvgSave(nvg);
  nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
  
  // paint image lighten over bg
  nvgBeginPath(nvg);
  nvgRect(nvg, margin, margin, w - margin*2, h - margin*2);
  nvgFillPaint(nvg, img);
  nvgFill(nvg);
  
  nvgRestore(nvg);
 
}