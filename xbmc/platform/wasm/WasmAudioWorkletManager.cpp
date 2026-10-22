/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WasmAudioWorkletManager.h"

#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

#include <emscripten/threading.h>

extern "C"
{
  // audio_worklet.js
  int wasm_audio_open(int sampleRate, void* runningFlag, int* sampleRateOut, int* quantumSizeOut);
  void wasm_audio_start(int channels,
                        void* ring,
                        int capacityFrames,
                        int prebufferFrames,
                        void* ready,
                        void* prebufferComplete,
                        void* activeCallbacks,
                        void* readFrame,
                        void* writeFrame,
                        void* underrunFrames,
                        void* state);
  void wasm_audio_close();
  int wasm_audio_latency_us();
  int wasm_audio_max_output_channels();
}

namespace
{
constexpr unsigned int BUFFER_TARGET_MS = 100;
constexpr unsigned int MIN_BUFFER_QUANTA = 2;
constexpr unsigned int PREBUFFER_TARGET_MS = 40;
constexpr unsigned int WORKLET_STALL_LIMIT_MS = 500;
constexpr auto ASYNC_TIMEOUT = std::chrono::seconds(5);

constexpr uint32_t NODE_PENDING = 0;
constexpr uint32_t NODE_STARTED = 1;

// The processor accesses these as plain shared memory with JS Atomics.
static_assert(std::atomic<bool>::is_always_lock_free && sizeof(std::atomic<bool>) == 1);
static_assert(std::atomic<unsigned int>::is_always_lock_free &&
              sizeof(std::atomic<unsigned int>) == 4);
static_assert(std::atomic<uint64_t>::is_always_lock_free && sizeof(std::atomic<uint64_t>) == 8);
} // namespace

namespace KODI::PLATFORM::WASM
{
CWasmAudioWorkletManager& CWasmAudioWorkletManager::GetInstance()
{
  static CWasmAudioWorkletManager instance;
  return instance;
}

bool CWasmAudioWorkletManager::Initialize(unsigned int channels, unsigned int requestedSampleRate)
{
  if (channels == 0 || channels > kMaxChannels)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: unsupported channel count {}", channels);
    return false;
  }

  if (m_contextOpen)
    Shutdown();

  m_ready.store(false, std::memory_order_release);
  m_channels.store(channels, std::memory_order_release);

  if (!OpenContext(requestedSampleRate))
  {
    Shutdown();
    return false;
  }

  EnsureBufferAllocated();
  ResetBuffer();

  if (!StartNode())
  {
    Shutdown();
    return false;
  }

  RefreshPipelineLatency();

  m_ready.store(true, std::memory_order_release);
  const unsigned int sampleRateNow = std::max(m_sampleRate.load(std::memory_order_relaxed), 1U);
  const double prebufMs =
      static_cast<double>(m_prebufferFrames) * 1000.0 / static_cast<double>(sampleRateNow);
  CLog::Log(LOGINFO,
            "WASM AudioWorklet: initialized (channels={}, sampleRate={}, quantum={}, "
            "pipelineLatency={:.3f} ms, ringCapacity={:.3f} ms, prebuffer={:.1f} ms)",
            channels, m_sampleRate.load(std::memory_order_relaxed),
            m_quantumSize.load(std::memory_order_relaxed),
            GetPipelineLatencySeconds() * 1000.0, GetBufferCapacitySeconds() * 1000.0,
            prebufMs);
  return true;
}

void CWasmAudioWorkletManager::ResetBuffer()
{
  const uint64_t currentWrite = m_writeFrame.load(std::memory_order_acquire);
  m_readFrame.store(currentWrite, std::memory_order_release);
  m_underrunFrames.store(0, std::memory_order_relaxed);
  m_prebufferComplete.store(false, std::memory_order_release);
}

void CWasmAudioWorkletManager::Shutdown()
{
  WaitForWorkletIdle();

  if (m_contextOpen)
  {
    wasm_audio_close();
    m_contextOpen = false;
  }

  m_bufferCapacityFrames = 0;
  m_bufferChannels = 0;
  m_bufferSampleRate = 0;
  m_bufferQuantumSize = 0;
  m_prebufferFrames = 0;
  m_prebufferComplete.store(false, std::memory_order_release);
  m_ringBuffer.clear();
  m_ringBuffer.shrink_to_fit();
  m_readFrame.store(0, std::memory_order_release);
  m_writeFrame.store(0, std::memory_order_release);
  m_pipelineLatencyUs.store(0, std::memory_order_relaxed);
  m_underrunFrames.store(0, std::memory_order_relaxed);
}

