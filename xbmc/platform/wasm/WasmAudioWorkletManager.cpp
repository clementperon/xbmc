/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WasmAudioWorkletManager.h"

#include "utils/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>

#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

namespace
{
constexpr char AUDIO_PROCESSOR_NAME[] = "kodi-audio-worklet";
constexpr unsigned int BUFFER_TARGET_MS = 60;
constexpr unsigned int MAX_CHANNELS = 8;
constexpr unsigned int MIN_BUFFER_QUANTA = 2;
constexpr auto ASYNC_TIMEOUT = std::chrono::seconds(5);

int CreateAudioContextOnMain(int requestedSampleRate)
{
  EmscriptenWebAudioCreateAttributes attrs{};
  attrs.latencyHint = "interactive";
  attrs.sampleRate = static_cast<uint32_t>(std::max(requestedSampleRate, 0));
  attrs.renderSizeHint = AUDIO_CONTEXT_RENDER_SIZE_DEFAULT;
  return emscripten_create_audio_context(&attrs);
}

int GetAudioContextSampleRateOnMain(int audioContext)
{
  return emscripten_audio_context_sample_rate(audioContext);
}

int GetAudioContextQuantumSizeOnMain(int audioContext)
{
  return emscripten_audio_context_quantum_size(audioContext);
}

void StartWorkletThreadOnMain(int audioContext,
                              uintptr_t stackBase,
                              int stackSize,
                              uintptr_t callback,
                              uintptr_t userData)
{
  emscripten_start_wasm_audio_worklet_thread_async(
      audioContext, reinterpret_cast<void*>(stackBase), static_cast<uint32_t>(stackSize),
      reinterpret_cast<EmscriptenStartWebAudioWorkletCallback>(callback),
      reinterpret_cast<void*>(userData));
}

void CreateProcessorOnMain(int audioContext,
                           uintptr_t options,
                           uintptr_t callback,
                           uintptr_t userData)
{
  emscripten_create_wasm_audio_worklet_processor_async(
      audioContext, reinterpret_cast<const WebAudioWorkletProcessorCreateOptions*>(options),
      reinterpret_cast<EmscriptenWorkletProcessorCreatedCallback>(callback),
      reinterpret_cast<void*>(userData));
}

int CreateNodeOnMain(int audioContext,
                     uintptr_t name,
                     uintptr_t options,
                     uintptr_t processCallback,
                     uintptr_t userData)
{
  return emscripten_create_wasm_audio_worklet_node(
      audioContext, reinterpret_cast<const char*>(name),
      reinterpret_cast<const EmscriptenAudioWorkletNodeCreateOptions*>(options),
      reinterpret_cast<EmscriptenWorkletNodeProcessCallback>(processCallback),
      reinterpret_cast<void*>(userData));
}

void ConnectAudioNodeOnMain(int source, int destination, int outputIndex, int inputIndex)
{
  emscripten_audio_node_connect(source, destination, outputIndex, inputIndex);
}

void DestroyAudioContextOnMain(int audioContext)
{
  emscripten_destroy_audio_context(audioContext);
}

void DestroyAudioNodeOnMain(int audioNode)
{
  emscripten_destroy_web_audio_node(audioNode);
}

} // namespace

namespace KODI::PLATFORM::WASM
{
CWasmAudioWorkletManager& CWasmAudioWorkletManager::Instance()
{
  static CWasmAudioWorkletManager instance;
  return instance;
}

bool CWasmAudioWorkletManager::Initialize(unsigned int channels, unsigned int requestedSampleRate)
{
  if (channels == 0 || channels > MAX_CHANNELS)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: unsupported channel count {}", channels);
    return false;
  }

  if (!EnsureContext(requestedSampleRate))
    return false;

  if (!EnsureWorkletProcessor())
    return false;

  if (!ConfigureNode(channels))
    return false;

  EnsureBufferAllocated();
  ResetBuffer();
  InstallResumeHooks();

  m_ready.store(true, std::memory_order_release);
  CLog::Log(LOGINFO, "WASM AudioWorklet: initialized (channels={}, sampleRate={}, quantum={})",
            channels, m_sampleRate.load(std::memory_order_relaxed),
            m_quantumSize.load(std::memory_order_relaxed));
  return true;
}

void CWasmAudioWorkletManager::ResetBuffer()
{
  const uint64_t currentWrite = m_writeFrame.load(std::memory_order_acquire);
  m_readFrame.store(currentWrite, std::memory_order_release);
}

