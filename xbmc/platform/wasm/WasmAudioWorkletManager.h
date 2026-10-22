/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace KODI::PLATFORM::WASM
{
class CWasmAudioWorkletManager
{
public:
  static constexpr unsigned int kMaxChannels = 8;

  static CWasmAudioWorkletManager& GetInstance();

  bool Initialize(unsigned int channels, unsigned int requestedSampleRate);
  void Shutdown();

  unsigned int WritePlanar(const float* const* planes,
                           unsigned int channels,
                           unsigned int frames,
                           unsigned int offsetFrames);
  void Drain();

  double GetBufferedSeconds() const;
  double GetBufferCapacitySeconds() const;
  double GetPipelineLatencySeconds() const;
  double GetTotalDelaySeconds() const;
  void RefreshPipelineLatency();

  unsigned int GetSampleRate() const;
  unsigned int GetQuantumSize() const;
  unsigned int GetChannels() const;
  unsigned int GetMaxOutputChannels() const;
  bool IsReady() const;

  // Returns and resets the number of underrun frames.
  uint64_t ConsumeUnderrunFrames();

private:
  CWasmAudioWorkletManager() = default;
  ~CWasmAudioWorkletManager() = default;
  CWasmAudioWorkletManager(const CWasmAudioWorkletManager&) = delete;
  CWasmAudioWorkletManager& operator=(const CWasmAudioWorkletManager&) = delete;

  bool OpenContext(unsigned int requestedSampleRate);
  bool StartNode();
  void EnsureBufferAllocated();
  void ResetBuffer();
  void WaitForWorkletIdle();
  int QuantumSleepMs() const;

  bool m_contextOpen{false};

  std::atomic<bool> m_ready{false};
  // 0 while audio_worklet.js starts the node, then 1 on success or 2 on failure.
  std::atomic<uint32_t> m_nodeState{0};

  std::atomic<unsigned int> m_channels{2};
  std::atomic<unsigned int> m_sampleRate{48000};
  std::atomic<unsigned int> m_quantumSize{128};

  // SPSC ring: the sink thread writes m_writeFrame and (re)allocates, only after
  // WaitForWorkletIdle(); the processor in audio_worklet.js reads the ring and
  // writes m_readFrame through raw wasm memory.
  std::vector<float> m_ringBuffer;
  unsigned int m_bufferCapacityFrames{0};
  unsigned int m_bufferChannels{0};
  unsigned int m_bufferSampleRate{0};
  unsigned int m_bufferQuantumSize{0};
  std::atomic<uint64_t> m_readFrame{0};
  std::atomic<uint64_t> m_writeFrame{0};

  // Written by the main thread through Atomics.store().
  std::atomic<bool> m_contextRunning{false};

  std::atomic<uint32_t> m_pipelineLatencyUs{0};

  std::atomic<uint64_t> m_underrunFrames{0};
  std::atomic<unsigned int> m_activeCallbacks{0};

  // Prebuffer watermark
  unsigned int m_prebufferFrames{0};
  std::atomic<bool> m_prebufferComplete{false};
};
} // namespace KODI::PLATFORM::WASM
