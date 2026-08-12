/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioBookFileDirectory.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "dbwrappers/Database.h"
#include "filesystem/File.h"
#include "imagefiles/ImageFileURL.h"
#include "music/MusicEmbeddedCoverLoaderFFmpeg.h"
#include "music/tags/MatroskaTagMapping.h"
#include "music/tags/MatroskaTagReader.h"
#include "music/tags/MusicCodecInfoFFmpeg.h"
#include "music/tags/MusicInfoTag.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <commons/ilog.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>

using namespace XFILE;
using namespace MUSIC_INFO;

bool CAudioBookFileDirectory::GetDirectory(const CURL& url, CFileItemList& items)
{
  /*!
   * Nothing promises this is the file ContainsFiles() was asked about: the same instance can be
   * handed another URL, and both what was read and the context it was read through describe the
   * one before it. Reopening is what ContainsFiles() does, so ask it again.
   */
  if ((!m_read || m_read->url != url.Get()) && !ContainsFiles(url))
    return true;

  AVFormatContext* const fctx = m_demux.FormatContext();

  std::string title;
  std::string author;
  std::string album;
  std::string desc;

  std::vector<std::string> separators{" feat. ", " ft. ", " Feat. ", " Ft. ",  ";", ":",
                                      "|",       "#",     "/",       " with ", "&"};
  const std::string musicsep =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;
  if (musicsep.find_first_of(";/,&|#") == std::string::npos)
    separators.push_back(musicsep); // add custom music separator from as.xml

  const bool isAudioBook = url.IsFileType("m4b");

  // Some tags are relevant to the whole album - these are read first
  CMusicInfoTag albumtag;

  if (isAudioBook)
  {
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(fctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
      if (StringUtils::CompareNoCase(tag->key, "title") == 0)
        title = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "album") == 0)
        album = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "artist") == 0)
        author = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "description") == 0)
        desc = tag->value;
    }
  }
  else
  {
    if (!m_read->album.hasAlbumTags())
      return true;
    /*!
     * initially just get the (file) Album level tags to be use in subsequent tracks
     * (chapters) processed below to create Kodi music Songs
    */
    /*!
     * Album level first, then what the file says about itself, so that a Segment title names the
     * song where the album's title only stood in. Each track's own tags come later still.
     */
    for (const auto& t : m_read->album.albumTags)
      MatroskaTagMapping::MapTag(t.first, t.second, MatroskaTagMapping::TagLevel::Album, separators,
                                 musicsep, albumtag);
    for (const auto& t : m_read->album.fileTags)
      MatroskaTagMapping::MapTag(t.first, t.second, MatroskaTagMapping::TagLevel::File, separators,
                                 musicsep, albumtag);
  }

  std::string thumb;
  thumb = IMAGE_FILES::URLFromFile(url.Get(), "music");
  /*! Look for any embedded cover art
  * FFmpeg rather than TagLib: TagLib reads whole Matroska attachments eagerly, which is slow for
  * large attachments over SMB/NFS. Still unfixed as of TagLib 2.3.1 (it was expected there).
  */
  CMusicEmbeddedCoverLoaderFFmpeg::GetEmbeddedCover(fctx, albumtag);

  // now get the AudioCodec -------------------------------------
  bool haveFFmpegInfo = false;
  musicCodecInfo codec_info;
  haveFFmpegInfo = CMusicCodecInfoFFmpeg::GetMusicCodecInfo(fctx, codec_info);
  if (haveFFmpegInfo) // use data from FFmpeg (taglib 2.3 does not support some codecs)
  {
    albumtag.SetBitRate(codec_info.bitRate);
    albumtag.SetSampleRate(codec_info.sampleRate);
    /*!
    * Additional Music properties (next PR - Add Album Codec Support to Music)
    * albumtag.SetBitsPerSample(codec_info.bitsPerSample);
    * albumtag.SetCodec(codec_info.codecName); // e.g. 'truehd_atmos', 'dts_ma', 'dts_hd', etc
    */
    albumtag.SetNoOfChannels(codec_info.channels);
    albumtag.SetDuration(codec_info.duration);
  }

  /*!
   * The chapters come from whichever reader read the file: ReadMatroskaTags for Matroska,
   * FFmpeg for an audiobook, which has no other reader. Taking the play ranges from one list and
   * the tags from the other pairs a chapter's tags with a different chapter's range as soon as the
   * two disagree on how many chapters there are - which they do over a file holding several
   * editions, or once either of them has dropped a chapter too short to be a track.
   */
  const size_t chapterCount =
      isAudioBook ? (fctx->chapters ? fctx->nb_chapters : 0) : m_read->album.chapters.size();
  int trackNumber = 0;
  bool chapter_error = false;
  for (size_t i = 0; i < chapterCount; ++i)
  {
    double start = 0.0;
    double end = 0.0;
    if (isAudioBook)
    {
      const AVChapter* chapter = fctx->chapters[i];
      if (!chapter || chapter->start < 0) // null or negative start time
        continue;
      start = chapter->start * av_q2d(chapter->time_base);
      end = chapter->end * av_q2d(chapter->time_base);
    }
    else
    {
      start = m_read->album.chapters[i].start;
      end = m_read->album.chapters[i].end;
    }

    if (!IsTrack(start, end))
    {
      CLog::Log(LOGWARNING,
                "CAudioBookFileDirectory: Tiny chapter of size {}s detected when scanning {} Most "
                "likely this file needs the chapters correcting",
                end - start, url.GetRedacted());
      chapter_error = true;
      continue;
    }

    // Numbers the tracks that are kept, so a dropped chapter leaves no gap in the album.
    ++trackNumber;

    std::shared_ptr<CFileItem> item(new CFileItem(url.Get(), false));
    *item->GetMusicInfoTag() = albumtag;

    if (isAudioBook)
    {
      AVDictionaryEntry* tag = nullptr;
      std::string chaptitle = StringUtils::Format(
          CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(25010), trackNumber);
      std::string chapauthor;
      std::string chapalbum;

      while ((tag = av_dict_get(fctx->chapters[i]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
      {
        if (StringUtils::CompareNoCase(tag->key, "title") == 0)
          chaptitle = tag->value;
        else if (StringUtils::CompareNoCase(tag->key, "artist") == 0)
          chapauthor = tag->value;
        else if (StringUtils::CompareNoCase(tag->key, "album") == 0)
          chapalbum = tag->value;
      }
      item->GetMusicInfoTag()->SetTitle(chaptitle);
      item->GetMusicInfoTag()->SetAlbum(chapalbum.empty() ? album.empty() ? title : album
                                                          : chapalbum);
      item->GetMusicInfoTag()->SetArtist(chapauthor.empty() ? author : chapauthor);
      if (!desc.empty())
        item->GetMusicInfoTag()->SetComment(desc);
    }
    else
    {
      /*!
       * Drop the album level track number before reading the chapter's own tags, so that what
       * remains afterwards came from this chapter. Leaving it would read as the chapter having
       * numbered itself and suppress the positional fallback below on every track.
       */
      item->GetMusicInfoTag()->SetTrackNumber(0);

      // process chapter tags for this track, in file order
      for (const auto& t : m_read->album.chapters[i].tags)
        MatroskaTagMapping::MapTag(t.first, t.second, MatroskaTagMapping::TagLevel::Track,
                                   separators, musicsep, *item->GetMusicInfoTag());
    }

    item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(start));
    item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(end));
    /*!
     * A chapter ContainsFiles() could not close has no end to play to and no length to state: the
     * file said neither and nothing could measure it. An unset end offset is how the player is
     * told to play to the end of the file, and the duration the album tag already carries is the
     * closest thing to this track's there is.
     */
    if (item->GetEndOffset() > item->GetStartOffset())
      item->GetMusicInfoTag()->SetDuration(
          CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));

    // Position in the album, for a track whose own tags did not number it.
    if (item->GetMusicInfoTag()->GetTrackNumber() <= 0)
      item->GetMusicInfoTag()->SetTrackNumber(trackNumber);
    item->GetMusicInfoTag()->SetLoaded(true);

    // The number the track ended up with, which is the chapter's own where it gave one.
    item->SetLabel(StringUtils::Format(
        "{0:02}. {1} - {2}", item->GetMusicInfoTag()->GetTrackNumber(),
        item->GetMusicInfoTag()->GetAlbum(), item->GetMusicInfoTag()->GetTitle()));

    item->SetProperty("item_start", item->GetStartOffset());
    item->SetProperty("audio_bookmark", item->GetStartOffset());
    if (!thumb.empty() && !chapter_error)
      item->SetArt("thumb", thumb);
    items.Add(item);
  }
  return true;
}