unsigned int CWasmAudioWorkletManager::WritePlanar(const float* const* planes,
                                                   unsigned int channels,
                                                   unsigned int frames,
                                                   unsigned int offsetFrames)
{
  if (!planes || frames == 0 || !IsReady())
    return 0;

  const unsigned int configuredChannels = m_channels.load(std::memory_order_relaxed);
  if (configuredChannels == 0 || configuredChannels > kMaxChannels)
    return 0;

  const unsigned int copyChannels = std::min(channels, configuredChannels);
  if (copyChannels == 0)
    return 0;

  const int quantumMs = QuantumSleepMs();

  unsigned int writtenFrames = 0;
  unsigned int stalledMs = 0;
  while (writtenFrames < frames)
  {
    if (!IsReady())
      return writtenFrames;

    const unsigned int capacityFrames = m_bufferCapacityFrames;
    if (capacityFrames == 0 || m_ringBuffer.empty())
      return writtenFrames;

    const uint64_t readFrame = m_readFrame.load(std::memory_order_acquire);
    const uint64_t writeFrame = m_writeFrame.load(std::memory_order_relaxed);
    const uint64_t usedFrames = writeFrame - readFrame;
    if (usedFrames >= capacityFrames)
    {
      // A context without a user gesture never drains the ring, and ActiveAE
      // treats repeated zero writes as a dead sink.
      if (!m_contextRunning.load(std::memory_order_acquire))
      {
        emscripten_thread_sleep(static_cast<double>(frames - writtenFrames) * 1000.0 /
                                static_cast<double>(std::max(m_sampleRate.load(), 1U)));
        return frames;
      }
      stalledMs += static_cast<unsigned int>(quantumMs);
      if (stalledMs >= WORKLET_STALL_LIMIT_MS)
        return writtenFrames;
      emscripten_thread_sleep(quantumMs);
      continue;
    }
    stalledMs = 0;

    const uint64_t freeFrames = capacityFrames - usedFrames;
    const uint64_t toWrite = std::min<uint64_t>(frames - writtenFrames, freeFrames);
    const uint64_t dstStart = writeFrame % capacityFrames;
    const uint64_t firstChunk = std::min<uint64_t>(toWrite, capacityFrames - dstStart);
    const uint64_t secondChunk = toWrite - firstChunk;

    for (unsigned int ch = 0; ch < copyChannels; ++ch)
    {
      if (!planes[ch])
        return writtenFrames;
      float* const dstPlane = &m_ringBuffer[static_cast<size_t>(ch) * capacityFrames];
      const float* const srcPlane = planes[ch] + offsetFrames + writtenFrames;
      std::memcpy(dstPlane + dstStart, srcPlane, firstChunk * sizeof(float));
      if (secondChunk > 0)
        std::memcpy(dstPlane, srcPlane + firstChunk, secondChunk * sizeof(float));
    }
    // Zero any ring planes the writer did not fill (e.g. upmix headroom) so
    // stale data never leaks into the browser output.
    for (unsigned int ch = copyChannels; ch < configuredChannels; ++ch)
    {
      float* const dstPlane = &m_ringBuffer[static_cast<size_t>(ch) * capacityFrames];
      std::memset(dstPlane + dstStart, 0, firstChunk * sizeof(float));
      if (secondChunk > 0)
        std::memset(dstPlane, 0, secondChunk * sizeof(float));
    }

    m_writeFrame.store(writeFrame + toWrite, std::memory_order_release);
    writtenFrames += static_cast<unsigned int>(toWrite);
  }

  return writtenFrames;
}

