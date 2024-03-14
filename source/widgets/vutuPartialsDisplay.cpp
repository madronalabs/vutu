

#include "vutuPartialsDisplay.h"

using namespace ml;

const float kFreqMargin{20.f};




void VutuPartialsDisplay::redrawPartials()
{
  // don't do the drawing here, because the context is not guaranteed to be valid.
  _partialsDirty = true;
}

  
void VutuPartialsDisplay::resize(ml::DrawContext dc)
{
  // constant background layer size, for now.
  int w = 2000;
  int h = 800;
  int bw = 0;
  int bh = 0;
  
  Rect bounds = getLocalBounds(dc, *this);
  w = bounds.width();
  h = bounds.height();
  
  // create new backing layer if needed
  if(_backingLayer.get())
  {
    bw = _backingLayer->width;
    bh = _backingLayer->height;
  }
  
  // if we do add a variable background layer size, this will repaint it when it changes.
  int xDiff = abs(bw - w);
  int yDiff = abs(bh - h);
  const int repaintDistance{256};
  int maxDiff = max(xDiff, yDiff);
  
  if((w != bw) || (h != bh))
  {
    // if timer is not running, start it
    if(!redrawTimer_.isActive())
    {
      redrawTimer_.callOnce
      ([&]() { redrawPartials(); }, milliseconds(125) );
    }
    else
    {
      // otherwise postpone to expire in 1/8 second.
      redrawTimer_.postpone(milliseconds(125));
    }
  }
}


MessageList VutuPartialsDisplay::animate(int elapsedTimeInMs, ml::DrawContext dc)
{
  if(_partialsDirty)
  {
    NativeDrawContext* nvg = getNativeContext(dc);
    
    Rect bounds = getLocalBounds(dc, *this);
    int w = bounds.width();
    int h = bounds.height();
    std::cout << "VutuPartialsDisplay::resize: " << w << " x " << h <<  "\n";
    
    _backingLayer = std::make_unique< DrawableImage >(nvg, w, h);
    
    paintPartials(dc);
    _partialsDirty = false;
    _dirty = true;
  }
  auto fundamental = _params.getRealFloatValue("fundamental");
  if(fundamental != prevFundamental)
  {
    _dirty = true;
    prevFundamental = fundamental;
  }
  
  return MessageList{};
}

void VutuPartialsDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("partials")):
      _pPartials = static_cast< const VutuPartialsData* > (ptr);
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
void VutuPartialsDisplay::paintPartials(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  if(!_backingLayer) return;
  int w = _backingLayer->width;
  int h = _backingLayer->height;
  
  const int gridSize = dc.coords.gridSizeInPixels;
  constexpr float kMarginSize{1/8.};
  int margin = gridSize*kMarginSize;
  
  const float maxBwSize = h/64.f;//gridSize/2.f;
  const float strokeWidth = 2.f;//gridSize/16.f;
  
  // begin rendering to backing image
  drawToImage(_backingLayer.get());
  nvgBeginFrame(nvg, w, h, 1.0f);
  
  // draw opaque black bg
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, ml::Rect{0.f, 0.f, float(_backingLayer->width), float(_backingLayer->height)});
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
  }
  
  bool partialsOK = _pPartials && _pPartials->stats.nPartials;
  if(partialsOK)
  {
    size_t nPartials = _pPartials->stats.nPartials;
    std::cout << "painting " << nPartials << " partials... \n";
    
    Interval analysisInterval = getParamValue("analysis_interval").getIntervalValue();
    
    Interval xRange{0.f, w - 1.f};
    Interval yRange{h - 1.f, 0.f};
    Interval timeInterval{0.f, (analysisInterval.mX2 -  analysisInterval.mX1)*_pPartials->sourceDuration};
    
    constexpr float kMinLineLength{2.f};
    auto xToTime = projections::linear(xRange, timeInterval);
    auto timeToX = projections::linear(timeInterval, xRange);
    
    auto freqRange = _pPartials->stats.freqRange;
    freqRange.mX1 -= kFreqMargin;
    freqRange.mX1 = max(freqRange.mX1, kFreqMargin);
    
    auto freqToY = projections::intervalMap(freqRange, yRange, projections::exp(freqRange));

    // drawn amplidutes range from -60dB to max in partials
    auto ampRange = _pPartials->stats.ampRange;
    ampRange.mX1 = dBToAmp(-90);
    
    Interval thicknessRange{h/512.f, h/32.f};
    //auto ampToThickness = projections::intervalMap(ampRange, thicknessRange, projections::unity);
    auto ampToThickness = projections::intervalMap(ampRange, thicknessRange, projections::exp(thicknessRange));
    
    // projections::printTable(ampToThickness, "ampToThickness", ampRange, 5);
    auto bandwidthToUnity = projections::linear(_pPartials->stats.bandwidthRange, {0, 1});
    
    /*
    auto testColor = rgba(1, 1, 1, 0.5f);
    nvgStrokeColor(nvg, testColor);
    nvgStrokeWidth(nvg, strokeWidth*5);
    nvgBeginPath(nvg);
    nvgX(nvg, Rect(0, 0, w, h));
    nvgStroke(nvg);
*/
    
    // time the partials bit
    auto roughStart = high_resolution_clock::now();
    size_t totalFramesDrawn{0};
    
    // draw frame ribs, always separated vby at least kMinLineLength
    nvgBeginPath(nvg);
    auto sineColor = rgba(0, 1, 0, 0.5f);
    nvgStrokeColor(nvg, sineColor);
    nvgStrokeWidth(nvg, strokeWidth);
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      float x1 = 0.;
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        
        if((i == 0) || (x > x1 + kMinLineLength))
        {
          x1 = x;
          totalFramesDrawn++;
          float thickness = ampToThickness(frame.amp);
          float colorOpacity = 0.5f;
          float maxOpacity = 1.0f;
          
          // std::cout << "frame " << i << " bw " << bw << "\n";
          
          if(thickness < strokeWidth)
          {
            thickness = strokeWidth;
          }
          
          float y1 = clamp(y - thickness/2.f, 0.f, float(h));
          float y2 = clamp(y + thickness/2.f, 0.f, float(h));
          
          nvgMoveTo(nvg, x, y1);
          nvgLineTo(nvg, x, y2);
        }
      }
    }
    nvgStroke(nvg);
    
    
    // draw frame fills
    nvgBeginPath(nvg);
    auto partialFillColor(rgba(0, 1, 0, 0.5));
    nvgFillColor(nvg, partialFillColor);
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      float x1 = 0.;
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        
        
        x1 = x;
        float thickness = ampToThickness(frame.amp);
        float y1 = clamp(y - thickness/2.f, 0.f, float(h));
        float y2 = clamp(y + thickness/2.f, 0.f, float(h));
        
        if(i == 0)
        {
          nvgMoveTo(nvg, x, y1);
        }
        else
        {
          nvgLineTo(nvg, x, y1);
        }
        
      }
      for(int i = framesInPartial - 1; i >= 0; --i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        
        
        x1 = x;
        float thickness = ampToThickness(frame.amp);
        float y1 = clamp(y - thickness/2.f, 0.f, float(h));
        float y2 = clamp(y + thickness/2.f, 0.f, float(h));
        

          nvgLineTo(nvg, x, y2);
        
        
      }
    }
    nvgFill(nvg);
    
    
    // draw spines
    auto spineColor(rgba(0, 1, 0, 0.25));
    nvgStrokeWidth(nvg, strokeWidth);
    nvgStrokeColor(nvg, spineColor);
    nvgBeginPath(nvg);
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        
        if(i == 0)
        {
          nvgMoveTo(nvg, x, y);
        }
        else 
        {
          nvgLineTo(nvg, x, y);
        }
      }
    }
    nvgStroke(nvg);
    
    // draw noise Xs
    nvgBeginPath(nvg);
    nvgStrokeWidth(nvg, strokeWidth);
    auto xColor(rgba(0, 1, 0, 1.0));
    nvgStrokeColor(nvg, xColor);
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        float bw = (frame.bandwidth);
        if(bw > 0.f)
        {
          float rectSize = bw*bw*maxBwSize;
          nvgX(nvg, alignCenterToPoint(Rect(0.f, 0.f, rectSize, rectSize), Vec2(x, y)));
        }
      }
    }
    nvgStroke(nvg);

    auto roughEnd = high_resolution_clock::now();
    auto roughMillisTotal = duration_cast<milliseconds>(roughEnd - roughStart).count();
    std::cout << "partials painting time rough millis: " << roughMillisTotal << "\n";
  }
  
  // end backing layer update
  nvgEndFrame(nvg);
}
 

