//
// Copyright (c) 2021 Greg Wuller.
//
// SPDX-License-Identifier: GPL-2.0-or-later
//

#pragma once

#include <sndfile.h>

#include <filesystem>
#include <optional>
#include <vector>

class AudioFile final
{
 public:
  using Samples = std::vector<double>;

  enum Mode {
    READ,
    WRITE,
  };

  enum Format { WAV, AIFF, CAF };

  enum Encoding {
    PCM_16,
    PCM_24,
    PCM_32,
    FLOAT,
    DOUBLE,
  };

  static std::optional<Format> inferFormat(const std::filesystem::path& p);
  static std::optional<Encoding> inferEncoding(const std::string& s);

  static AudioFile forRead(const std::filesystem::path& p);
  static AudioFile forWrite(const std::filesystem::path& p, uint32_t sampleRate,
                            uint16_t channels = 1, Format format = Format::WAV,
                            Encoding encoding = Encoding::PCM_24);

  ~AudioFile();

  AudioFile(const AudioFile&) = delete;
  AudioFile& operator=(const AudioFile&) = delete;

  AudioFile(AudioFile&& other);
  AudioFile& operator=(AudioFile&& other);

  const std::filesystem::path& path() const { return _path; };
  Mode mode() const { return _mode; };

  Samples& samples();

  int sampleRate() const;
  int channels() const;
  int64_t frames() const;

  void write();
  void write(const Samples& samples);
  void close();

 private:
  AudioFile(const std::filesystem::path& p, Mode m) : _path(p), _mode(m), _file(nullptr){};

  void _loadSamples();

  std::filesystem::path _path;
  Mode _mode;
  Samples _samples;

  SNDFILE* _file;
  SF_INFO _info;
};