// Sleeping for one audio quantum wakes a waiting thread right when the worklet has
// consumed a block, rather than busy-looping on 1 ms sleeps.
int CWasmAudioWorkletManager::QuantumSleepMs() const
{
  const unsigned int sampleRate = std::max(m_sampleRate.load(std::memory_order_relaxed), 1U);
  const unsigned int quantum = std::max(m_quantumSize.load(std::memory_order_relaxed), 1U);
  return std::max(1,
                  static_cast<int>((static_cast<uint64_t>(quantum) * 1000ULL + sampleRate - 1ULL) /
                                   sampleRate));
}

void CWasmAudioWorkletManager::Drain()
{
  const int quantumMs = QuantumSleepMs();
  uint64_t lastReadFrame = m_readFrame.load(std::memory_order_acquire);
  unsigned int stalledMs = 0;

  while (IsReady() && GetBufferedSeconds() > 0.0)
  {
    emscripten_thread_sleep(quantumMs);

    const uint64_t readFrame = m_readFrame.load(std::memory_order_acquire);
    if (readFrame != lastReadFrame)
    {
      lastReadFrame = readFrame;
      stalledMs = 0;
      continue;
    }

    // An AudioContext that no user gesture has resumed yet never drains the ring,
    // so stop waiting rather than hold up the sink thread until a fixed deadline.
    stalledMs += static_cast<unsigned int>(quantumMs);
    if (stalledMs >= WORKLET_STALL_LIMIT_MS)
      return;
  }

  // The browser still holds the pipeline latency's worth of audio after the ring empties.
  if (IsReady() && m_contextRunning.load(std::memory_order_acquire))
  {
    RefreshPipelineLatency();
    emscripten_thread_sleep(GetPipelineLatencySeconds() * 1000.0);
  }
}

double CWasmAudioWorkletManager::GetBufferedSeconds() const
{
  const unsigned int rate = m_sampleRate.load(std::memory_order_relaxed);
  if (rate == 0)
    return 0.0;

  const uint64_t readFrame = m_readFrame.load(std::memory_order_acquire);
  const uint64_t writeFrame = m_writeFrame.load(std::memory_order_relaxed);
  const uint64_t queuedFrames = writeFrame - readFrame;
  return static_cast<double>(queuedFrames) / static_cast<double>(rate);
}

double CWasmAudioWorkletManager::GetBufferCapacitySeconds() const
{
  const unsigned int rate = m_sampleRate.load(std::memory_order_relaxed);
  if (rate == 0 || m_bufferCapacityFrames == 0)
    return 0.0;

  return static_cast<double>(m_bufferCapacityFrames) / static_cast<double>(rate);
}

double CWasmAudioWorkletManager::GetPipelineLatencySeconds() const
{
  return static_cast<double>(m_pipelineLatencyUs.load(std::memory_order_relaxed)) / 1'000'000.0;
}

double CWasmAudioWorkletManager::GetTotalDelaySeconds() const
{
  return GetBufferedSeconds() + GetPipelineLatencySeconds();
}

uint64_t CWasmAudioWorkletManager::ConsumeUnderrunFrames()
{
  return m_underrunFrames.exchange(0, std::memory_order_relaxed);
}

unsigned int CWasmAudioWorkletManager::GetSampleRate() const
{
  return m_sampleRate.load(std::memory_order_relaxed);
}

unsigned int CWasmAudioWorkletManager::GetQuantumSize() const
{
  return m_quantumSize.load(std::memory_order_relaxed);
}

unsigned int CWasmAudioWorkletManager::GetChannels() const
{
  return m_channels.load(std::memory_order_relaxed);
}

unsigned int CWasmAudioWorkletManager::GetMaxOutputChannels() const
{
  const int channels = wasm_audio_max_output_channels();
  return channels > 0 ? static_cast<unsigned int>(channels) : 2U;
}

// seq_cst pairs with the processor, which increments m_activeCallbacks before loading
// m_ready: either the worklet sees the flag or we see the callback and wait for it.
void CWasmAudioWorkletManager::WaitForWorkletIdle()
{
  m_ready.store(false, std::memory_order_seq_cst);
  for (unsigned int i = 0; i < 100 && m_activeCallbacks.load(std::memory_order_seq_cst) > 0; ++i)
    emscripten_thread_sleep(1);
}

bool CWasmAudioWorkletManager::IsReady() const
{
  return m_ready.load(std::memory_order_acquire);
}