void VutuPartialsDisplay::draw(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;
  float strokeWidthMul = getFloatPropertyWithDefault("stroke_width", getFloat(dc, "common_stroke_width"));
  int strokeWidth = gridSizeInPixels*strokeWidthMul;

  int margin = gridSizeInPixels/8;
  Rect marginBounds = shrink(bounds, margin);
  
  auto bgColor = getColorPropertyWithDefault("color", getColor(dc, "panel_bg"));
  auto markColor = getColor(dc, "partials");
  
  Interval xRange{0.f, w - 1.f};
  Interval yRange{h - 1.f, 0.f};

  // background
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, marginBounds);
    nvgFillColor(nvg, colors::blue);
    nvgFill(nvg);
  }
  
  if(!_backingLayer) return;
 
  bool partialsOK = _pPartials && _pPartials->stats.nPartials;
  if(partialsOK)
  {
    Rect pBounds = getPixelBounds(dc, *this);
    int pw = pBounds.width();
    int ph = pBounds.height();
    
    int bw = _backingLayer->width;
    int bh = _backingLayer->height;
    

    auto freqRange = _pPartials->stats.freqRange;
    freqRange.mX1 -= kFreqMargin;
    freqRange.mX1 = max(freqRange.mX1, kFreqMargin);
    
    auto freqToY = projections::intervalMap(freqRange, yRange, projections::exp(freqRange));
    auto fundamental = _params.getRealFloatValue("fundamental");
    
    float intervalStart = getFloatProperty("interval_start");
    float intervalEnd = getFloatProperty("interval_end");
    Interval timeInterval{intervalStart, intervalEnd};
    
    // make an image pattern. The entire source image maps to the specified rect of the destination.
    //NVGpaint img = nvgImagePattern(nvg, margin, margin, w - margin*2, h - margin*2, 0, nativeImage, 1.0f);


    NVGpaint img = nvgImagePattern(nvg, margin*2, margin*2, w - margin*4, h - margin*4, 0, _backingLayer->_buf->image, 1.0f);

    // paint image lighten over bg
 //   nvgSave(nvg);
//    nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
    nvgBeginPath(nvg);
//    nvgRect(nvg, margin, margin, w - margin*2, h - margin*2);
    nvgRect(nvg, margin*2, margin*2, w - margin*4, h - margin*4);
    nvgFillPaint(nvg, img);
    nvgFill(nvg);
//    nvgRestore(nvg);
    
    // draw max active time triangle
    if(_pPartials->stats.maxActiveTime > 0.f)
    {
      Interval xRange{marginBounds.left(), marginBounds.right()};
      auto xToProgress = projections::linear({0.f, 1.f}, xRange);
      auto timeToX = projections::linear(timeInterval, {0.f, w - 1.f});
      
      float px = timeToX(_pPartials->stats.maxActiveTime);
      
      float triSize = gridSizeInPixels/8;

      nvgBeginPath(nvg);
      nvgFillColor(nvg, markColor);

      nvgMoveTo(nvg, px, marginBounds.top());
      nvgLineTo(nvg, px + triSize, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.top() + triSize*2 );
      nvgLineTo(nvg, px - triSize, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.top());

      nvgFill(nvg);
    }
    
    // draw fundamental line
    {
      float funY = freqToY(fundamental);
      nvgBeginPath(nvg);
      nvgStrokeWidth(nvg, strokeWidth);
      nvgStrokeColor(nvg, markColor);
      nvgMoveTo(nvg, 0, funY);
      nvgLineTo(nvg, w, funY);
      nvgStroke(nvg);
    }
    
  }
  
  // TEST
  //std::cout << "PartialsDisplay start: " << getFloatProperty("interval_start") << " -- " <<  getFloatProperty("interval_end") << "\n";
  
  // draw border
  nvgBeginPath(nvg);
  nvgRoundedRect(nvg, marginBounds, strokeWidth*2);
  nvgStrokeWidth(nvg, strokeWidth);
  nvgStrokeColor(nvg, markColor);
  nvgStroke(nvg);
}
