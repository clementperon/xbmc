/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "DVDVideoCodecWebCodecs.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDStreamInfo.h"
#include "DVDVideoCodecWebCodecsBridge.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <emscripten/bind.h>

// Expose the bridge enums to JavaScript via Embind so the JS bridge can read
// the canonical C++ values (Module.WebCodecsPixelFormat.NV12 etc.) instead of
// duplicating them. Must live at global scope, hence outside any namespace.
EMSCRIPTEN_BINDINGS(kodi_webcodecs_bridge)
{
  emscripten::enum_<WebCodecsPixelFormat>("WebCodecsPixelFormat")
      .value("UNKNOWN", WEBCODECS_PIXFMT_UNKNOWN)
      .value("YUV420P", WEBCODECS_PIXFMT_YUV420P)
      .value("NV12", WEBCODECS_PIXFMT_NV12);

  emscripten::enum_<WebCodecsPushStatus>("WebCodecsPushStatus")
      .value("QUEUED", WEBCODECS_PUSH_QUEUED)
      .value("EMPTY", WEBCODECS_PUSH_EMPTY)
      .value("HANDLE_NOT_FOUND", WEBCODECS_PUSH_HANDLE_NOT_FOUND)
      .value("DECODER_FAILED", WEBCODECS_PUSH_DECODER_FAILED)
      .value("NOT_CONFIGURED", WEBCODECS_PUSH_NOT_CONFIGURED)
      .value("DECODE_THREW", WEBCODECS_PUSH_DECODE_THREW)
      .value("BUSY", WEBCODECS_PUSH_BUSY);
}