void CWasmAudioWorkletManager::Shutdown()
{
  m_ready.store(false, std::memory_order_release);
  if (m_workletNode != 0)
  {
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, DestroyAudioNodeOnMain,
                                               m_workletNode);
    m_workletNode = 0;
  }
  if (m_audioContext != 0)
  {
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, DestroyAudioContextOnMain,
                                               m_audioContext);
    m_audioContext = 0;
  }

  m_workletThreadCreated.store(false, std::memory_order_release);
  m_processorCreated.store(false, std::memory_order_release);
  m_bufferCapacityFrames = 0;
  m_ringBuffer.clear();
  m_ringBuffer.shrink_to_fit();
  m_readFrame.store(0, std::memory_order_release);
  m_writeFrame.store(0, std::memory_order_release);
}

unsigned int CWasmAudioWorkletManager::WriteInterleaved(const float* source,
                                                        unsigned int frames,
                                                        unsigned int offsetFrames)
{
  if (!source || frames == 0 || !IsReady())
    return 0;

  const unsigned int channels = m_channels.load(std::memory_order_relaxed);
  if (channels == 0 || channels > MAX_CHANNELS || m_bufferCapacityFrames == 0)
    return 0;

  unsigned int writtenFrames = 0;
  while (writtenFrames < frames)
  {
    const uint64_t readFrame = m_readFrame.load(std::memory_order_acquire);
    const uint64_t writeFrame = m_writeFrame.load(std::memory_order_relaxed);
    const uint64_t usedFrames = writeFrame - readFrame;
    if (usedFrames >= m_bufferCapacityFrames)
    {
      emscripten_thread_sleep(1);
      continue;
    }

    const uint64_t availableFrames = m_bufferCapacityFrames - usedFrames;
    const uint64_t toWrite = std::min<uint64_t>(frames - writtenFrames, availableFrames);
    const uint64_t dstStart = writeFrame % m_bufferCapacityFrames;

    for (uint64_t frame = 0; frame < toWrite; ++frame)
    {
      const uint64_t dstFrame = (dstStart + frame) % m_bufferCapacityFrames;
      const size_t dstOffset = static_cast<size_t>(dstFrame * channels);
      const size_t srcOffset =
          static_cast<size_t>((offsetFrames + writtenFrames + frame) * channels);

      for (unsigned int ch = 0; ch < channels; ++ch)
        m_ringBuffer[dstOffset + ch] = source[srcOffset + ch];
    }

    m_writeFrame.store(writeFrame + toWrite, std::memory_order_release);
    writtenFrames += static_cast<unsigned int>(toWrite);
  }

  return writtenFrames;
}

void CWasmAudioWorkletManager::Drain()
{
  constexpr int maxIterations = 5000; // ~5 seconds
  int iteration = 0;
  while (iteration++ < maxIterations && GetBufferedSeconds() > 0.0)
    emscripten_thread_sleep(1);
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

bool CWasmAudioWorkletManager::IsReady() const
{
  return m_ready.load(std::memory_order_acquire);
}

bool CWasmAudioWorkletManager::EnsureContext(unsigned int requestedSampleRate)
{
  if (m_audioContext != 0)
    return true;

  m_audioContext = emscripten_sync_run_in_main_runtime_thread(
      EM_FUNC_SIG_II, CreateAudioContextOnMain, static_cast<int>(requestedSampleRate));

  if (m_audioContext == 0)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create AudioContext");
    return false;
  }

  const int sampleRate = emscripten_sync_run_in_main_runtime_thread(
      EM_FUNC_SIG_II, GetAudioContextSampleRateOnMain, m_audioContext);
  const int quantumSize = emscripten_sync_run_in_main_runtime_thread(
      EM_FUNC_SIG_II, GetAudioContextQuantumSizeOnMain, m_audioContext);

  m_sampleRate.store(static_cast<unsigned int>(sampleRate), std::memory_order_relaxed);
  m_quantumSize.store(static_cast<unsigned int>(quantumSize), std::memory_order_relaxed);

  return true;
}

