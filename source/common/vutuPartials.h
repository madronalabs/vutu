
// vutu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "mldsp.h"

#include "madronalib.h"

#include "MZFiles.h"

#include <assert.h>

// stats about partials

namespace ml
{


static constexpr float kVutuPartialsFileVersion{ 1.0 };
static constexpr char kVutuPartialsFileType[] = "VutuPartials";
static constexpr char kVutuPartials2FileType[] = "VutuPartials2";

// these values are calculated after reading in the partials data.
struct PartialsStats
{
  // get range of values for each parameter (except phase)
  Interval timeRange; // min and max time in all partials
  Interval ampRange;
  Interval bandwidthRange;
  Interval freqRange;
  size_t nPartials;
  size_t maxActivePartials;
  float maxActiveTime;
  
  std::vector< Interval > partialTimeRanges; // time range for each partial
};

// a single partial is a trajectory of amp, frequency, bandwidth and phase over time.
// the time vector at index i contains the times for the value changes in all the other vectors at index i.
struct VutuPartial
{
  std::vector< float > time;
  std::vector< float > amp;
  std::vector< float > freq;
  std::vector< float > bandwidth;
  std::vector< float > phase;
};

// this structure holds the individual partials and related data.
// all this data except stats is stored to a partials file.
struct VutuPartialsData
{
  PartialsStats stats;
  std::vector< VutuPartial > partials;
  int version;
  Symbol type;
  
