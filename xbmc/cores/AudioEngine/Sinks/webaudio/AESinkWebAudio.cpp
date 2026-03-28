/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AESinkWebAudio.h"

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "utils/log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, wasm_webaudio_init, (int sampleRate, int channels), {
  if (typeof Module !== 'undefined' && Module.webaudioInit)
    Module.webaudioInit(sampleRate, channels);
});

EM_JS(void, wasm_webaudio_write, (const uint8_t* ptr, int bytes), {
  const heap = HEAPU8.subarray(ptr, ptr + bytes);
  if (typeof Module !== 'undefined' && Module.webaudioWrite)
    Module.webaudioWrite(heap);
});
#endif

CAESinkWebAudio::~CAESinkWebAudio()
{
  Deinitialize();
}

bool CAESinkWebAudio::Initialize(AEAudioFormat& format, std::string& device)
{
  format.m_dataFormat = AE_FMT_FLOAT;
  format.m_sampleRate = (format.m_sampleRate > 0) ? format.m_sampleRate : 48000;
  format.m_channelLayout.Reset();
  format.m_channelLayout += AE_CH_FL;
  format.m_channelLayout += AE_CH_FR;
  format.m_frames = 1024;
  format.m_frameSize = format.m_channelLayout.Count() * sizeof(float);

  m_format = format;
  m_initialized = true;
#ifdef __EMSCRIPTEN__
  wasm_webaudio_init(static_cast<int>(format.m_sampleRate),
                     format.m_channelLayout.Count());
#else
  CLog::Log(LOGINFO, "CAESinkWebAudio: stub (non-Emscripten build)");
#endif
  return true;
}

void CAESinkWebAudio::Deinitialize()
{
  m_initialized = false;
}

double CAESinkWebAudio::GetCacheTotal()
{
  return 0.05;
}

double CAESinkWebAudio::GetLatency()
{
  return 0.02;
}

unsigned int CAESinkWebAudio::AddPackets(uint8_t** data,
                                         unsigned int frames,
                                         unsigned int offset)
{
  if (!m_initialized || !data || !data[0])
    return 0;

  const unsigned int frameSize = m_format.m_frameSize;
  void* buffer = data[0] + offset * frameSize;
  const size_t size = static_cast<size_t>(frames) * frameSize;
#ifdef __EMSCRIPTEN__
  wasm_webaudio_write(static_cast<const uint8_t*>(buffer), static_cast<int>(size));
#endif
  return frames;
}

void CAESinkWebAudio::GetDelay(AEDelayStatus& status)
{
  status.delay = 0.0;
}

void CAESinkWebAudio::Drain()
{
}

void CAESinkWebAudio::EnumerateDevicesEx(AEDeviceInfoList& list, bool force)
{
  CAEDeviceInfo info;
  info.m_deviceName = "webaudio";
  info.m_displayName = "Web Audio (browser)";
  info.m_deviceType = AE_DEVTYPE_PCM;
  info.m_wantsIECPassthrough = false;
  info.m_channels += AE_CH_FL;
  info.m_channels += AE_CH_FR;
  info.m_dataFormats.push_back(AE_FMT_FLOAT);
  info.m_dataFormats.push_back(AE_FMT_S16NE);
  info.m_sampleRates.push_back(44100);
  info.m_sampleRates.push_back(48000);
  list.push_back(info);
}

std::unique_ptr<IAESink> CAESinkWebAudio::Create(std::string& device, AEAudioFormat& desiredFormat)
{
  auto sink = std::make_unique<CAESinkWebAudio>();
  if (sink->Initialize(desiredFormat, device))
    return sink;
  return {};
}

void CAESinkWebAudio::Register()
{
  AE::AESinkRegEntry entry;
  entry.sinkName = "WEBAUDIO";
  entry.createFunc = CAESinkWebAudio::Create;
  entry.enumerateFunc = CAESinkWebAudio::EnumerateDevicesEx;
  AE::CAESinkFactory::RegisterSink(entry);
}
