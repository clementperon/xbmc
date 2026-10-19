/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerWasm.h"

#include "MediaBackendWasm.h"
#include "MediaPipelineWasm.h"
#include "VideoPlayerAudioWasm.h"
#include "VideoPlayerVideoWasm.h"

#include <algorithm>

CVideoPlayerWasm::CVideoPlayerWasm(IPlayerCallback& callback) : CVideoPlayer(callback)
{
}

CVideoPlayerWasm::~CVideoPlayerWasm() = default;

bool CVideoPlayerWasm::CanUseMediaPipeline(IMediaBackendWasm& backend)
{
  const auto supportsVideo = [&backend](const SelectionStream& stream)
  {
    CDVDStreamInfo hint;
    hint.codec = stream.codecId;
    hint.profile = stream.profile;
    hint.width = stream.width;
    hint.height = stream.height;
    hint.fpsrate = static_cast<int>(stream.fpsRate);
    hint.fpsscale = static_cast<int>(stream.fpsScale);
    hint.dovi = stream.dovi;
    return backend.SupportsVideo(hint);
  };
  const auto supportsAudio = [&backend](const SelectionStream& stream)
  {
    CDVDStreamInfo hint;
    hint.codec = stream.codecId;
    hint.profile = stream.profile;
    hint.channels = stream.channels;
    return backend.GetAudioPath(hint) != IMediaBackendWasm::AudioPath::NONE;
  };

  // VideoPlayer opens the first audio stream the pipeline takes.
  const auto audio = m_SelectionStreams.Get(StreamType::AUDIO);
  return std::ranges::any_of(m_SelectionStreams.Get(StreamType::VIDEO), supportsVideo) &&
         (audio.empty() || std::ranges::any_of(audio, supportsAudio));
}

void CVideoPlayerWasm::CreatePlayers()
{
  // The backend depends on the file and on a setting, so it can change between files.
  std::unique_ptr<IMediaBackendWasm> backend;
  for (auto& candidate : IMediaBackendWasm::Create())
  {
    if (CanUseMediaPipeline(*candidate))
    {
      backend = std::move(candidate);
      break;
    }
  }
  if (backend)
  {
    const bool hasAudio = m_SelectionStreams.CountType(StreamType::AUDIO);
    // The pipeline outlives the file when a playlist moves on with the same backend.
    if (m_mediaPipeline && m_mediaPipeline->Backend().Name() == backend->Name())
      m_mediaPipeline->SetHasAudio(hasAudio);
    else
    {
      const bool subtitlesEnabled = m_VideoPlayerVideo->IsSubtitleEnabled();
      const double subtitleDelay = m_VideoPlayerVideo->GetSubtitleDelay();

      m_mediaPipeline = std::make_unique<CMediaPipelineWasm>(
          std::move(backend), *m_processInfo, m_renderManager, m_clock, m_messenger,
          m_overlayContainer, hasAudio,
          [this] { return m_pDemuxer ? m_pDemuxer->GetStreamLength() : 0; });
      m_VideoPlayerVideo = std::make_unique<CVideoPlayerVideoWasm>(*m_mediaPipeline, *m_processInfo);
      m_VideoPlayerAudio = std::make_unique<CVideoPlayerAudioWasm>(*m_mediaPipeline, *m_processInfo);
      m_VideoPlayerVideo->EnableSubtitle(subtitlesEnabled);
      m_VideoPlayerVideo->SetSubtitleDelay(subtitleDelay);
    }
  }
  else if (m_mediaPipeline || (!m_VideoPlayerVideo && !m_VideoPlayerAudio))
  {
    m_mediaPipeline = nullptr;
    m_VideoPlayerVideo = std::make_unique<CVideoPlayerVideo>(
        &m_clock, &m_overlayContainer, m_messenger, m_renderManager, *m_processInfo,
        m_messageQueueTimeSize);
    m_VideoPlayerAudio = std::make_unique<CVideoPlayerAudio>(&m_clock, m_messenger, *m_processInfo,
                                                             m_messageQueueTimeSize);
  }

  if (m_players_created)
    return;

  m_VideoPlayerSubtitle =
      std::make_unique<CVideoPlayerSubtitle>(&m_overlayContainer, *m_processInfo);
  m_VideoPlayerTeletext = std::make_unique<CDVDTeletextData>(*m_processInfo);
  m_VideoPlayerRadioRDS = std::make_unique<CDVDRadioRDSData>(*m_processInfo);
  m_VideoPlayerAudioID3 = std::make_unique<CVideoPlayerAudioID3>(*m_processInfo);
  m_players_created = true;
}

void CVideoPlayerWasm::GetVideoResolution(unsigned int& width, unsigned int& height)
{
  if (m_mediaPipeline)
    m_mediaPipeline->GetVideoResolution(width, height);
  else
    CVideoPlayer::GetVideoResolution(width, height);
}

void CVideoPlayerWasm::UpdateContent()
{
  CVideoPlayer::UpdateContent();
  CreatePlayers();
}

bool CVideoPlayerWasm::CanTempo()
{
  return m_mediaPipeline || CVideoPlayer::CanTempo();
}