  // source and analysis parameters
  TextFragment sourceFile;
  float sourceDuration{0}; // whole duration of source, will be longer than stats.timeRange
  float resolution{0};
  float windowWidth{0};
  float ampFloor{0};
  float freqDrift{0};
  float loCut{0};
  float hiCut{0};
  float fundamental{0};
};

struct PartialFrame
{
  float amp{0};
  float freq{0};
  float bandwidth{0};
  float phase{0};
};

Interval getParamRangeInPartials(const VutuPartialsData& partialData, Symbol param);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrame(const VutuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial at the nearest sample to the specified time.
PartialFrame getPartialFrameNearest(const VutuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial by its frame index.
PartialFrame getPartialFrameByIndex(const VutuPartialsData& partialData, size_t partialIndex, size_t frameIndex);

inline std::ostream& operator<< (std::ostream& out, const PartialFrame& r)
{
  out << "[";
  out << r.amp;
  out << ", ";
  out << r.freq;
  out << ", ";
  out << r.bandwidth;
  out << ", ";
  out << r.phase;
  out << "]";
  return out;
}


inline Interval getVectorExtrema(const std::vector< float >& vec)
{
  float fMin{0}, fMax{0};
  if(vec.size() > 0)
  {
    fMin = *std::min_element(vec.begin(), vec.end());
    fMax = *std::max_element(vec.begin(), vec.end());
  }
  return Interval{fMin, fMax};
}

inline Interval getParamRangeInPartials(const VutuPartialsData& partialData, Symbol param)
{
  Interval r{std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
  
  for(int i=0; i<partialData.partials.size(); ++i)
  {
    const VutuPartial& partial = partialData.partials[i];
    Interval paramRange{0, 0};
    switch(hash(param))
    {
      case(hash("time")):
        paramRange = getVectorExtrema(partial.time);
        break;
      case(hash("amp")):
        paramRange = getVectorExtrema(partial.amp);
        break;
      case(hash("freq")):
        paramRange = getVectorExtrema(partial.freq);
        break;
      case(hash("bandwidth")):
        paramRange = getVectorExtrema(partial.bandwidth);
        break;
      default:
        break;
    }
    if(paramRange.x1 < r.x1)
    {
      r.x1 = paramRange.x1;
    }
    if(paramRange.x2 > r.x2)
    {
      r.x2 = paramRange.x2;
    }
  }
  return r;
}

inline void cutHighs(VutuPartialsData& p, float fCut)
{
  for(int i=0; i < p.partials.size(); ++i)
  {
    const VutuPartial& partial = p.partials[i];
    
    // if any instantaneous frequency of partial is > f, remove the partial
    float fMax = *std::max_element(partial.freq.begin(), partial.freq.end());
    if(fMax > fCut)
    {
      p.partials[i] = VutuPartial();
    }
  }
  
  // do cleanOutliers() afterwards to remove empty Partials
}

inline void cleanOutliers(VutuPartialsData& p)
{
  int before, after;
  
  auto discardPartial = [](const VutuPartial& p){
    return p.time.size() <= 1;
  };
  
  before = sizeToInt(p.partials.size());
  p.partials.erase(std::remove_if(p.partials.begin(), p.partials.end(), discardPartial), p.partials.end());
  after = sizeToInt(p.partials.size());
  std::cout << "cleanOutliers: before: " << before << ", after: " << after << "\n";
}

// Get stats for partials data to aid synthesis and drawing.
// TODO check that time is monotonically increasing
//
inline void calcStats(VutuPartialsData& p)
{
  p.stats.timeRange = getParamRangeInPartials(p, "time");
  p.stats.ampRange = getParamRangeInPartials(p, "amp");
  p.stats.bandwidthRange = getParamRangeInPartials(p, "bandwidth");
  p.stats.freqRange = getParamRangeInPartials(p, "freq");
  p.stats.nPartials = p.partials.size();
  
  // store min, max times for each partial
  p.stats.partialTimeRanges.clear();
  for(int i=0; i<p.stats.nPartials; ++i)
  {
    const VutuPartial& partial = p.partials[i];
    if(partial.time.size() > 0)
    {
      const auto& timeVec = partial.time;
      Interval ptr = getVectorExtrema(timeVec);
      p.stats.partialTimeRanges.push_back(ptr);
    }
    else
    {
      p.stats.partialTimeRanges.push_back(Interval{0, 0});
    }
  }
  
  // calc max simultaneous partials:
  //
  // push all start and end times

  std::vector< std::pair< float, bool > > startAndEndTimes;
  for(const auto& startAndEnd : p.stats.partialTimeRanges)
  {
    startAndEndTimes.push_back(std::pair< float, bool >{startAndEnd.x1, 0});
    startAndEndTimes.push_back(std::pair< float, bool >{startAndEnd.x2, 1});
  }
  // sort them
  std::sort(startAndEndTimes.begin(), startAndEndTimes.end(), [](std::pair< float, bool > a, std::pair< float, bool > b){
    return a.first < b.first;
  });
  // walk the sorted list keeping track of max simultaneously active partials
  int activePartials{0};
  int maxActive{0};
  float maxActiveTime{0.f};
  for(auto& p : startAndEndTimes)
  {
    if(p.second)
    {
      activePartials--;
    }
    else
    {
      activePartials++;
      maxActive = std::max(activePartials, maxActive);
      if(maxActive == activePartials)
      {
        maxActiveTime = p.first;
      }
    }
    
    // std::cout << "time: " << p.time << (p.isEnd ? "-" : "+") << ", n = " << activePartials << "\n";
  }
  
  assert(activePartials == 0);
  
  p.stats.maxActivePartials = maxActive;
  p.stats.maxActiveTime = maxActiveTime;
}

// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrame(const VutuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, size_t(0), nPartials))
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

// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrameNearest(const VutuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, size_t(0), nPartials))
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


// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrameByIndex(const VutuPartialsData& partialData, size_t partialIndex, size_t frameIndex)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, size_t(0), nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    size_t partialFrames = partial.time.size();
    
    //   std::cout << partialIndex << " range:" << partialTimeRange << "\n";
    
    if(within(frameIndex, size_t(0), partialFrames))
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

inline Tree< Value > vutuPartialsToValueTree(const VutuPartialsData& partialsData)
{
  Tree<Value> tree;
  tree["version"] = kVutuPartialsFileVersion;
  tree["type"] = kVutuPartials2FileType;
  tree["source"] = partialsData.sourceFile.getText();
  tree["resolution"] = partialsData.resolution;
  tree["window_width"] = partialsData.windowWidth;
  tree["amp_floor"] = partialsData.ampFloor;
  tree["freq_drift"] = partialsData.freqDrift;
  tree["lo_cut"] = partialsData.loCut;
  tree["hi_cut"] = partialsData.hiCut;
  tree["fundamental"] = partialsData.fundamental;
  
  const size_t nPartials = partialsData.partials.size();
  tree["n_partials"] = (int)nPartials;
  
  for(int i=0; i<nPartials; ++i)
  {
    VutuPartial& sp = const_cast<VutuPartial&>(partialsData.partials[i]);
    
    size_t partialLength = sp.time.size();
    
    TextFragment partialIndexText ("p", textUtils::naturalNumberToText(i));
    Path timePath(partialIndexText, "time");
    tree[timePath] = Value(sp.time);
    
    Path ampPath(partialIndexText, "amp");
    tree[ampPath] = Value(sp.amp);
    
    Path freqPath(partialIndexText, "freq");
    tree[freqPath] = Value(sp.freq);
    
    Path bwPath(partialIndexText, "bw");
    tree[bwPath] = Value(sp.bandwidth);
    
    Path phasePath(partialIndexText, "phase");
    tree[phasePath] = Value(sp.phase);
  }

  
  return tree;
}



// return a JSON object representing the partials. The caller is responsible for freeing the object.
//
inline JSONHolder vutuPartialsToJSON(const VutuPartialsData& partialsData)
{
  return valueTreeToJSON(vutuPartialsToValueTree(partialsData));
}


// return a binary blob representing the partials. The caller is responsible for freeing the object.
//
inline std::vector<uint8_t> vutuPartialsToBinary(const VutuPartialsData& partialsData)
{
  return valueTreeToBinary(vutuPartialsToValueTree(partialsData));
}

inline std::vector< float > getPartialDataFromTree(const Tree<Value>& tree, int partialIdx, Path pname)
{
  TextFragment partialIndexText ("p", textUtils::naturalNumberToText(partialIdx));
  Path dataPath(partialIndexText, pname);
  
  Value dataBlob = tree[dataPath];
  auto* blobDataPtr = dataBlob.data();
  unsigned blobSize = dataBlob.size();
  
  const float* pVectorData{reinterpret_cast<const float*>(blobDataPtr)};
  unsigned sizeInFloats = blobSize/sizeof(float);
  return std::vector<float>(pVectorData, pVectorData + sizeInFloats);
}

// parse the binary data and return a new VutuPartialsData object.
//
inline VutuPartialsData* valueTreeToVutuPartials(const Tree<Value>& tree)
{
  constexpr size_t kMaxPartials{2 << 24};
  VutuPartialsData* partialsData = new VutuPartialsData;
  
  size_t nPartials{0};
  if(tree.getNode("n_partials"))
  {
    int nPartials = tree["n_partials"].getIntValue();
  }
  else
  {
    while(1)
    {
      TextFragment partialIndexText ("p", textUtils::naturalNumberToText(nPartials));
      
      Path partialExistsPath(partialIndexText, "time");
      
      if(tree.getNode(partialExistsPath))
      {
        nPartials++;
      }
      else
      {
        break;
      }
      if(nPartials > kMaxPartials) break;
    }
  }

  if(partialsData)
  {

    size_t res = tree["resolution"].getFloatValue();
    
    if(nPartials > 0)
    {
      partialsData->version = tree["version"].getIntValue();
      partialsData->sourceDuration = tree["source_duration"].getFloatValue();
      partialsData->resolution = tree["resolution"].getFloatValue();
      partialsData->windowWidth = tree["window_width"].getFloatValue();
      partialsData->ampFloor = tree["amp_floor"].getFloatValue();
      partialsData->freqDrift = tree["freq_drift"].getFloatValue();
      partialsData->loCut = tree["lo_cut"].getFloatValue();
      partialsData->hiCut = tree["hi_cut"].getFloatValue();
      partialsData->fundamental = tree["fundamental"].getFloatValue();
      
      partialsData->partials.resize(nPartials);
      
      for(int i=0; i<nPartials; ++i)
      {
        partialsData->partials[i].time = getPartialDataFromTree(tree, i, "time");
        partialsData->partials[i].amp = getPartialDataFromTree(tree, i, "amp");
        partialsData->partials[i].freq = getPartialDataFromTree(tree, i, "freq");
        partialsData->partials[i].bandwidth = getPartialDataFromTree(tree, i, "bw");
        partialsData->partials[i].phase = getPartialDataFromTree(tree, i, "phase");
      }
    }
    calcStats(*partialsData);
  }
  

  return partialsData;
}

// parse the binary data and return a new VutuPartialsData object.
//
inline VutuPartialsData* binaryToVutuPartials(const std::vector<unsigned char>& binaryData)
{
  const uint8_t* pData{binaryData.data()};
  Tree<Value> tree = binaryToValueTree(binaryData);
  return valueTreeToVutuPartials(tree);
}

// parse the JSON and return a new VutuPartialsData object.
//
inline VutuPartialsData* jsonToVutuPartials(const JSONHolder& jsonData)
{
  auto vt = JSONToValueTree(jsonData);
  return valueTreeToVutuPartials(vt);
}

// load Vutu partials from the file. If successful, creates a new VutuPartialsData object that the caller must own.
//
inline VutuPartialsData* loadVutuPartialsFromFile(const File& fileToLoad)
{
  VutuPartialsData* newPartials{nullptr};
  
  // TODO verify successful load
  
  TextPath filePath = fileToLoad.getFullPath();
  TextFragment extension = FileUtils::getExtensionFromPath(filePath);
  
  if(extension == "utu")
  {
    TextFragment partialsText;
    if(fileToLoad.loadAsText(partialsText))
    {
      auto json = textToJSON(partialsText);
      newPartials = jsonToVutuPartials(json);
    }
  }
  if(extension == "ut2")
  {
    CharVector binaryData;
    if(fileToLoad.load(binaryData))
    {
      newPartials = binaryToVutuPartials(binaryData);
    }
  }
  
  // if we didn't save a source duration, fake one from partials data
  if(newPartials)
  {
    if(newPartials->sourceDuration == 0.0f)
    {
      //std::cout << "No duration found! using partials range " << newPartials->stats.timeRange << "\n";
      newPartials->sourceDuration = newPartials->stats.timeRange.x2;
    }
  }
  return newPartials;
}

}