bool CWasmAudioWorkletManager::EnsureWorkletProcessor()
{
  if (m_audioContext == 0)
    return false;

  alignas(16) static std::array<uint8_t, 16384> workletStack{};

  if (!m_workletThreadCreated.load(std::memory_order_acquire))
  {
    m_asyncDone.store(false, std::memory_order_release);
    m_asyncResult.store(false, std::memory_order_release);

    emscripten_sync_run_in_main_runtime_thread(
        EM_FUNC_SIG_VIIIII, StartWorkletThreadOnMain, m_audioContext,
        reinterpret_cast<uintptr_t>(workletStack.data()), static_cast<int>(workletStack.size()),
        reinterpret_cast<uintptr_t>(&CWasmAudioWorkletManager::OnWorkletThreadStarted),
        reinterpret_cast<uintptr_t>(this));

    if (!WaitForState(m_asyncDone, "thread"))
      return false;

    if (!m_asyncResult.load(std::memory_order_acquire))
    {
      CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create worklet thread");
      return false;
    }
    m_workletThreadCreated.store(true, std::memory_order_release);
  }

  if (!m_processorCreated.load(std::memory_order_acquire))
  {
    WebAudioWorkletProcessorCreateOptions opts{};
    opts.name = AUDIO_PROCESSOR_NAME;

    m_asyncDone.store(false, std::memory_order_release);
    m_asyncResult.store(false, std::memory_order_release);
    emscripten_sync_run_in_main_runtime_thread(
        EM_FUNC_SIG_VIIII, CreateProcessorOnMain, m_audioContext,
        reinterpret_cast<uintptr_t>(&opts),
        reinterpret_cast<uintptr_t>(&CWasmAudioWorkletManager::OnProcessorCreated),
        reinterpret_cast<uintptr_t>(this));

    if (!WaitForState(m_asyncDone, "processor"))
      return false;

    if (!m_asyncResult.load(std::memory_order_acquire))
    {
      CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create processor");
      return false;
    }
    m_processorCreated.store(true, std::memory_order_release);
  }

  return true;
}

bool CWasmAudioWorkletManager::ConfigureNode(unsigned int channels)
{
  m_channels.store(channels, std::memory_order_release);

  if (m_workletNode != 0)
  {
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, DestroyAudioNodeOnMain,
                                               m_workletNode);
    m_workletNode = 0;
  }

  int outputChannelCounts[1] = {static_cast<int>(channels)};
  EmscriptenAudioWorkletNodeCreateOptions options{};
  options.numberOfInputs = 0;
  options.numberOfOutputs = 1;
  options.outputChannelCounts = outputChannelCounts;
  options.channelCount = channels;
  options.channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_EXPLICIT;
  options.channelInterpretation = WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS;

  m_workletNode = emscripten_sync_run_in_main_runtime_thread(
      EM_FUNC_SIG_IIIIII, CreateNodeOnMain, m_audioContext,
      reinterpret_cast<uintptr_t>(AUDIO_PROCESSOR_NAME), reinterpret_cast<uintptr_t>(&options),
      reinterpret_cast<uintptr_t>(
          reinterpret_cast<EmscriptenWorkletNodeProcessCallback>(&CWasmAudioWorkletManager::ProcessAudio)),
      reinterpret_cast<uintptr_t>(this));
  if (m_workletNode == 0)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: failed to create node");
    return false;
  }

  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIIII, ConnectAudioNodeOnMain,
                                             m_workletNode, m_audioContext, 0, 0);
  return true;
}

void CWasmAudioWorkletManager::InstallResumeHooks() const
{
  MAIN_THREAD_ASYNC_EM_ASM(
      ({
        const ctx = emscriptenGetAudioObject($0);
        if (!ctx)
          return;
        if (ctx.state === "running")
          return;
        if (ctx.__kodiResumeHooksInstalled)
          return;

        ctx.__kodiResumeHooksInstalled = true;
        const tryResume = () => {
          if (ctx.state !== "running")
            ctx.resume().catch(() => {});
          if (ctx.state === "running")
          {
            window.removeEventListener("pointerdown", tryResume, true);
            window.removeEventListener("keydown", tryResume, true);
            window.removeEventListener("touchstart", tryResume, true);
            document.removeEventListener("visibilitychange", tryResume, true);
            ctx.__kodiResumeHooksInstalled = false;
          }
        };

        window.addEventListener("pointerdown", tryResume, true);
        window.addEventListener("keydown", tryResume, true);
        window.addEventListener("touchstart", tryResume, true);
        document.addEventListener("visibilitychange", tryResume, true);
      }),
      m_audioContext);
}

