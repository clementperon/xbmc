/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "DVDVideoCodecWebCodecs.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDStreamInfo.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <emscripten.h>

namespace
{
constexpr int INVALID_DECODER_HANDLE = 0;
constexpr int DEFAULT_ALIGNMENT = 64;

int AlignUp(int value, int alignment)
{
  return ((value + alignment - 1) / alignment) * alignment;
}

std::string BuildH264CodecString(const CDVDStreamInfo& hints)
{
  if (hints.extradata.GetSize() >= 4 && hints.extradata.GetData()[0] == 1)
  {
    const uint8_t profile = hints.extradata.GetData()[1];
    const uint8_t compat = hints.extradata.GetData()[2];
    const uint8_t level = hints.extradata.GetData()[3];

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "avc1.%02X%02X%02X", profile, compat, level);
    return buffer;
  }

  return "avc1.42E01E";
}

EM_JS(int, WebCodecsCreateDecoder, (const char* codecPtr,
                                    int width,
                                    int height,
                                    const uint8_t* extraDataPtr,
                                    int extraDataSize,
                                    int annexB), {
  if (typeof VideoDecoder === 'undefined' || typeof EncodedVideoChunk === 'undefined')
    return 0;

  Module.__kodiWebCodecs = Module.__kodiWebCodecs || {
    nextId: 1,
    decoders: new Map(),
  };

  const codec = UTF8ToString(codecPtr);
  const decoders = Module.__kodiWebCodecs.decoders;
  const id = Module.__kodiWebCodecs.nextId++;

  const state = {
    id,
    codec,
    failed: false,
    errorMessage: "",
    lastTimestamp: 0,
    frames: [],
    decoder: null,
  };

  const outputCallback = (frame) => {
    const codedWidth = frame.codedWidth;
    const codedHeight = frame.codedHeight;
    const yStride = codedWidth;
    const uvWidth = (codedWidth + 1) >> 1;
    const uvHeight = (codedHeight + 1) >> 1;
    const uvStride = uvWidth;
    const ySize = yStride * codedHeight;
    const uSize = uvStride * uvHeight;
    const vSize = uvStride * uvHeight;
    const payload = new Uint8Array(ySize + uSize + vSize);
    const layout = [
      { offset: 0, stride: yStride },
      { offset: ySize, stride: uvStride },
      { offset: ySize + uSize, stride: uvStride },
    ];

    const timestampMicroseconds = Number.isFinite(frame.timestamp)
      ? Number(frame.timestamp) : state.lastTimestamp;
    const durationMicroseconds = Number.isFinite(frame.duration)
      ? Number(frame.duration) : 0;

    state.lastTimestamp = timestampMicroseconds;

    (async () => {
      try
      {
        await frame.copyTo(payload, { layout, format: 'I420' });
        state.frames.push({
          payload,
          width: codedWidth,
          height: codedHeight,
          yStride,
          uStride: uvStride,
          vStride: uvStride,
          ptsSeconds: timestampMicroseconds / 1000000.0,
          durationSeconds: durationMicroseconds / 1000000.0,
          keyFrame: frame.type === 'key',
        });

        if (state.frames.length > 8)
          state.frames.shift();
      }
      catch (e)
      {
        state.failed = true;
        state.errorMessage = `frame copy failed: ${String(e)}`;
      }
      finally
      {
        frame.close();
      }
    })();
  };

  const errorCallback = (error) => {
    state.failed = true;
    state.errorMessage = String(error);
  };

  try
  {
    const config = {
      codec,
      codedWidth: width > 0 ? width : undefined,
      codedHeight: height > 0 ? height : undefined,
      optimizeForLatency: true,
      hardwareAcceleration: 'prefer-hardware',
    };

    if (codec.startsWith('avc1'))
      config.avc = { format: annexB ? 'annexb' : 'avc' };

    if (extraDataSize > 0)
      config.description = HEAPU8.slice(extraDataPtr, extraDataPtr + extraDataSize);

    state.decoder = new VideoDecoder({
      output: outputCallback,
      error: errorCallback,
    });
    state.decoder.configure(config);
    decoders.set(id, state);
    return id;
  }
  catch (e)
  {
    console.warn('WASM WebCodecs: create/configure decoder failed', e);
    return 0;
  }
});

