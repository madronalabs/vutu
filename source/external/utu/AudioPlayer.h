//
// Copyright (c) 2021 Greg Wuller.
//
// SPDX-License-Identifier: GPL-2.0-or-later
//

#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <RtAudio.h>
#pragma GCC diagnostic pop

#include <samplerate.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

class AudioPlayer final
{
 public:
  using Samples64 = std::vector<double>;
  using Samples32 = std::vector<float>;

  static std::vector<std::string> getOutputDeviceDescriptions()
  {
    std::vector<std::string> descriptions;

    RtAudio dac(RtAudio::UNSPECIFIED, &_errorCallback);

    auto deviceCount = dac.getDeviceCount();
    for (unsigned int i = 0; i < deviceCount; i++) {
      auto info = dac.getDeviceInfo(i);
      descriptions.push_back(info.name);
    }

    return descriptions;
  }

  AudioPlayer(const Samples64& samples, uint32_t sampleRate)
      : _samples(samples),
        _sampleRate(sampleRate),
        _playbackOffset(0),
        _blocksOutput(0),
        _convertedSamples({})
  {
  }

  int play(std::optional<uint8_t> outputDevice = {}, bool verbose = true, bool src = true)
  {
    using namespace std::chrono_literals;

    _playbackOffset = 0;
    _blocksOutput = 0;

    int status = 0;

    RtAudio dac(RtAudio::UNSPECIFIED, &_errorCallback);

    if (dac.getDeviceCount() < 1) {
      std::cout << "error: No audio devices found\n";
      return -1;
    }

    dac.showWarnings();

    RtAudio::StreamParameters params;
    params.deviceId = outputDevice ? *outputDevice : dac.getDefaultOutputDevice();
    params.nChannels = 1;
    params.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = 0;
    options.flags |= RTAUDIO_SCHEDULE_REALTIME;

    unsigned int bufferFrames = 512;  // size of output block

    auto info = dac.getDeviceInfo(params.deviceId);
    if (verbose) {
      std::cout << "Output Device: " << info.name << " (sr: " << info.preferredSampleRate
                << " ch: " << info.outputChannels << ")\n";
    }

    if (info.preferredSampleRate != _sampleRate) {
      if (!_convertedSamples && !src) {
        // NOTE: bail if the sample rate of the playback device doesn't match the
        // input. Forcing the audio device to change sample rate causes problems
        // anywhere from disruption of playback in other applications to
        // destabalizing the audio system if the host is not in control of the
        // sample rate for external hardware.
        std::cout << "error: Output device sr: " << info.preferredSampleRate
                  << " does not match sample rate of playback material\n";
        return -1;
      } else {
        _convert(info.preferredSampleRate);
        if (verbose) {
          std::cout << "Converted sr: " << _sampleRate << " source to " << info.preferredSampleRate
                    << " for playback\n";
        }
      }
    }

    RtAudioFormat bufferFormat = _convertedSamples ? RTAUDIO_FLOAT32 : RTAUDIO_FLOAT64;
    if (dac.openStream(&params, nullptr /* input options */, bufferFormat, info.preferredSampleRate,
                       &bufferFrames, &_audioCallback, this, &options)) {
      status = -200;
      goto cleanup;
    }

    if (!dac.isStreamOpen()) {
      status = -201;
      goto cleanup;
    }

    if (dac.startStream()) {
      status = -202;
      goto cleanup;
    }

    std::cerr << "Playing...";
    while (dac.isStreamRunning()) {
      std::this_thread::sleep_for(500ms);
      std::cerr << ".";
    }
    std::cerr << "done. (blocks: " << _blocksOutput << ")\n";

  cleanup:
    if (dac.isStreamOpen()) {
      dac.closeStream();
    }

    return status;
  }

 private:
  const Samples64& _samples;
  const uint32_t _sampleRate;

  uint64_t _playbackOffset;
  uint64_t _blocksOutput;

  std::optional<Samples32> _convertedSamples;
  uint32_t _convertedRate;

  void _convert(uint32_t desiredRate)
  {
    if (_convertedSamples && _convertedRate == desiredRate) {
      return;  // nothing to do
    }

    // determine size of converted audio
    double conversionRatio = static_cast<double>(desiredRate) / static_cast<double>(_sampleRate);
    uint32_t outputFrames =
        static_cast<uint32_t>(std::ceil(static_cast<double>(_samples.size()) * conversionRatio));

    // NOTE: libsamplerate does not support double precision samples so
    // unfortunately a single precision copy of the input samples needs to be
    // created to feed conversion.
    Samples32 original(_samples.begin(), _samples.end());

    // allocate converted audio buffer
    Samples32 converted;
    converted.assign(outputFrames, 0);

    SRC_DATA params;
    params.data_in = original.data();
    params.data_out = converted.data();
    params.input_frames = static_cast<long>(original.size());
    params.output_frames = static_cast<long>(outputFrames);
    params.src_ratio = conversionRatio;

    int status = src_simple(&params, SRC_SINC_BEST_QUALITY, 1);
    if (status != 0) {
      std::cerr << "error: " << src_strerror(status) << std::endl;
      return;
    }

    _convertedSamples = converted;
    _convertedRate = desiredRate;
  }

  template <typename T>
  inline int _outputSamples(void* outputBuffer, unsigned int nFrames, const T& outputSamples)
  {
    uint64_t framesRemaining = outputSamples.size() - _playbackOffset;
    uint64_t framesToCopy = std::min(static_cast<uint64_t>(nFrames), framesRemaining);

    std::memcpy(outputBuffer, &outputSamples[_playbackOffset],
                framesToCopy * sizeof(typename T::value_type));

    _playbackOffset += framesToCopy;
    framesRemaining = outputSamples.size() - _playbackOffset;
    _blocksOutput += 1;
    return framesRemaining > 0 ? 0 /* keep requesting */ : 1 /* drain the buffer and stop */;
  }

  int _output(void* outputBuffer, unsigned int nFrames)
  {
    if (_convertedSamples) {
      return _outputSamples(outputBuffer, nFrames, *_convertedSamples);
    }
    return _outputSamples(outputBuffer, nFrames, _samples);
  }

  static void _errorCallback(RtAudioErrorType /* type */, const std::string& error)
  {
    std::cerr << "error: " << error << "\n";
  }

  static int _audioCallback(void* outputBuffer, void* /* inputBuffer */, unsigned int nFrames,
                            double /* streamTime */, RtAudioStreamStatus /* status */,
                            void* userData)
  {
    return reinterpret_cast<AudioPlayer*>(userData)->_output(outputBuffer, nFrames);
  }
};
