
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuSynth.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "MLDSPBank.h"
#include "mldsp.h"

namespace ml::utu
{

namespace
{

constexpr double kPi = 3.14159265358979324;
constexpr double kTwoPiD = 2. * kPi;
constexpr int kMaxVoices = 256;
constexpr int kNumGroups = kMaxVoices / 4;

// modulator gain matching the modulation energy of Loris's Gaussian noise
// through its Chebyshev lowpass, calibrated by comparing rendered bw=1
// partials against Loris::Synthesizer (see synth-test)
constexpr float kModGain = 11.11f;

// One linear segment of a partial's rendering schedule. phi0 is the carrier
// phase at s0, precomputed in double following Loris Oscillator/Synthesizer:
// anchored to the stored breakpoint phase where the amplitude leaves zero,
// continuous (midpoint-frequency integration) elsewhere.
struct Segment
{
  int64_t s0{0}, s1{0};  // samples
  float f0{0}, f1{0};    // Hz
  float a0{0}, a1{0};
  float b0{0}, b1{0};
  double phi0{0};
};

struct Schedule
{
  std::vector<Segment> segs;
  int64_t startSample{0}, endSample{0};
};

double phaseTravel(double f0, double f1, int64_t samples, double sr)
{
  return kPi * (f0 + f1) * (double(samples) / sr);
}

// phase within a segment at s0 + m, by the same midpoint-frequency sums the
// per-frame rendering performs
double phaseInSegment(const Segment& seg, int64_t m, double sr)
{
  const int64_t len = seg.s1 - seg.s0;
  if (len <= 0 || m <= 0) return seg.phi0;
  const double dF = (double(seg.f1) - seg.f0) / len;
  return seg.phi0 + (kTwoPiD / sr) * (m * double(seg.f0) + 0.5 * dF * m * m);
}

Schedule buildSchedule(const VutuPartial& p, double sr, double fadeTime)
{
  Schedule sched;
  const size_t n = p.time.size();
  if (n == 0) return sched;

  // quantize breakpoint times to samples, correcting phase for the shift
  std::vector<int64_t> s(n);
  std::vector<double> phi(n);
  std::vector<float> aEff(n), bw(n);
  const double nyquist = 0.5 * sr;
  for (size_t k = 0; k < n; ++k)
  {
    s[k] = int64_t(llround(double(p.time[k]) * sr));
    if (k > 0)
    {
      s[k] = std::max(s[k], s[k - 1]);
    }
    phi[k] = p.phase[k] + kTwoPiD * p.freq[k] * (s[k] / sr - double(p.time[k]));
    aEff[k] = (p.freq[k] > nyquist) ? 0.f : p.amp[k];
    bw[k] = std::min(1.f, std::max(0.f, p.bandwidth[k]));
  }

  const int64_t fadeSamples = std::max(int64_t(1), int64_t(llround(fadeTime * sr)));
  sched.startSample = std::max(int64_t(0), s[0] - fadeSamples);

  auto pushSegment = [&](int64_t s0, int64_t s1, float f0, float f1, float a0, float a1,
                         float b0, float b1, bool anchorAtEnd, double phiEnd, double phiStart)
  {
    Segment seg{s0, s1, f0, f1, a0, a1, b0, b1, 0.};
    seg.phi0 = anchorAtEnd ? (phiEnd - phaseTravel(f0, f1, s1 - s0, sr)) : phiStart;
    sched.segs.push_back(seg);
  };

  // onset fade: amplitude 0 up to the first breakpoint, phase anchored so
  // the stored phase is achieved there
  pushSegment(sched.startSample, s[0], p.freq[0], p.freq[0], 0.f, aEff[0], bw[0], bw[0], true,
              phi[0], 0.);

  for (size_t k = 1; k < n; ++k)
  {
    const Segment& prev = sched.segs.back();
    const double phiStart = phaseInSegment(prev, prev.s1 - prev.s0, sr);
    // a zero-amplitude segment start is a phase reset point
    const bool anchor = (prev.a1 == 0.f);
    pushSegment(s[k - 1], s[k], p.freq[k - 1], p.freq[k], aEff[k - 1], aEff[k], bw[k - 1],
                bw[k], anchor, phi[k], phiStart);
  }

  // offset fade to zero
  {
    const Segment& prev = sched.segs.back();
    const double phiStart = phaseInSegment(prev, prev.s1 - prev.s0, sr);
    pushSegment(s[n - 1], s[n - 1] + fadeSamples, p.freq[n - 1], p.freq[n - 1], aEff[n - 1],
                0.f, bw[n - 1], bw[n - 1], false, 0., phiStart);
  }

  sched.endSample = sched.segs.back().s1;
  return sched;
}

struct Voice
{
  const Schedule* sched{nullptr};
  size_t segIdx{0};
  bool active() const { return sched != nullptr; }
};

struct LaneRows
{
  alignas(16) float freq[kFramesPerBlock];  // cycles per sample, midpoint
  alignas(16) float amp[kFramesPerBlock];
  alignas(16) float bw[kFramesPerBlock];
  float phiWrapped{0};  // carrier phase for frame 0, wrapped to [0, 2pi)
};

// fill one voice's rows for the block starting at sample S; returns false
// when the voice is finished before the block starts
bool fillLane(Voice& v, int64_t S, double sr, LaneRows& rows)
{
  const auto& segs = v.sched->segs;
  const float oneOverSR = float(1. / sr);

  // advance past completed segments
  while ((v.segIdx < segs.size()) && (S >= segs[v.segIdx].s1))
  {
    ++v.segIdx;
  }
  if (v.segIdx >= segs.size())
  {
    return false;
  }

  // carrier phase at block start: before the schedule begins, integrate the
  // constant onset frequency backwards so the anchor still lands
  size_t si = v.segIdx;
  const Segment* seg = &segs[si];
  double phiS;
  if (S < seg->s0)
  {
    phiS = seg->phi0 - kTwoPiD * seg->f0 * ((seg->s0 - S) / sr);
  }
  else
  {
    phiS = phaseInSegment(*seg, S - seg->s0, sr);
  }
  rows.phiWrapped = float(phiS - kTwoPiD * std::floor(phiS / kTwoPiD));

  for (int i = 0; i < kFramesPerBlock; ++i)
  {
    const int64_t sNow = S + i;
    while ((si < segs.size()) && (sNow >= segs[si].s1))
    {
      ++si;
    }
    if (si >= segs.size())
    {
      // finished mid-block; hold frequency, silence
      rows.freq[i] = segs.back().f1 * oneOverSR;
      rows.amp[i] = 0.f;
      rows.bw[i] = 0.f;
      continue;
    }
    seg = &segs[si];
    if (sNow < seg->s0)
    {
      // not started yet
      rows.freq[i] = seg->f0 * oneOverSR;
      rows.amp[i] = 0.f;
      rows.bw[i] = seg->b0;
      continue;
    }
    const int64_t len = seg->s1 - seg->s0;
    const float m = float(sNow - seg->s0);
    const float inv = 1.f / float(len);  // len >= 1 here
    rows.freq[i] = (seg->f0 + (m + 0.5f) * (seg->f1 - seg->f0) * inv) * oneOverSR;
    rows.amp[i] = seg->a0 + m * (seg->a1 - seg->a0) * inv;
    rows.bw[i] = seg->b0 + m * (seg->b1 - seg->b0) * inv;
  }
  v.segIdx = si;
  return true;
}

}  // namespace

void PartialSynthesizer::render(const VutuPartialsData& partials, std::vector<float>& out)
{
  const double sr = _params.sampleRate;
  const double fade = _params.fadeTime;

  // build schedules, sorted by start
  std::vector<Schedule> schedules;
  schedules.reserve(partials.partials.size());
  int64_t endMax = 0;
  for (const VutuPartial& p : partials.partials)
  {
    if (p.time.empty()) continue;
    Schedule s = buildSchedule(p, sr, fade);
    endMax = std::max(endMax, s.endSample);
    schedules.push_back(std::move(s));
  }
  std::sort(schedules.begin(), schedules.end(),
            [](const Schedule& a, const Schedule& b) { return a.startSample < b.startSample; });

  out.assign(size_t(endMax + 1), 0.f);

  GenBank<TestSineGen, kMaxVoices> carrier;
  std::array<NoiseGen<float4>, kNumGroups> noise;
  std::array<Lopass<float4>, kNumGroups> lp1, lp2;
  const float4 lpOmega(float(500. / sr));
  const float4 lpK(1.414f);
  const auto lpCoeffs = Lopass<float4>::makeCoeffs({lpOmega, lpK});
  for (int g = 0; g < kNumGroups; ++g)
  {
    noise[g].setSeed(uint32_t(g * 4 + 1));
    lp1[g].clear();
    lp2[g].clear();
  }

  std::vector<const Schedule*> pending;
  for (const Schedule& s : schedules) pending.push_back(&s);

  // multiple passes over the timeline if ever there are more simultaneous
  // partials than voices
  while (!pending.empty())
  {
    std::vector<const Schedule*> deferred;
    std::array<Voice, kMaxVoices> voices{};
    std::array<LaneRows, 4> laneRows{};
    size_t nextEvent = 0;
    carrier.clear();

    for (int64_t S = 0; S <= endMax; S += kFramesPerBlock)
    {
      // assign schedules starting in this block to free voices
      while ((nextEvent < pending.size()) &&
             (pending[nextEvent]->startSample < S + kFramesPerBlock))
      {
        int free = -1;
        for (int v = 0; v < kMaxVoices; ++v)
        {
          if (!voices[v].active())
          {
            free = v;
            break;
          }
        }
        if (free >= 0)
        {
          voices[free] = Voice{pending[nextEvent], 0};
        }
        else
        {
          deferred.push_back(pending[nextEvent]);
        }
        ++nextEvent;
      }

      for (int g = 0; g < kNumGroups; ++g)
      {
        bool anyActive = false;
        for (int lane = 0; lane < 4; ++lane)
        {
          Voice& v = voices[g * 4 + lane];
          if (v.active() && fillLane(v, S, sr, laneRows[lane]))
          {
            anyActive = true;
            if (v.sched->endSample < S + kFramesPerBlock)
            {
              v = Voice{};  // finished within this block
            }
          }
          else
          {
            v = Voice{};
            std::fill(laneRows[lane].freq, laneRows[lane].freq + kFramesPerBlock, 0.f);
            std::fill(laneRows[lane].amp, laneRows[lane].amp + kFramesPerBlock, 0.f);
            std::fill(laneRows[lane].bw, laneRows[lane].bw + kFramesPerBlock, 0.f);
            laneRows[lane].phiWrapped = 0.f;
          }
        }
        if (!anyActive) continue;

        // anchor carrier phases for this block: TestSineGen increments
        // before output, and sin(x + pi/2) = cos(x)
        {
          alignas(16) float om[4];
          for (int lane = 0; lane < 4; ++lane)
          {
            double o = laneRows[lane].phiWrapped + 0.5 * kPi -
                       kTwoPiD * laneRows[lane].freq[0];
            o -= kTwoPiD * std::floor(o / kTwoPiD);
            om[lane] = float(o);
          }
          carrier[g].omega_ = loadFloat4(om);
        }

        float* dst = out.data() + S;
        const int64_t blockEnd = std::min<int64_t>(kFramesPerBlock, endMax + 1 - S);
        for (int i = 0; i < blockEnd; ++i)
        {
          const float4 f4(laneRows[0].freq[i], laneRows[1].freq[i], laneRows[2].freq[i],
                          laneRows[3].freq[i]);
          const float4 a4(laneRows[0].amp[i], laneRows[1].amp[i], laneRows[2].amp[i],
                          laneRows[3].amp[i]);
          const float4 b4(laneRows[0].bw[i], laneRows[1].bw[i], laneRows[2].bw[i],
                          laneRows[3].bw[i]);

          const float4 c4 = carrier[g].nextFrame({f4});
          const float4 nz4 =
              lp2[g].nextFrame(lp1[g].nextFrame(noise[g].nextFrame(), lpCoeffs), lpCoeffs) *
              float4(kModGain);
          const float4 am4 = sqrt(float4(1.f) - b4) + nz4 * sqrt(float4(2.f) * b4);
          dst[i] += vecSumH(c4 * am4 * a4);
        }
      }
    }
    pending = std::move(deferred);
    if (!pending.empty() && (pending.size() == schedules.size()))
    {
      break;  // safety: no voice was ever freed
    }
  }
}

}  // namespace ml::utu
