
//
// Copyright (c) 2021 Greg Wuller.
//
// SPDX-License-Identifier: GPL-2.0-or-later
//

#include "AudioFile.h"

#include <cassert>
#include <cstring>
#include <limits>

std::optional<AudioFile::Format> AudioFile::inferFormat(const std::filesystem::path& p)
{
  std::string e = p.extension();
  if (e == ".wav") {
    return AudioFile::Format::WAV;
  }
  if (e == ".aiff") {
    return AudioFile::Format::AIFF;
  }
  if (e == ".caf") {
    return AudioFile::Format::CAF;
  }

  return {};
}

std::optional<AudioFile::Encoding> AudioFile::inferEncoding(const std::string& s)
{
  if (s == "16") {
    return AudioFile::Encoding::PCM_16;
  }
  if (s == "24") {
    return AudioFile::Encoding::PCM_24;
  }
  if (s == "32") {
    return AudioFile::Encoding::PCM_32;
  }
  if (s == "f32") {
    return AudioFile::Encoding::FLOAT;
  }
  if (s == "f64") {
    return AudioFile::Encoding::DOUBLE;
  }

  return {};
}

AudioFile AudioFile::forRead(const std::filesystem::path& p)
{
  AudioFile file(p, Mode::READ);

  file._info.format = 0;
  file._file = sf_open(p.c_str(), SFM_READ, &file._info);
  // FIXME: need proper error handling
  assert(file._file != nullptr);

  return file;
}

AudioFile AudioFile::forWrite(const std::filesystem::path& p, uint32_t sampleRate,
                              uint16_t channels, Format format, Encoding encoding)
{
  AudioFile file(p, Mode::WRITE);

  int fmt = 0;

  switch (format) {
    case Format::WAV:
      fmt = SF_FORMAT_WAV;
      break;
    case Format::AIFF:
      fmt = SF_FORMAT_AIFF;
      break;
    case Format::CAF:
      fmt = SF_FORMAT_CAF;
      break;
  }

  switch (encoding) {
    case Encoding::PCM_16:
      fmt |= SF_FORMAT_PCM_16;
      break;
    case Encoding::PCM_24:
      fmt |= SF_FORMAT_PCM_24;
      break;
    case Encoding::PCM_32:
      fmt |= SF_FORMAT_PCM_32;
      break;
    case Encoding::FLOAT:
      fmt |= SF_FORMAT_FLOAT;
      break;
    case Encoding::DOUBLE:
      fmt |= SF_FORMAT_DOUBLE;
      break;
  }

  assert(fmt != 0);
  file._info.format = fmt;

  assert(sampleRate <= std::numeric_limits<int>::max());
  file._info.samplerate = static_cast<int>(sampleRate);
  file._info.channels = channels;

  file._file = sf_open(p.c_str(), SFM_WRITE, &file._info);
  assert(file._file != nullptr);

  return file;
}

AudioFile::~AudioFile() { close(); }

AudioFile::AudioFile(AudioFile&& other)
{
  _path = other._path;
  _mode = other._mode;
  _file = other._file;
  _info = other._info;
  _samples = other._samples;

  other._file = nullptr;
  memset(&other._info, 0, sizeof(SF_INFO));
}

AudioFile& AudioFile::operator=(AudioFile&& other)
{
  if (this != &other) {
    // close any file handle which might be open in this instance
    close();

    _path = other._path;
    _mode = other._mode;
    _file = other._file;
    _info = other._info;
    _samples = other._samples;

    other._file = nullptr;
    memset(&other._info, 0, sizeof(SF_INFO));
  }

  return *this;
}

std::vector<double>& AudioFile::samples()
{
  if (_mode == Mode::READ && _samples.size() == 0) {
    _loadSamples();
  }
  return _samples;
}

int AudioFile::sampleRate() const { return _file ? _info.samplerate : 0; }

int AudioFile::channels() const { return _file ? _info.channels : 0; }

int64_t AudioFile::frames() const { return _file ? _info.frames : 0; }

void AudioFile::write(const Samples& samples)
{
  if (_file) {
    sf_count_t sampleCount = static_cast<sf_count_t>(samples.size());
    sf_count_t wrote = sf_write_double(_file, samples.data(), sampleCount);
    assert(wrote == sampleCount);
  }
}

void AudioFile::write() { write(_samples); }

void AudioFile::close()
{
  if (_file) {
    // FIXME: check for errors
    sf_close(_file);
  }
}

void AudioFile::_loadSamples()
{
  if (_file) {
    // TODO: generalize to handle files with multiple channels
    assert(_info.channels == 1);

    // ensure the down cast to reserve size will not overflow
    assert(_info.frames >= 0);
    assert(std::numeric_limits<sf_count_t>::max() <=
           std::numeric_limits<Samples::size_type>::max());

    // assign (as opposed to reserve) so that the vector size reflects the size of the data being
    // written into the backing memory
    _samples.assign(static_cast<Samples::size_type>(_info.frames), 0.0);

    sf_count_t read =
        sf_read_double(_file, _samples.data(), static_cast<sf_count_t>(_samples.capacity()));
    assert(read == _info.frames);
  }
}