EM_JS(int, WebCodecsDestroyDecoder, (int decoderId), {
  const registry = Module.__kodiWebCodecs;
  if (!registry)
    return 0;

  const state = registry.decoders.get(decoderId);
  if (!state)
    return 0;

  try
  {
    state.decoder.close();
  }
  catch (e)
  {
    console.warn('WASM WebCodecs: decoder close failed', e);
  }

  registry.decoders.delete(decoderId);
  return 1;
});

EM_JS(int, WebCodecsResetDecoder, (int decoderId), {
  const state = Module.__kodiWebCodecs?.decoders.get(decoderId);
  if (!state)
    return 0;

  try
  {
    state.decoder.reset();
    state.frames.length = 0;
    state.failed = false;
    state.errorMessage = "";
    return 1;
  }
  catch (e)
  {
    state.failed = true;
    state.errorMessage = `reset failed: ${String(e)}`;
    return 0;
  }
});

EM_JS(int, WebCodecsPushPacket, (int decoderId,
                                 const uint8_t* dataPtr,
                                 int dataSize,
                                 int keyFrame,
                                 double ptsSeconds,
                                 double durationSeconds), {
  const state = Module.__kodiWebCodecs?.decoders.get(decoderId);
  if (!state || state.failed || !state.decoder)
    return 0;

  if (dataSize <= 0)
    return 1;

  const timestampMicros = Math.max(0, Math.round(ptsSeconds * 1000000.0));
  const durationMicros = Math.max(0, Math.round(durationSeconds * 1000000.0));
  const payload = HEAPU8.slice(dataPtr, dataPtr + dataSize);

  try
  {
    state.decoder.decode(new EncodedVideoChunk({
      type: keyFrame ? 'key' : 'delta',
      timestamp: timestampMicros,
      duration: durationMicros > 0 ? durationMicros : undefined,
      data: payload,
    }));
    return 1;
  }
  catch (e)
  {
    state.failed = true;
    state.errorMessage = `decode failed: ${String(e)}`;
    return 0;
  }
});

EM_JS(int, WebCodecsDrainDecoder, (int decoderId), {
  const state = Module.__kodiWebCodecs?.decoders.get(decoderId);
  if (!state || !state.decoder)
    return 0;

  try
  {
    state.decoder.flush().catch((error) => {
      state.failed = true;
      state.errorMessage = `flush failed: ${String(error)}`;
    });
    return 1;
  }
  catch (e)
  {
    state.failed = true;
    state.errorMessage = `flush failed: ${String(e)}`;
    return 0;
  }
});

EM_JS(int, WebCodecsCopyNextFrame, (int decoderId,
                                    uint8_t* dstPtr,
                                    int dstSize,
                                    int* widthPtr,
                                    int* heightPtr,
                                    int* yStridePtr,
                                    int* uStridePtr,
                                    int* vStridePtr,
                                    int* keyFramePtr,
                                    double* ptsSecondsPtr,
                                    double* durationSecondsPtr), {
  const state = Module.__kodiWebCodecs?.decoders.get(decoderId);
  if (!state)
    return 0;

  if (state.failed)
    return -1;

  if (state.frames.length === 0)
    return 0;

  const frame = state.frames[0];
  if (!frame || frame.payload.byteLength > dstSize)
    return 0;

  HEAPU8.set(frame.payload, dstPtr);
  HEAP32[widthPtr >> 2] = frame.width;
  HEAP32[heightPtr >> 2] = frame.height;
  HEAP32[yStridePtr >> 2] = frame.yStride;
  HEAP32[uStridePtr >> 2] = frame.uStride;
  HEAP32[vStridePtr >> 2] = frame.vStride;
  HEAP32[keyFramePtr >> 2] = frame.keyFrame ? 1 : 0;
  HEAPF64[ptsSecondsPtr >> 3] = frame.ptsSeconds;
  HEAPF64[durationSecondsPtr >> 3] = frame.durationSeconds;

  state.frames.shift();
  return 1;
});
} // namespace