void CWasmAudioWorkletManager::EnsureBufferAllocated()
{
  if (m_bufferCapacityFrames > 0)
    return;

  const unsigned int sampleRate = std::max(m_sampleRate.load(std::memory_order_relaxed), 1U);
  const unsigned int quantumSize = std::max(m_quantumSize.load(std::memory_order_relaxed), 1U);
  const unsigned int channels = std::max(m_channels.load(std::memory_order_relaxed), 1U);
  const uint64_t targetFrames =
      (static_cast<uint64_t>(sampleRate) * BUFFER_TARGET_MS + 999ULL) / 1000ULL;
  const uint64_t minFrames = static_cast<uint64_t>(quantumSize) * MIN_BUFFER_QUANTA;
  m_bufferCapacityFrames = static_cast<unsigned int>(std::max(targetFrames, minFrames));
  const size_t sampleCount = static_cast<size_t>(m_bufferCapacityFrames) * channels;
  m_ringBuffer.assign(sampleCount, 0.0f);
}

bool CWasmAudioWorkletManager::WaitForState(std::atomic<bool>& readyFlag, const char* stageName)
{
  std::unique_lock<std::mutex> lock(m_stateMutex);
  const bool completed =
      m_stateCv.wait_for(lock, ASYNC_TIMEOUT, [&readyFlag] { return readyFlag.load(); });
  if (!completed)
  {
    CLog::Log(LOGERROR, "WASM AudioWorklet: timeout waiting for {}", stageName);
    return false;
  }
  return true;
}

void CWasmAudioWorkletManager::OnWorkletThreadStarted(int audioContext, bool success, void* userData)
{
  auto* self = static_cast<CWasmAudioWorkletManager*>(userData);
  if (!self)
    return;

  self->m_asyncResult.store(success && audioContext != 0, std::memory_order_release);
  self->m_asyncDone.store(true, std::memory_order_release);
  self->m_stateCv.notify_all();
}

void CWasmAudioWorkletManager::OnProcessorCreated(int, bool success, void* userData)
{
  auto* self = static_cast<CWasmAudioWorkletManager*>(userData);
  if (!self)
    return;

  self->m_asyncResult.store(success, std::memory_order_release);
  self->m_asyncDone.store(true, std::memory_order_release);
  self->m_stateCv.notify_all();
}

bool CWasmAudioWorkletManager::ProcessAudio(int,
                                            const void*,
                                            int numOutputs,
                                            void* outputs,
                                            int,
                                            const void*,
                                            void* userData)
{
  auto* self = static_cast<CWasmAudioWorkletManager*>(userData);
  if (!self)
    return false;

  return self->ProcessAudioImpl(numOutputs, outputs);
}

bool CWasmAudioWorkletManager::ProcessAudioImpl(int numOutputs, void* outputsRaw)
{
  auto* outputs = static_cast<AudioSampleFrame*>(outputsRaw);
  if (!IsReady() || numOutputs <= 0 || !outputs)
    return true;

  const unsigned int channels = m_channels.load(std::memory_order_relaxed);
  const unsigned int capacityFrames = m_bufferCapacityFrames;
  if (channels == 0 || capacityFrames == 0)
    return true;

  const int samplesPerChannel = outputs[0].samplesPerChannel;
  if (samplesPerChannel <= 0 || !outputs[0].data)
    return true;

  const int outputChannels = outputs[0].numberOfChannels;
  const unsigned int copyChannels = std::min<unsigned int>(channels, std::max(outputChannels, 0));
  float* outputData = outputs[0].data;

  const int totalSamples = outputChannels * samplesPerChannel;
  std::fill(outputData, outputData + totalSamples, 0.0f);

  const uint64_t readFrame = m_readFrame.load(std::memory_order_relaxed);
  const uint64_t writeFrame = m_writeFrame.load(std::memory_order_acquire);
  const uint64_t availableFrames = writeFrame - readFrame;
  const uint64_t toRead = std::min<uint64_t>(availableFrames, static_cast<uint64_t>(samplesPerChannel));
  if (toRead == 0)
    return true;

  const uint64_t srcStart = readFrame % capacityFrames;
  for (uint64_t frame = 0; frame < toRead; ++frame)
  {
    const uint64_t srcFrame = (srcStart + frame) % capacityFrames;
    const size_t srcOffset = static_cast<size_t>(srcFrame * channels);
    for (unsigned int ch = 0; ch < copyChannels; ++ch)
    {
      outputData[ch * samplesPerChannel + frame] = m_ringBuffer[srcOffset + ch];
    }
  }

  m_readFrame.store(readFrame + toRead, std::memory_order_release);
  return true;
}
} // namespace KODI::PLATFORM::WASM