namespace
{
constexpr int INVALID_DECODER_HANDLE = 0;
constexpr int DEFAULT_ALIGNMENT = 64;
constexpr int DECODER_ERROR_BUFFER_SIZE = 512;
constexpr int MIN_PROBE_DIMENSION = 64;
constexpr int STRIDE_ALIGNMENT = 2;
constexpr int DISPLAY_WIDTH_ALIGN_MASK = -3;
constexpr int DRAIN_POLL_LIMIT = 120;
constexpr int DROPPED_FRAMES_LOG_THRESHOLD = 8;
constexpr int PICTURE_COLOR_BITS = 8;

constexpr int CODEC_STRING_BUFFER_SIZE = 24;
constexpr int H264_AVCC_MIN_EXTRADATA_SIZE = 7;
constexpr uint8_t H264_AVCC_CONFIG_VERSION = 1;
constexpr int H264_AVCC_PROFILE_OFFSET = 1;
constexpr int H264_AVCC_COMPAT_OFFSET = 2;
constexpr int H264_AVCC_LEVEL_OFFSET = 3;
constexpr int H264_AVCC_LENGTH_SIZE_OFFSET = 4;
constexpr uint8_t H264_AVCC_LENGTH_SIZE_MASK = 0x03;

constexpr uint8_t H264_NAL_TYPE_MASK = 0x1F;
constexpr uint8_t H264_NAL_TYPE_IDR = 5;

constexpr uint8_t ANNEXB_START_CODE_BYTE = 0x01;

int AlignUp(int value, int alignment)
{
  return ((value + alignment - 1) / alignment) * alignment;
}

std::string BuildH264CodecString(const CDVDStreamInfo& hints)
{
  const auto& extradata = hints.extradata;
  if (extradata.GetSize() >= H264_AVCC_LEVEL_OFFSET + 1 &&
      extradata.GetData()[0] == H264_AVCC_CONFIG_VERSION)
  {
    const uint8_t profile = extradata.GetData()[H264_AVCC_PROFILE_OFFSET];
    const uint8_t compat = extradata.GetData()[H264_AVCC_COMPAT_OFFSET];
    const uint8_t level = extradata.GetData()[H264_AVCC_LEVEL_OFFSET];

    char buffer[CODEC_STRING_BUFFER_SIZE];
    std::snprintf(buffer, sizeof(buffer), "avc1.%02X%02X%02X", profile, compat, level);
    return buffer;
  }

  return "avc1.42E01E";
}

int ClampTwoDigitCodecValue(int value, int fallback)
{
  if (value < 0)
    value = fallback;
  return std::clamp(value, 0, 99);
}

int VP9ProfileFromHints(const CDVDStreamInfo& hints)
{
  switch (hints.profile)
  {
    case AV_PROFILE_VP9_0:
      return 0;
    case AV_PROFILE_VP9_1:
      return 1;
    case AV_PROFILE_VP9_2:
      return 2;
    case AV_PROFILE_VP9_3:
      return 3;
    default:
      return hints.bitdepth > 8 ? 2 : 0;
  }
}

std::string BuildVP9CodecString(const CDVDStreamInfo& hints)
{
  const int profile = ClampTwoDigitCodecValue(VP9ProfileFromHints(hints), 0);
  const int level = ClampTwoDigitCodecValue(hints.level, 10);
  const int bitDepth = ClampTwoDigitCodecValue(hints.bitdepth > 0 ? hints.bitdepth : 8, 8);

  char buffer[CODEC_STRING_BUFFER_SIZE];
  std::snprintf(buffer, sizeof(buffer), "vp09.%02d.%02d.%02d", profile, level, bitDepth);
  return buffer;
}

// Returns true if the H.264 sample contains an IDR NAL unit.
bool H264SampleContainsIDR(const uint8_t* data, int size, int nalLengthSize)
{
  if (!data || size <= 0)
    return false;

  if (nalLengthSize > 0)
  {
    int offset = 0;
    while (offset + nalLengthSize <= size)
    {
      uint32_t nalSize = 0;
      for (int i = 0; i < nalLengthSize; ++i)
        nalSize = (nalSize << 8) | data[offset + i];
      offset += nalLengthSize;
      if (nalSize == 0 || offset + static_cast<int>(nalSize) > size)
        break;
      if ((data[offset] & H264_NAL_TYPE_MASK) == H264_NAL_TYPE_IDR)
        return true;
      offset += nalSize;
    }
    return false;
  }

  for (int i = 0; i + 3 < size; ++i)
  {
    const bool threeByteStart = data[i] == 0x00 && data[i + 1] == 0x00 &&
                                data[i + 2] == ANNEXB_START_CODE_BYTE;
    const bool fourByteStart = data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
                               data[i + 3] == ANNEXB_START_CODE_BYTE;
    if (!threeByteStart && !fourByteStart)
      continue;

    const int nalStart = threeByteStart ? i + 3 : i + 4;
    if (nalStart < size && (data[nalStart] & H264_NAL_TYPE_MASK) == H264_NAL_TYPE_IDR)
      return true;
  }
  return false;
}

bool VP8SampleIsKeyFrame(const uint8_t* data, int size)
{
  return data && size > 0 && (data[0] & 0x01) == 0;
}

bool VP9SampleIsKeyFrame(const uint8_t* data, int size)
{
  if (!data || size <= 0)
    return false;

  const uint8_t firstByte = data[0];
  // VP9 uncompressed headers are bit-packed LSB-first. Bits 0-1 are the
  // frame marker, bits 2-3 the profile, then show_existing_frame and frame_type.
  if ((firstByte & 0x03) != 0x02)
    return false;

  int bit = 4;
  const int profile = (firstByte >> 2) & 0x03;
  if (profile == 3)
    ++bit; // reserved profile bit

  const bool showExistingFrame = (firstByte & (1U << bit)) != 0;
  if (showExistingFrame)
    return false;
  ++bit;

  const bool interFrame = (firstByte & (1U << bit)) != 0;
  return !interFrame;
}

bool PacketIsKeyFrame(const CDVDStreamInfo& hints,
                      const uint8_t* data,
                      int size,
                      int nalLengthSize)
{
  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      return H264SampleContainsIDR(data, size, nalLengthSize);
    case AV_CODEC_ID_VP8:
      return VP8SampleIsKeyFrame(data, size);
    case AV_CODEC_ID_VP9:
      return VP9SampleIsKeyFrame(data, size);
    default:
      return true;
  }
}

// Reads any pending diagnostic string from the JS decoder side.
std::string ReadDecoderError(int decoderHandle)
{
  if (decoderHandle == INVALID_DECODER_HANDLE)
    return {};
  char buffer[DECODER_ERROR_BUFFER_SIZE];
  const int written = webcodecs_take_error(decoderHandle, buffer, sizeof(buffer));
  if (written <= 0)
    return {};
  buffer[sizeof(buffer) - 1] = '\0';
  return std::string(buffer);
}

const char* PushStatusToString(int status)
{
  switch (status)
  {
    case WEBCODECS_PUSH_QUEUED:
      return "queued";
    case WEBCODECS_PUSH_EMPTY:
      return "empty packet";
    case WEBCODECS_PUSH_HANDLE_NOT_FOUND:
      return "decoder handle not found";
    case WEBCODECS_PUSH_DECODER_FAILED:
      return "decoder in failed state";
    case WEBCODECS_PUSH_NOT_CONFIGURED:
      return "decoder not configured";
    case WEBCODECS_PUSH_DECODE_THREW:
      return "decode threw";
    case WEBCODECS_PUSH_BUSY:
      return "decoder busy (backpressure)";
  }
  return "unknown";
}