CDVDVideoCodecWebCodecs::CDVDVideoCodecWebCodecs(CProcessInfo& processInfo)
  : CDVDVideoCodec(processInfo)
{
  m_videoBufferPool = std::make_shared<CVideoBufferPoolSysMem>();
}

CDVDVideoCodecWebCodecs::~CDVDVideoCodecWebCodecs()
{
  Dispose();
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecWebCodecs::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecWebCodecs>(processInfo);
}

bool CDVDVideoCodecWebCodecs::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("webcodecs_dec", CDVDVideoCodecWebCodecs::Create);
  return true;
}

bool CDVDVideoCodecWebCodecs::SupportsCodec(const CDVDStreamInfo& hints) const
{
  return hints.codec == AV_CODEC_ID_H264 || hints.codec == AV_CODEC_ID_VP9;
}

bool CDVDVideoCodecWebCodecs::BuildCodecConfiguration(const CDVDStreamInfo& hints)
{
  m_codecString.clear();
  m_annexB = false;

  if (hints.codec == AV_CODEC_ID_H264)
  {
    m_codecString = BuildH264CodecString(hints);
    if (!(hints.extradata.GetSize() >= 4 && hints.extradata.GetData()[0] == 1))
      m_annexB = true;
    return true;
  }

  if (hints.codec == AV_CODEC_ID_VP9)
  {
    m_codecString = "vp09.00.10.08";
    return true;
  }

  return false;
}

bool CDVDVideoCodecWebCodecs::CreateDecoder()
{
  const uint8_t* extraData = m_hints.extradata.GetData();
  const int extraDataSize = static_cast<int>(m_hints.extradata.GetSize());

  m_decoderHandle = WebCodecsCreateDecoder(m_codecString.c_str(), m_hints.width, m_hints.height,
                                           extraData, extraDataSize, m_annexB ? 1 : 0);
  if (m_decoderHandle == INVALID_DECODER_HANDLE)
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::CreateDecoder - unable to configure decoder for {}",
              m_codecString);
    return false;
  }

  return true;
}

void CDVDVideoCodecWebCodecs::Dispose()
{
  if (m_decoderHandle != INVALID_DECODER_HANDLE)
  {
    WebCodecsDestroyDecoder(m_decoderHandle);
    m_decoderHandle = INVALID_DECODER_HANDLE;
  }
  m_opened = false;
  m_eof = false;
  m_waitingForKeyFrame = true;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;
}

bool CDVDVideoCodecWebCodecs::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (!SupportsCodec(hints))
    return false;

  (void)options;
  Dispose();

  m_hints = hints;
  if (!BuildCodecConfiguration(hints))
    return false;

  if (!CreateDecoder())
    return false;

  m_name = "webcodecs-" + m_codecString;
  m_opened = true;
  m_eof = false;
  m_waitingForKeyFrame = true;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;
  m_codecControlFlags = 0;
  m_processInfo.SetVideoDecoderName(m_name, true);
  m_processInfo.SetVideoDimensions(hints.width, hints.height);
  CLog::Log(LOGINFO,
            "CDVDVideoCodecWebCodecs::Open - Using WebCodecs for video decoding: {} ({}x{}, annexB={})",
            m_codecString, hints.width, hints.height, m_annexB);
  return true;
}

