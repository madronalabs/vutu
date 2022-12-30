
// sumu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com



#include "sumuPartials.h"

namespace ml
{

Interval getVectorRange(const std::vector< float >& vec)
{
  float fMin = *std::min_element(vec.begin(), vec.end());
  float fMax = *std::max_element(vec.begin(), vec.end());
  return Interval{fMin, fMax};
}

Interval getParamRangeInPartials(const SumuPartialsData& partialData, Symbol param)
{
  Interval r{std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
  
  for(int i=0; i<partialData.partials.size(); ++i)
  {
    const SumuPartial& partial = partialData.partials[i];
    Interval paramRange{0, 0};
    switch(hash(param))
    {
      case(hash("time")):
        paramRange = getVectorRange(partial.time);
        break;
      case(hash("amp")):
        paramRange = getVectorRange(partial.amp);
        break;
      case(hash("freq")):
        paramRange = getVectorRange(partial.freq);
        break;
      case(hash("bandwidth")):
        paramRange = getVectorRange(partial.bandwidth);
        break;
      default:
        break;
    }
    if(paramRange.mX1 < r.mX1)
    {
      r.mX1 = paramRange.mX1;
    }
    if(paramRange.mX2 > r.mX2)
    {
      r.mX2 = paramRange.mX2;
    }
  }
  return r;
}

struct IntervalEndpoint
{
  float time;
  long isEnd;
};


void SumuPartialsData::cleanOutliers()
{
  int before, after;
  
  auto discardPartial = [](const SumuPartial& p){
    return p.time.size() <= 1;
  };
 
  
  before = partials.size();
  partials.erase(std::remove_if(partials.begin(), partials.end(), discardPartial), partials.end());
  after = partials.size();
  std::cout << "cleanup: before: " << before << ", after: " << after << "\n";
             
}


// Get stats for partials data to aid synthesis and drawing.
// TODO check that time is monotonically increasing
//
void SumuPartialsData::calcStats()
{
  stats.timeRange = getParamRangeInPartials(*this, "time");
  stats.ampRange = getParamRangeInPartials(*this, "amp");
  stats.bandwidthRange = getParamRangeInPartials(*this, "bandwidth");
  stats.freqRange = getParamRangeInPartials(*this, "freq");
  stats.nPartials = partials.size();

  // store min, max times for each partial
  stats.partialTimeRanges.clear();
  for(int i=0; i<stats.nPartials; ++i)
  {
    const SumuPartial& partial = partials[i];
    if(partial.time.size() > 0)
    {
      const auto& timeVec = partial.time;
      Interval ptr = getVectorRange(timeVec);
      stats.partialTimeRanges.push_back(ptr);
    }
    else
    {
      stats.partialTimeRanges.push_back(Interval{0, 0});
    }
  }
  
  // calc max simultaneous partials:
  //
  // push all start and end times
  std::vector< IntervalEndpoint > startAndEndTimes;
  for(const auto& startAndEnd : stats.partialTimeRanges)
  {
    startAndEndTimes.push_back(IntervalEndpoint{startAndEnd.mX1, 0});
    startAndEndTimes.push_back(IntervalEndpoint{startAndEnd.mX2, 1});
  }
  // sort them
  std::sort(startAndEndTimes.begin(), startAndEndTimes.end(), [](IntervalEndpoint a, IntervalEndpoint b){
    return a.time < b.time;
  });
  // walk the sorted list keeping track of max simultaneously active partials
  size_t activePartials{0};
  size_t maxActive{0};
  float maxActiveTime{0.f};
  for(auto& p : startAndEndTimes)
  {
    if(p.isEnd)
    {
      activePartials--;
    }
    else
    {
      activePartials++;
      maxActive = std::max(activePartials, maxActive);
      if(maxActive == activePartials)
      {
        maxActiveTime = p.time;
      }
    }
    
    // std::cout << "time: " << p.time << (p.isEnd ? "-" : "+") << ", n = " << activePartials << "\n";
  }
  
  assert(activePartials == 0);
  
  stats.maxActivePartials = maxActive;
  stats.maxActiveTime = maxActiveTime;
  
  std::cout << "\n\n max active: " << stats.maxActivePartials <<  "at time: " << stats.maxActiveTime << "\n";
}

TextFragment SumuPartialsData::getStatsText()
{
  TextFragment nPartials;
  
  TextFragment freq;
 // TextFragment r(nPartials, maxPartials, freq, duration);
  
  return "OK";
}

// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrame(const SumuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    auto partialTimeRange = partialData.stats.partialTimeRanges[partialIndex];
    if(within(t, partialTimeRange))
    {
      // get indexes of time samples before and after t
      // if this is slow, we can make an index into the time vector
      size_t i1{0};
      size_t i2{1};
      for(int i=1; i<partial.time.size(); ++i)
      {
        if(t < partial.time[i])
        {
          i1 = i - 1;
          i2 = i;
          break;
        }
      }
      
      // interpolate time to get fractional index corresponding to t
      float t1 = partial.time[i1];
      float t2 = partial.time[i2];
      float timeFrac = (t - t1) / (t2 - t1);
      
      // interpolate data
      f.amp = lerp(partial.amp[i1], partial.amp[i2], timeFrac);
      f.freq = lerp(partial.freq[i1], partial.freq[i2], timeFrac);
      f.bandwidth = lerp(partial.bandwidth[i1], partial.bandwidth[i2], timeFrac);
      
      // use phase directly
      // TODO compute from freq
      f.phase = partial.phase[i1];
    }
    else
    {
      // TODO generate interpolated frames just before and after timeRange
      
    }
  }
  
  return f;
}

// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrameNearest(const SumuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    auto partialTimeRange = partialData.stats.partialTimeRanges[partialIndex];
    
    //   std::cout << partialIndex << " range:" << partialTimeRange << "\n";
    
    if(within(t, partialTimeRange))
    {
      // get indexes of time samples before and after t
      // if this is slow, we can make an index into the time vector
      size_t i1{0};
      size_t i2{1};
      for(int i=1; i<partial.time.size(); ++i)
      {
        if(t < partial.time[i])
        {
          i1 = i - 1;
          i2 = i;
          break;
        }
      }
      
      // get the nearest index to time t
      float d1 = t - partial.time[i1];
      float d2 = partial.time[i2] - t;
      size_t nearestIndex = (d1 < d2) ? i1 : i2;
      
      // get data
      f.amp = partial.amp[nearestIndex];
      f.freq = partial.freq[nearestIndex];
      f.bandwidth = partial.bandwidth[nearestIndex];
      f.phase = partial.phase[nearestIndex];
    }
  }
  
  return f;
}


// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrameByIndex(const SumuPartialsData& partialData, size_t partialIndex, size_t frameIndex)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    size_t partialFrames = partial.time.size();
    
    //   std::cout << partialIndex << " range:" << partialTimeRange << "\n";
    
    if(within(frameIndex, 0UL, partialFrames))
    {
      // get data
      f.amp = partial.amp[frameIndex];
      f.freq = partial.freq[frameIndex];
      f.bandwidth = partial.bandwidth[frameIndex];
      f.phase = partial.phase[frameIndex];
    }
  }
  return f;
}

}