AVPixelFormat PixelFormatFromWebCodecs(int pixelFormat)
{
  switch (pixelFormat)
  {
    case WEBCODECS_PIXFMT_YUV420P:
      return AV_PIX_FMT_YUV420P;
    case WEBCODECS_PIXFMT_NV12:
      return AV_PIX_FMT_NV12;
    case WEBCODECS_PIXFMT_UNKNOWN:
    default:
      return AV_PIX_FMT_NONE;
  }
}
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
  return hints.codec == AV_CODEC_ID_H264 || hints.codec == AV_CODEC_ID_VP8 ||
         hints.codec == AV_CODEC_ID_VP9;
}

bool CDVDVideoCodecWebCodecs::BuildCodecConfiguration(const CDVDStreamInfo& hints)
{
  m_codecString.clear();
  m_annexB = false;
  m_nalLengthSize = 0;

  if (hints.codec == AV_CODEC_ID_H264)
  {
    m_codecString = BuildH264CodecString(hints);
    const auto& extradata = hints.extradata;
    const bool haveAVCC = extradata.GetSize() >= H264_AVCC_MIN_EXTRADATA_SIZE &&
                          extradata.GetData()[0] == H264_AVCC_CONFIG_VERSION;
    if (haveAVCC)
    {
      m_annexB = false;
      m_nalLengthSize =
          (extradata.GetData()[H264_AVCC_LENGTH_SIZE_OFFSET] & H264_AVCC_LENGTH_SIZE_MASK) + 1;
    }
    else
    {
      m_annexB = true;
      m_nalLengthSize = 0;
    }
    return true;
  }

  if (hints.codec == AV_CODEC_ID_VP9)
  {
    m_codecString = BuildVP9CodecString(hints);
    return true;
  }

  if (hints.codec == AV_CODEC_ID_VP8)
  {
    m_codecString = "vp8";
    return true;
  }

  return false;
}

bool CDVDVideoCodecWebCodecs::CreateDecoder()
{
  const uint8_t* extraData = m_hints.extradata.GetData();
  const int extraDataSize = static_cast<int>(m_hints.extradata.GetSize());

  m_decoderHandle = webcodecs_create_decoder(m_codecString.c_str(), m_hints.width, m_hints.height,
                                             extraData, extraDataSize, m_annexB ? 1 : 0);
  if (m_decoderHandle == INVALID_DECODER_HANDLE)
  {
    CLog::Log(LOGDEBUG,
              "CDVDVideoCodecWebCodecs::CreateDecoder - unable to configure decoder for {} "
              "(annexB={}, extradataSize={}, {}x{}): check that VideoDecoder is available and "
              "that the codec/description match the stream",
              m_codecString, m_annexB, extraDataSize, m_hints.width, m_hints.height);
    return false;
  }

  return true;
}

