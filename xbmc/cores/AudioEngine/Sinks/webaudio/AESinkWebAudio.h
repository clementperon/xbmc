/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"

#include <atomic>
#include <string>

class CAESinkWebAudio : public IAESink
{
public:
  CAESinkWebAudio() = default;
  ~CAESinkWebAudio() override;

  const char* GetName() override { return "WebAudio"; }

  bool Initialize(AEAudioFormat& format, std::string& device) override;
  void Deinitialize() override;

  double GetCacheTotal() override;
  double GetLatency() override;
  unsigned int AddPackets(uint8_t** data, unsigned int frames, unsigned int offset) override;
  void GetDelay(AEDelayStatus& status) override;
  void Drain() override;

  static void Register();
  static std::unique_ptr<IAESink> Create(std::string& device, AEAudioFormat& desiredFormat);
  static void EnumerateDevicesEx(AEDeviceInfoList& list, bool force);

private:
  AEAudioFormat m_format{};
  std::atomic<bool> m_initialized{false};
};
