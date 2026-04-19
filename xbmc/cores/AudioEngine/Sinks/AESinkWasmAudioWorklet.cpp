/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AESinkWasmAudioWorklet.h"

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "platform/wasm/WasmAudioWorkletManager.h"
#include "utils/log.h"

using KODI::PLATFORM::WASM::CWasmAudioWorkletManager;

void CAESinkWasmAudioWorklet::Register()
{
  AE::AESinkRegEntry entry;
  entry.sinkName = "WASM";
  entry.createFunc = CAESinkWasmAudioWorklet::Create;
  entry.enumerateFunc = CAESinkWasmAudioWorklet::EnumerateDevicesEx;
  entry.cleanupFunc = CAESinkWasmAudioWorklet::Cleanup;
  AE::CAESinkFactory::RegisterSink(entry);
}

void CAESinkWasmAudioWorklet::Cleanup()
{
  CWasmAudioWorkletManager::Instance().Shutdown();
}

std::unique_ptr<IAESink> CAESinkWasmAudioWorklet::Create(std::string& device,
                                                         AEAudioFormat& desiredFormat)
{
  auto sink = std::make_unique<CAESinkWasmAudioWorklet>();
  if (sink->Initialize(desiredFormat, device))
    return sink;

  return {};
}

void CAESinkWasmAudioWorklet::EnumerateDevicesEx(AEDeviceInfoList& list, bool)
{
  list.clear();

  CAEDeviceInfo info;
  info.m_deviceName = "default";
  info.m_displayName = "Browser Audio";
  info.m_displayNameExtra = "Wasm Audio Worklet";
  info.m_deviceType = AE_DEVTYPE_PCM;
  info.m_channels = AE_CH_LAYOUT_7_1;
  info.m_sampleRates = {44100, 48000};
  info.m_dataFormats = {AE_FMT_FLOAT};
  info.m_wantsIECPassthrough = false;
  info.m_onlyPCM = true;
  list.push_back(info);
}

bool CAESinkWasmAudioWorklet::Initialize(AEAudioFormat& format, std::string& device)
{
  if (format.m_dataFormat == AE_FMT_RAW)
  {
    CLog::Log(LOGERROR, "CAESinkWasmAudioWorklet::Initialize - passthrough is not supported");
    return false;
  }

  unsigned int channels = format.m_channelLayout.Count();
  if (channels == 0)
  {
    channels = 2;
    format.m_channelLayout = CAEUtil::GuessChLayout(channels);
  }

  if (!CWasmAudioWorkletManager::Instance().Initialize(channels, format.m_sampleRate))
    return false;

  device = "default";
  format.m_dataFormat = AE_FMT_FLOAT;
  format.m_sampleRate = CWasmAudioWorkletManager::Instance().GetSampleRate();
  format.m_frames = CWasmAudioWorkletManager::Instance().GetQuantumSize();
  format.m_frameSize = channels * static_cast<unsigned int>(sizeof(float));
  m_initialized = true;

  return true;
}

void CAESinkWasmAudioWorklet::Deinitialize()
{
  if (!m_initialized)
    return;

  CWasmAudioWorkletManager::Instance().ResetBuffer();
  m_initialized = false;
}

double CAESinkWasmAudioWorklet::GetCacheTotal()
{
  return CWasmAudioWorkletManager::Instance().GetBufferCapacitySeconds();
}

unsigned int CAESinkWasmAudioWorklet::AddPackets(uint8_t** data, unsigned int frames, unsigned int offset)
{
  if (!m_initialized || !data || !data[0])
    return 0;

  const auto* interleaved = reinterpret_cast<const float*>(data[0]);
  const unsigned int written =
      CWasmAudioWorkletManager::Instance().WriteInterleaved(interleaved, frames, offset);
  DrainUnderrunLog();
  return written;
}

void CAESinkWasmAudioWorklet::GetDelay(AEDelayStatus& status)
{
  status.SetDelay(CWasmAudioWorkletManager::Instance().GetTotalDelaySeconds());
}

void CAESinkWasmAudioWorklet::Drain()
{
  if (!m_initialized)
    return;

  CWasmAudioWorkletManager::Instance().Drain();
  DrainUnderrunLog();
}

void CAESinkWasmAudioWorklet::DrainUnderrunLog()
{
  m_pendingUnderrunFrames += CWasmAudioWorkletManager::Instance().ConsumeUnderrunFrames();
  if (m_pendingUnderrunFrames == 0)
    return;

  const unsigned int sampleRate = CWasmAudioWorkletManager::Instance().GetSampleRate();
  if (sampleRate == 0)
    return;

  constexpr uint64_t LOG_THRESHOLD_MS = 10;
  const uint64_t thresholdFrames = (static_cast<uint64_t>(sampleRate) * LOG_THRESHOLD_MS) / 1000;
  if (thresholdFrames == 0 || m_pendingUnderrunFrames < thresholdFrames)
    return;

  const double missingMs =
      static_cast<double>(m_pendingUnderrunFrames) * 1000.0 / static_cast<double>(sampleRate);
  CLog::Log(LOGWARNING,
            "CAESinkWasmAudioWorklet: worklet underrun, {} frames ({:.2f} ms) of silence "
            "emitted since last report",
            m_pendingUnderrunFrames, missingMs);
  m_pendingUnderrunFrames = 0;
}