void CDVDVideoCodecWebCodecs::Dispose()
{
  if (m_decoderHandle != INVALID_DECODER_HANDLE)
  {
    webcodecs_destroy_decoder(m_decoderHandle);
    m_decoderHandle = INVALID_DECODER_HANDLE;
  }
  m_opened = false;
  m_eof = false;
  m_waitingForKeyFrame = true;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;
  m_lastLoggedDroppedFrames = 0;
  m_highWaterMark = 0;
  m_bufferPixelFormat = AV_PIX_FMT_YUV420P;
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
  m_lastLoggedDroppedFrames = 0;
  m_highWaterMark = 0;
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

  const bool isKeyFrame = packet.recoveryPoint ||
                          PacketIsKeyFrame(m_hints, packet.pData, packet.iSize, m_nalLengthSize);

  // Feeding a delta before the first IDR would permanently fail the decoder.
  if (m_waitingForKeyFrame && !isKeyFrame)
    return true;

  double ptsSeconds = 0.0;
  if (packet.pts != DVD_NOPTS_VALUE && std::isfinite(packet.pts))
    ptsSeconds = packet.pts / static_cast<double>(DVD_TIME_BASE);

  double durationSeconds = 0.0;
  if (packet.duration > 0.0 && std::isfinite(packet.duration))
    durationSeconds = packet.duration / static_cast<double>(DVD_TIME_BASE);

  const int status = webcodecs_push_packet(m_decoderHandle, packet.pData, packet.iSize,
                                           isKeyFrame ? 1 : 0, ptsSeconds, durationSeconds);

  // Backpressure: decoder is saturated, let VideoPlayer re-queue this packet.
  // Don't clear EOF/drain state; the packet wasn't consumed.
  if (status == WEBCODECS_PUSH_BUSY)
    return false;

  m_eof = false;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;

  if (status <= 0)
  {
    if (status == WEBCODECS_PUSH_EMPTY)
      return true;

    const std::string error = ReadDecoderError(m_decoderHandle);
    CLog::Log(LOGDEBUG,
              "CDVDVideoCodecWebCodecs::AddData - decode rejected packet (size={}, pts={:.6f}s, "
              "key={}, status={} [{}]): {}",
              packet.iSize, ptsSeconds, isKeyFrame, status, PushStatusToString(status),
              error.empty() ? "<no js error>" : error);
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

  webcodecs_reset_decoder(m_decoderHandle);
  m_eof = false;
  m_waitingForKeyFrame = true;
  m_drainSubmitted = false;
  m_drainPollsWithoutFrames = 0;
  m_lastLoggedDroppedFrames = 0;
  m_highWaterMark = 0;
  CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::Reset - decoder reset after seek/flush");
}

void CDVDVideoCodecWebCodecs::SetCodecControl(int flags)
{
  m_codecControlFlags = flags;
}

void CDVDVideoCodecWebCodecs::PollDecoderStats()
{
  if (m_decoderHandle == INVALID_DECODER_HANDLE)
    return;

  int droppedFrames = 0;
  int highWaterMark = 0;
  if (!webcodecs_read_stats(m_decoderHandle, &droppedFrames, &highWaterMark))
    return;

  if (highWaterMark > m_highWaterMark)
    m_highWaterMark = highWaterMark;

  if ((droppedFrames - m_lastLoggedDroppedFrames) < DROPPED_FRAMES_LOG_THRESHOLD)
    return;

  CLog::Log(LOGWARNING,
            "CDVDVideoCodecWebCodecs::GetPicture - dropped {} queued WebCodecs frames "
            "(totalDropped={}, queueHighWater={})",
            droppedFrames - m_lastLoggedDroppedFrames, droppedFrames, m_highWaterMark);
  m_lastLoggedDroppedFrames = droppedFrames;
}

bool CDVDVideoCodecWebCodecs::AcquirePictureBuffer(AVPixelFormat pixelFormat,
                                                   int width,
                                                   int height,
                                                   int bufferSize,
                                                   CVideoBuffer*& outBuffer)
{
  m_videoBufferPool->Configure(pixelFormat, AlignUp(bufferSize, DEFAULT_ALIGNMENT));
  outBuffer = m_videoBufferPool->Get();
  if (!outBuffer || !outBuffer->GetMemPtr())
    return false;
  return true;
}

void CDVDVideoCodecWebCodecs::FillPictureMetadata(VideoPicture* pVideoPicture,
                                                  CVideoBuffer* videoBuffer,
                                                  AVPixelFormat pixelFormat,
                                                  int width,
                                                  int height,
                                                  bool keyFrame,
                                                  double ptsSeconds,
                                                  double durationSeconds) const
{
  pVideoPicture->Reset();
  pVideoPicture->videoBuffer = videoBuffer;
  pVideoPicture->pixelFormat = pixelFormat;
  pVideoPicture->iWidth = width;
  pVideoPicture->iHeight = height;

  double aspect = m_hints.aspect > 0.0 ? m_hints.aspect
                                       : (height > 0 ? static_cast<double>(width) / height : 1.0);
  pVideoPicture->iDisplayWidth =
      static_cast<int>(std::lrint(height * aspect)) & DISPLAY_WIDTH_ALIGN_MASK;
  pVideoPicture->iDisplayHeight = height;
  if (pVideoPicture->iDisplayWidth > static_cast<unsigned int>(width))
  {
    pVideoPicture->iDisplayWidth = width;
    pVideoPicture->iDisplayHeight =
        static_cast<int>(std::lrint(width / aspect)) & DISPLAY_WIDTH_ALIGN_MASK;
  }

  // VideoPicture::pts / iDuration are in DVD_TIME_BASE units (microseconds).
  pVideoPicture->pts = std::isfinite(ptsSeconds) ? ptsSeconds * DVD_TIME_BASE : DVD_NOPTS_VALUE;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
  pVideoPicture->iDuration = durationSeconds > 0.0 ? durationSeconds * DVD_TIME_BASE : 0.0;
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
  pVideoPicture->colorBits = PICTURE_COLOR_BITS;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecWebCodecs::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_opened || m_decoderHandle == INVALID_DECODER_HANDLE)
    return VC_ERROR;

  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
  {
    if (!m_drainSubmitted)
    {
      webcodecs_drain_decoder(m_decoderHandle);
      m_drainSubmitted = true;
      m_drainPollsWithoutFrames = 0;
      CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::GetPicture - drain requested");
    }
  }
  else if (m_drainSubmitted)
  {
    // Input resumed after a stillframe/drain - clear the EOF state.
    m_drainSubmitted = false;
    m_drainPollsWithoutFrames = 0;
    m_eof = false;
  }

  CVideoBuffer* videoBuffer = nullptr;
  // YUV420P and NV12 both need width*height*3/2 bytes.
  const int probeWidth = std::max(m_hints.width, MIN_PROBE_DIMENSION);
  const int probeHeight = std::max(m_hints.height, MIN_PROBE_DIMENSION);
  const int probeStride = AlignUp(probeWidth, STRIDE_ALIGNMENT);
  const int probeBufferSize = probeStride * probeHeight * 3 / 2;

  if (!AcquirePictureBuffer(m_bufferPixelFormat, probeWidth, probeHeight, probeBufferSize,
                            videoBuffer))
  {
    return VC_NOBUFFER;
  }

  WebCodecsFrameInfo info{};
  int copyStatus =
      webcodecs_copy_next_frame(m_decoderHandle, videoBuffer->GetMemPtr(), probeBufferSize, &info);
  if (m_drainSubmitted)
    PollDecoderStats();

  if (copyStatus == -2)
  {
    videoBuffer->Release();
    const AVPixelFormat requiredPixelFormat = PixelFormatFromWebCodecs(info.pixelFormat);
    if (requiredPixelFormat == AV_PIX_FMT_NONE || info.payloadSize <= 0)
    {
      CLog::Log(LOGERROR,
                "CDVDVideoCodecWebCodecs::GetPicture - invalid frame metadata for larger "
                "WebCodecs frame (pixelFormat={}, payloadSize={})",
                info.pixelFormat, info.payloadSize);
      return VC_ERROR;
    }

    if (!AcquirePictureBuffer(requiredPixelFormat, info.width, info.height, info.payloadSize,
                              videoBuffer))
    {
      return VC_NOBUFFER;
    }

    copyStatus =
        webcodecs_copy_next_frame(m_decoderHandle, videoBuffer->GetMemPtr(), info.payloadSize, &info);
    if (copyStatus <= 0)
    {
      videoBuffer->Release();
      return copyStatus == 0 ? VC_BUFFER : VC_ERROR;
    }
  }

  if (copyStatus == 0)
  {
    videoBuffer->Release();
    if (m_drainSubmitted)
    {
      const int pendingWork = webcodecs_drain_decoder(m_decoderHandle);
      if (pendingWork <= 0)
      {
        m_eof = true;
        CLog::Log(LOGDEBUG, "CDVDVideoCodecWebCodecs::GetPicture - drain completed (EOF)");
        return VC_EOF;
      }

      if (++m_drainPollsWithoutFrames >= DRAIN_POLL_LIMIT)
      {
        m_eof = true;
        CLog::Log(LOGWARNING,
                  "CDVDVideoCodecWebCodecs::GetPicture - drain timed out with {} pending "
                  "WebCodecs operations",
                  pendingWork);
        return VC_EOF;
      }
    }
    return VC_BUFFER;
  }

  if (copyStatus < 0)
  {
    videoBuffer->Release();
    const std::string error = ReadDecoderError(m_decoderHandle);
    CLog::Log(LOGERROR, "CDVDVideoCodecWebCodecs::GetPicture - decoder entered failed state: {}",
              error.empty() ? "<no js error>" : error);
    return VC_ERROR;
  }

  const AVPixelFormat pixelFormat = PixelFormatFromWebCodecs(info.pixelFormat);
  if (pixelFormat == AV_PIX_FMT_NONE)
  {
    videoBuffer->Release();
    CLog::Log(LOGERROR, "CDVDVideoCodecWebCodecs::GetPicture - unsupported pixel format id {}",
              info.pixelFormat);
    return VC_ERROR;
  }

  m_bufferPixelFormat = pixelFormat;
  const int strides[YuvImage::MAX_PLANES] = {info.yStride, info.uStride, info.vStride};
  const int planeOffsets[YuvImage::MAX_PLANES] = {0, info.uOffset, info.vOffset};
  videoBuffer->SetPixelFormat(pixelFormat);
  videoBuffer->SetDimensions(info.width, info.height, strides, planeOffsets);

  FillPictureMetadata(pVideoPicture, videoBuffer, pixelFormat, info.width, info.height,
                      info.keyFrame != 0, info.ptsSeconds, info.durationSeconds);
  m_eof = false;
  m_drainPollsWithoutFrames = 0;
  return VC_PICTURE;
}