bool CAudioBookFileDirectory::Exists(const CURL& url)
{
  return CFile::Exists(url);
}

bool CAudioBookFileDirectory::ContainsFiles(const CURL& url)
{
  /*!
   * Whatever was open described some other file until this succeeds, so drop it first rather than
   * leave a caller that reuses this instance holding the last file's context beside this file's
   * tags.
   */
  m_read.reset();
  if (!m_demux.Open(url.Get()))
    return false;

  // From here on the context is this URL's, which is what m_read records for GetDirectory().
  m_read = CachedRead{url.Get(), {}};

  // m4b has no reader but FFmpeg, so its chapters are the only count there is.
  if (url.IsFileType("m4b"))
    return m_demux.FormatContext()->nb_chapters > 1;

  /*!
   * Ask the reader that will build the tracks how many there are, rather than FFmpeg on its
   * behalf: the two need not agree on a file whose chapters they read differently, and a file
   * turned away here is never offered to the reader that would have found an album in it. Holding
   * the result is what keeps that from costing a second parse in GetDirectory().
   */
  m_read->album = ReadMatroskaTags(url, m_demux.FormatContext());

  /*!
   * Before counting, not after: a chapter the file left open is as long as what remains of the
   * file, and until it is closed there is no telling a last song from a trailing artefact.
   */
  CloseOpenEndedChapters(m_read->album, m_demux.Duration());

  const auto tracks = std::count_if(m_read->album.chapters.begin(), m_read->album.chapters.end(),
                                    [](const ChapterTags& c) { return IsTrack(c.start, c.end); });
  return m_read->album.hasAlbumTags() && tracks > 1;
}
