/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"

#include <memory>
#include <string>

class CVideoBufferPoolSysMem;

class CDVDVideoCodecWebCodecs : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecWebCodecs(CProcessInfo& processInfo);
  ~CDVDVideoCodecWebCodecs() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static bool Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return m_name.c_str(); }
  void SetCodecControl(int flags) override;

private:
  bool CreateDecoder();
  void Dispose();
  bool SupportsCodec(const CDVDStreamInfo& hints) const;
  bool BuildCodecConfiguration(const CDVDStreamInfo& hints);
  void PollDecoderStats();
  bool AcquirePictureBuffer(int width,
                            int height,
                            int yStride,
                            int uStride,
                            int vStride,
                            CVideoBuffer*& outBuffer,
                            int (&outPlaneOffsets)[YuvImage::MAX_PLANES],
                            int& outBufferSize);
  void FillPictureMetadata(VideoPicture* pVideoPicture,
                           CVideoBuffer* videoBuffer,
                           int width,
                           int height,
                           bool keyFrame,
                           double ptsSeconds,
                           double durationSeconds) const;

  std::string m_name{"webcodecs"};
  CDVDStreamInfo m_hints;
  std::shared_ptr<CVideoBufferPoolSysMem> m_videoBufferPool;

  int m_decoderHandle{0};
  bool m_opened{false};
  bool m_eof{false};
  bool m_drainSubmitted{false};
  int m_drainPollsWithoutFrames{0};
  bool m_waitingForKeyFrame{true};
  bool m_annexB{false};
  int m_codecControlFlags{0};
  int m_lastLoggedDroppedFrames{0};
  int m_highWaterMark{0};

  std::string m_codecString;
};