bool CDVDVideoCodecWebCodecs::AddData(const DemuxPacket& packet)
{
  if (!m_opened || m_decoderHandle == INVALID_DECODER_HANDLE)
    return false;

  if (packet.iSize <= 0 || packet.pData == nullptr)
    return true;

  m_eof = false;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;

  const bool isKeyFrame = packet.recoveryPoint || m_waitingForKeyFrame;
  if (!WebCodecsPushPacket(m_decoderHandle, packet.pData, packet.iSize, isKeyFrame ? 1 : 0, packet.pts,
                           packet.duration))
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::AddData - decode rejected packet");
    return false;
  }

  if (isKeyFrame)
    m_waitingForKeyFrame = false;

  return true;
}

void CDVDVideoCodecWebCodecs::Reset()
{
  if (m_decoderHandle == INVALID_DECODER_HANDLE)
    return;

  WebCodecsResetDecoder(m_decoderHandle);
  m_eof = false;
  m_waitingForKeyFrame = true;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;
  CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::Reset - decoder reset after seek/flush");
}

void CDVDVideoCodecWebCodecs::SetCodecControl(int flags)
{
  m_codecControlFlags = flags;
}

bool CDVDVideoCodecWebCodecs::AcquirePictureBuffer(int width,
                                                   int height,
                                                   int yStride,
                                                   int uStride,
                                                   int vStride,
                                                   CVideoBuffer*& outBuffer,
                                                   int (&outPlaneOffsets)[YuvImage::MAX_PLANES],
                                                   int& outBufferSize)
{
  const int chromaHeight = (height + 1) >> 1;
  const int ySize = yStride * height;
  const int uSize = uStride * chromaHeight;
  const int vSize = vStride * chromaHeight;
  outBufferSize = ySize + uSize + vSize;

  m_videoBufferPool->Configure(AV_PIX_FMT_YUV420P, AlignUp(outBufferSize, DEFAULT_ALIGNMENT));
  outBuffer = m_videoBufferPool->Get();
  if (!outBuffer || !outBuffer->GetMemPtr())
    return false;

  const int strides[YuvImage::MAX_PLANES] = {yStride, uStride, vStride};
  outPlaneOffsets[0] = 0;
  outPlaneOffsets[1] = ySize;
  outPlaneOffsets[2] = ySize + uSize;
  outBuffer->SetPixelFormat(AV_PIX_FMT_YUV420P);
  outBuffer->SetDimensions(width, height, strides, outPlaneOffsets);
  return true;
}