bool CWasmAudioWorkletManager::OpenContext(unsigned int requestedSampleRate)
{
  int sampleRate = 0;
  int quantumSize = 0;
  if (!wasm_audio_open(static_cast<int>(requestedSampleRate), &m_contextRunning, &sampleRate,
                       &quantumSize))
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create AudioContext");
    return false;
  }
  m_contextOpen = true;

  m_sampleRate.store(static_cast<unsigned int>(sampleRate), std::memory_order_relaxed);
  m_quantumSize.store(static_cast<unsigned int>(quantumSize), std::memory_order_relaxed);
  return true;
}

bool CWasmAudioWorkletManager::StartNode()
{
  m_nodeState.store(NODE_PENDING, std::memory_order_relaxed);
  wasm_audio_start(static_cast<int>(m_channels.load(std::memory_order_relaxed)),
                   m_ringBuffer.data(), static_cast<int>(m_bufferCapacityFrames),
                   static_cast<int>(m_prebufferFrames), &m_ready, &m_prebufferComplete,
                   &m_activeCallbacks, &m_readFrame, &m_writeFrame, &m_underrunFrames,
                   &m_nodeState);

  const auto deadline = std::chrono::steady_clock::now() + ASYNC_TIMEOUT;
  uint32_t state;
  while ((state = m_nodeState.load(std::memory_order_acquire)) == NODE_PENDING)
  {
    const double remainingMs =
        std::chrono::duration<double, std::milli>(deadline - std::chrono::steady_clock::now())
            .count();
    if (remainingMs <= 0.0)
    {
      CLog::Log(LOGERROR, "WASM AudioWorklet: timeout waiting for the processor");
      return false;
    }
    emscripten_futex_wait(&m_nodeState, NODE_PENDING, remainingMs);
  }

  if (state != NODE_STARTED)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create node");
    return false;
  }
  return true;
}

void CWasmAudioWorkletManager::RefreshPipelineLatency()
{
  if (!m_contextOpen)
    return;

  const int latencyUs = wasm_audio_latency_us();
  if (latencyUs < 0)
    return;

  m_pipelineLatencyUs.store(static_cast<uint32_t>(latencyUs), std::memory_order_relaxed);
}

void CWasmAudioWorkletManager::EnsureBufferAllocated()
{
  const unsigned int sampleRate = std::max(m_sampleRate.load(std::memory_order_relaxed), 1U);
  const unsigned int quantumSize = std::max(m_quantumSize.load(std::memory_order_relaxed), 1U);
  const unsigned int channels = std::max(m_channels.load(std::memory_order_relaxed), 1U);
  const uint64_t targetFrames =
      (static_cast<uint64_t>(sampleRate) * BUFFER_TARGET_MS + 999ULL) / 1000ULL;
  const uint64_t minFrames = static_cast<uint64_t>(quantumSize) * MIN_BUFFER_QUANTA;
  const unsigned int capacityFrames =
      static_cast<unsigned int>(std::max(targetFrames, minFrames));
  if (m_bufferCapacityFrames == capacityFrames && m_bufferChannels == channels &&
      m_bufferSampleRate == sampleRate && m_bufferQuantumSize == quantumSize)
    return;

  WaitForWorkletIdle();
  m_bufferCapacityFrames = capacityFrames;
  m_bufferChannels = channels;
  m_bufferSampleRate = sampleRate;
  m_bufferQuantumSize = quantumSize;
  const size_t sampleCount = static_cast<size_t>(capacityFrames) * channels;
  m_ringBuffer.assign(sampleCount, 0.0f);

  // Compute the prebuffer watermark. Clamp to half the ring so we always
  // have room for new writes before output resumes.
  const uint64_t prebuf =
      (static_cast<uint64_t>(sampleRate) * PREBUFFER_TARGET_MS + 999ULL) / 1000ULL;
  const uint64_t quantaInPrebuf = (prebuf + quantumSize - 1ULL) / quantumSize;
  const uint64_t prebufRounded = quantaInPrebuf * quantumSize;
  m_prebufferFrames =
      static_cast<unsigned int>(std::min<uint64_t>(prebufRounded, capacityFrames / 2));
  m_prebufferComplete.store(false, std::memory_order_release);
}

} // namespace KODI::PLATFORM::WASM
