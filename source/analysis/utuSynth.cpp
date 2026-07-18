
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// Bandwidth-enhanced sinusoidal synthesizer, written from the model spec
// (Fitz & Haken; phase discipline per Fitz & Fulop Sec. 8); no Loris
// source consulted. Correctness is pinned by the synth-null, noise-bw and
// roundtrip selftests: a stationary partial must null against an ideal
// sinusoid below -60 dB with its stored (cosine-referenced) phase, and a
// noise analysis must render at the source's RMS.

#include "utuSynth.h"

#include <algorithm>
#include <cmath>

namespace ml::utu
{

namespace
{

constexpr double kPi = 3.14159265358979324;
constexpr double kTwoPi = 2. * kPi;

// Noise modulator: uniform noise through two flat (Butterworth) lowpass
// SVFs. Q must not peak: a resonant modulator rings the noise ±cutoff
// around every carrier, planting shadow peaks beside each partial where
// the source has valleys (measured as a mel-spectral regression on dense
// material). The gain sets E[nz²] = 1/2, which makes a bw = 1 partial
// carry exactly the energy its amplitude claims — the same bookkeeping
// addNoiseEnergy uses (verified by the noise-bw and bw-render selftests).
constexpr float kModCutoffHz = 500.f;
constexpr float kModQ = 0.707f;
constexpr float kModGain = 12.67f;

// deterministic uniform noise in [-1, 1] (LCG; every partial gets its own
// stream so partials decorrelate)
struct NoiseSource
{
  uint32_t state;
  explicit NoiseSource(uint32_t seed) : state(seed * 2654435761u + 1u) {}
  float next()
  {
    state = state * 1664525u + 1013904223u;
    return float(int32_t(state)) * (1.f / 2147483648.f);
  }
};

// ZDF state-variable lowpass (Simper), stable at any cutoff below Nyquist
struct SvfLopass
{
  float g{0.f}, k{0.f}, a1{0.f}, a2{0.f}, a3{0.f};
  float ic1{0.f}, ic2{0.f};
  void configure(float cutoffHz, float q, float sr)
  {
    g = tanf(float(kPi) * cutoffHz / sr);
    k = 1.f / q;
    a1 = 1.f / (1.f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  float process(float in)
  {
    const float v3 = in - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.f * v1 - ic1;
    ic2 = 2.f * v2 - ic2;
    return v2;
  }
};

// one sample-quantized breakpoint of the render schedule
struct SchedPoint
{
  int64_t sample;
  double amp, freq, bw, phase;
  bool anchor;  // phase is a stored analysis phase to anchor to
};

}  // namespace

// Build the schedule for one partial: quantize breakpoint times to samples
// (monotone; colliding breakpoints are dropped), correct each stored phase
// for the quantization shift, zero amplitude above Nyquist, clamp bw to
// [0, 1], and wrap the partial in fadeTime onset/offset ramps to zero.
static std::vector<SchedPoint> buildSchedule(const VutuPartial& p, double sr, double fadeTime)
{
  std::vector<SchedPoint> sched;
  const size_t n = p.time.size();
  if (n == 0) return sched;
  sched.reserve(n + 2);

  const double nyquist = 0.5 * sr;
  for (size_t k = 0; k < n; ++k)
  {
    const int64_t s = int64_t(llround(double(p.time[k]) * sr));
    if (!sched.empty() && (s <= sched.back().sample)) continue;
    SchedPoint sp;
    sp.sample = s;
    sp.freq = p.freq[k];
    sp.amp = (p.freq[k] > nyquist) ? 0. : p.amp[k];
    sp.bw = std::min(1., std::max(0., double(p.bandwidth[k])));
    // the stored phase belongs to the exact breakpoint time; move it to
    // the quantized sample
    sp.phase = p.phase[k] + kTwoPi * p.freq[k] * (s / sr - double(p.time[k]));
    sp.anchor = true;
    sched.push_back(sp);
  }
  if (sched.empty()) return sched;

  const int64_t fadeSamples = std::max(int64_t(1), int64_t(llround(fadeTime * sr)));
  SchedPoint in = sched.front();
  in.sample = std::max(int64_t(0), sched.front().sample - fadeSamples);
  in.amp = 0.;
  in.anchor = false;
  if (in.sample < sched.front().sample) sched.insert(sched.begin(), in);

  SchedPoint out = sched.back();
  out.sample += fadeSamples;
  out.amp = 0.;
  out.anchor = false;
  sched.push_back(out);
  return sched;
}

// Render one partial into out (which is already sized). The oscillator
// integrates frequency per sample (midpoint rule, matching the phase-fix
// travel π(f0+f1)·dt per segment) with double phase; wherever the
// amplitude leaves zero at an anchored breakpoint, phase snaps to the
// stored value — the phase-correct rendering discipline of Sec. 8. The
// stored phase is the phase of the analytic signal, so the carrier is
// cos(θ).
static void renderPartial(const std::vector<SchedPoint>& sched, double sr, uint32_t seed,
                          std::vector<float>& out)
{
  if (sched.size() < 2) return;

  NoiseSource noise(seed);
  SvfLopass svf1, svf2;
  svf1.configure(kModCutoffHz, kModQ, float(sr));
  svf2.configure(kModCutoffHz, kModQ, float(sr));

  // phase at the fade-in start: anchored so integration arrives at the
  // first stored phase on time
  const SchedPoint& first = sched.front().anchor ? sched.front() : sched[1];
  double theta = first.phase - kTwoPi * first.freq * ((first.sample - sched.front().sample) / sr);

  for (size_t seg = 0; seg + 1 < sched.size(); ++seg)
  {
    const SchedPoint& a = sched[seg];
    const SchedPoint& b = sched[seg + 1];
    const int64_t len = b.sample - a.sample;
    if (len <= 0) continue;

    if (a.anchor && (a.amp <= 0.))
    {
      theta = a.phase;
    }
    const double dAmp = (b.amp - a.amp) / len;
    const double dFreq = (b.freq - a.freq) / len;
    const double dBw = (b.bw - a.bw) / len;
    const int64_t end = std::min(b.sample, int64_t(out.size()));
    for (int64_t s = a.sample; s < end; ++s)
    {
      const int64_t m = s - a.sample;
      if (s >= 0)
      {
        const double amp = a.amp + dAmp * m;
        const double bw = a.bw + dBw * m;
        const double nz = kModGain * svf2.process(svf1.process(noise.next()));
        const double carrier = sqrt(1. - bw) + nz * sqrt(2. * bw);
        out[size_t(s)] += float(amp * carrier * cos(theta));
      }
      // midpoint-frequency integration: arrival phase after the segment is
      // theta0 + π·(f0+f1)·dt, the phase-fix travel
      theta += kTwoPi * (a.freq + dFreq * (m + 0.5)) / sr;
      theta = std::fmod(theta, kTwoPi);
    }
  }
}

void PartialSynthesizer::render(const VutuPartialsData& partials, std::vector<float>& out)
{
  const double sr = _params.sampleRate;
  const double fade = _params.fadeTime;

  int64_t endSample = 0;
  std::vector<std::vector<SchedPoint>> schedules;
  schedules.reserve(partials.partials.size());
  for (const auto& p : partials.partials)
  {
    schedules.push_back(buildSchedule(p, sr, fade));
    if (!schedules.back().empty())
    {
      endSample = std::max(endSample, schedules.back().back().sample);
    }
  }

  out.assign(size_t(std::max(int64_t(0), endSample)) + 1, 0.f);
  for (size_t i = 0; i < schedules.size(); ++i)
  {
    renderPartial(schedules[i], sr, uint32_t(i + 1), out);
  }
}

}  // namespace ml::utu