void CDVDVideoCodecWebCodecs::FillPictureMetadata(VideoPicture* pVideoPicture,
                                                  CVideoBuffer* videoBuffer,
                                                  int width,
                                                  int height,
                                                  bool keyFrame,
                                                  double ptsSeconds,
                                                  double durationSeconds) const
{
  pVideoPicture->Reset();
  pVideoPicture->videoBuffer = videoBuffer;
  pVideoPicture->pixelFormat = AV_PIX_FMT_YUV420P;
  pVideoPicture->iWidth = width;
  pVideoPicture->iHeight = height;

  double aspect = m_hints.aspect > 0.0 ? m_hints.aspect
                                       : (height > 0 ? static_cast<double>(width) / height : 1.0);
  pVideoPicture->iDisplayWidth = (static_cast<int>(std::lrint(height * aspect))) & -3;
  pVideoPicture->iDisplayHeight = height;
  if (pVideoPicture->iDisplayWidth > static_cast<unsigned int>(width))
  {
    pVideoPicture->iDisplayWidth = width;
    pVideoPicture->iDisplayHeight = (static_cast<int>(std::lrint(width / aspect))) & -3;
  }

  pVideoPicture->pts = ptsSeconds;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
  pVideoPicture->iDuration = durationSeconds;
  pVideoPicture->iRepeatPicture = 0.0;
  pVideoPicture->iFlags = 0;
  pVideoPicture->iFrameType = keyFrame ? FRAME_TYPE_I : FRAME_TYPE_P;

  pVideoPicture->color_space = m_hints.colorSpace == AVCOL_SPC_UNSPECIFIED
                                   ? AVCOL_SPC_BT709
                                   : m_hints.colorSpace;
  pVideoPicture->color_primaries = m_hints.colorPrimaries == AVCOL_PRI_UNSPECIFIED
                                       ? AVCOL_PRI_BT709
                                       : m_hints.colorPrimaries;
  pVideoPicture->color_transfer =
      m_hints.colorTransferCharacteristic == AVCOL_TRC_UNSPECIFIED
          ? AVCOL_TRC_BT709
          : m_hints.colorTransferCharacteristic;
  pVideoPicture->m_originalColorPrimaries = pVideoPicture->color_primaries;
  pVideoPicture->color_range = m_hints.colorRange == AVCOL_RANGE_JPEG;
  pVideoPicture->chroma_position = AVCHROMA_LOC_LEFT;
  pVideoPicture->colorBits = 8;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecWebCodecs::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_opened || m_decoderHandle == INVALID_DECODER_HANDLE)
    return VC_ERROR;

  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
  {
    if (!m_drainSubmitted)
    {
      WebCodecsDrainDecoder(m_decoderHandle);
      m_drainSubmitted = true;
      m_drainPollsWithoutFrames = 0;
      CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::GetPicture - drain requested");
    }
  }

  int width = 0;
  int height = 0;
  int yStride = 0;
  int uStride = 0;
  int vStride = 0;
  int keyFrame = 0;
  double ptsSeconds = DVD_NOPTS_VALUE;
  double durationSeconds = 0.0;

  CVideoBuffer* videoBuffer = nullptr;
  int planeOffsets[YuvImage::MAX_PLANES] = {0, 0, 0};
  int frameSize = 0;

  // Probe with a conservative allocation from stream hints first.
  const int probeWidth = std::max(m_hints.width, 64);
  const int probeHeight = std::max(m_hints.height, 64);
  const int probeYStride = AlignUp(probeWidth, 2);
  const int probeUStride = AlignUp((probeWidth + 1) / 2, 2);
  const int probeVStride = probeUStride;

  if (!AcquirePictureBuffer(probeWidth, probeHeight, probeYStride, probeUStride, probeVStride,
                            videoBuffer, planeOffsets, frameSize))
  {
    return VC_NOBUFFER;
  }

  int copyStatus =
      WebCodecsCopyNextFrame(m_decoderHandle, videoBuffer->GetMemPtr(), frameSize, &width, &height, &yStride,
                             &uStride, &vStride, &keyFrame, &ptsSeconds, &durationSeconds);

  if (copyStatus == 0)
  {
    videoBuffer->Release();
    if (m_drainSubmitted)
    {
      constexpr int drainPollLimit = 8;
      if (++m_drainPollsWithoutFrames >= drainPollLimit)
      {
        m_eof = true;
        CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::GetPicture - drain completed (EOF)");
        return VC_EOF;
      }
    }
    return VC_BUFFER;
  }

  if (copyStatus < 0)
  {
    videoBuffer->Release();
    return VC_ERROR;
  }

  // Reapply actual dimensions/strides from JS copy.
  const int strides[YuvImage::MAX_PLANES] = {yStride, uStride, vStride};
  const int ySize = yStride * height;
  const int uSize = uStride * ((height + 1) >> 1);
  planeOffsets[0] = 0;
  planeOffsets[1] = ySize;
  planeOffsets[2] = ySize + uSize;
  videoBuffer->SetPixelFormat(AV_PIX_FMT_YUV420P);
  videoBuffer->SetDimensions(width, height, strides, planeOffsets);

  FillPictureMetadata(pVideoPicture, videoBuffer, width, height, keyFrame != 0, ptsSeconds,
                      durationSeconds);
  m_eof = false;
  m_drainPollsWithoutFrames = 0;
  return VC_PICTURE;
}
