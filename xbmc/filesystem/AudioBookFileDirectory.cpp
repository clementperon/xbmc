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
#include "IFileTypes.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "dbwrappers/Database.h"
#include "filesystem/File.h"
#include "imagefiles/ImageFileURL.h"
#include "music/MusicEmbeddedCoverLoaderFFmpeg.h"
#include "music/tags/MatroskaTagParser.h"
#include "music/tags/MusicCodecInfoFFmpeg.h"
#include "music/tags/MusicInfoTag.h"
#include "music/tags/TagLibVersion.h"
#ifdef HAS_TAGLIB_MATROSKA
#include "music/tags/MusicInfoTagLoaderMatroska.h"
#endif
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <commons/ilog.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>

using namespace XFILE;
using namespace MUSIC_INFO;

static int cfile_file_read(void* h, uint8_t* buf, int size)
{
  CFile* pFile = static_cast<CFile*>(h);
  return pFile->Read(buf, size);
}

static int64_t cfile_file_seek(void* h, int64_t pos, int whence)
{
  CFile* pFile = static_cast<CFile*>(h);
  if (whence == AVSEEK_SIZE)
    return pFile->GetLength();
  else
    return pFile->Seek(pos, whence & ~AVSEEK_FORCE);
}

namespace
{
//! Tags of one chapter, plus its play range when the tag reader supplies one.
struct ChapterTags
{
  std::map<std::string, std::string> tags;
  double start = -1.0; //!< seconds; negative means "use the FFmpeg chapter timestamps"
  double end = -1.0;
};

//! Matroska tags of a whole file: file (album) level plus one entry per chapter, in file order.
struct MatroskaAlbum
{
  std::map<std::string, std::string> fileTags;
  std::vector<ChapterTags> chapters;
  bool valid = false;
};

#ifdef HAS_TAGLIB_MATROSKA

//! Read Matroska tags with TagLib, which follows the full tag hierarchy.
MatroskaAlbum ReadMatroskaTags(const CURL& url, const AVFormatContext* /*fctx*/)
{
  MatroskaAlbum album;

  std::map<unsigned long long, std::map<std::string, std::string>> chapterTags;
  std::vector<std::tuple<unsigned long long, std::string, double, double, unsigned long long>>
      chapterOrder;
  CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(url.Get(), album.fileTags, chapterTags,
                                                    chapterOrder);
  if (album.fileTags.empty())
    return album;

  album.chapters.resize(chapterOrder.size());
  for (size_t i = 0; i < chapterOrder.size(); ++i)
  {
    const auto it = chapterTags.find(std::get<0>(chapterOrder[i]));
    if (it != chapterTags.end())
      album.chapters[i].tags = it->second;
    album.chapters[i].start = std::get<2>(chapterOrder[i]);
    album.chapters[i].end = std::get<3>(chapterOrder[i]);
  }

  album.valid = true;
  return album;
}

#else

//! Harvest one FFmpeg metadata dictionary into the map CMatroskaTagParser expects.
void CollectTags(const AVDictionary* metadata, std::map<std::string, std::string>& tags)
{
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)))
    tags[StringUtils::ToUpper(entry->key)] = entry->value;
}

/*!
 * Read Matroska tags with FFmpeg.
 *
 * FFmpeg flattens the SimpleTag hierarchy into a file-level and a per-chapter dictionary, so
 * nesting and TargetTypeValue are lost, and only the last of a repeated set of SimpleTags
 * survives (https://trac.ffmpeg.org/ticket/9641) - which is why taggers write the comma
 * delimited INVOLVEDPEOPLE/INSTRUMENTS forms CMatroskaTagParser understands. Tag names and their
 * meanings match the TagLib variant above: both feed CMatroskaTagParser.
 */
MatroskaAlbum ReadMatroskaTags(const CURL& /*url*/, const AVFormatContext* fctx)
{
  MatroskaAlbum album;

  CollectTags(fctx->metadata, album.fileTags);

  album.chapters.resize(fctx->nb_chapters);
  for (unsigned int i = 0; fctx->chapters && i < fctx->nb_chapters; ++i)
  {
    if (fctx->chapters[i])
      CollectTags(fctx->chapters[i]->metadata, album.chapters[i].tags);
  }

  // Chapter play ranges stay unset; the caller takes them from the FFmpeg chapter timestamps.
  album.valid = true;
  return album;
}

#endif // HAS_TAGLIB_MATROSKA
} // unnamed namespace

CAudioBookFileDirectory::~CAudioBookFileDirectory(void)
{
  if (m_fctx)
    avformat_close_input(&m_fctx);
  if (m_ioctx)
  {
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
  }
}

bool CAudioBookFileDirectory::GetDirectory(const CURL& url, CFileItemList& items)
{
  if (!m_fctx && !ContainsFiles(url))
    return true;

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
  MatroskaAlbum matroska;

  if (isAudioBook)
  {
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(m_fctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
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
    matroska = ReadMatroskaTags(url, m_fctx);
    if (!matroska.valid)
      return true;
    /*!
     * initially just get the (file) Album level tags to be use in subsequent tracks
     * (chapters) processed below to create Kodi music Songs
    */
    for (const auto& t : matroska.fileTags)
      CMatroskaTagParser::ParseTag(t.first, t.second, separators, musicsep, albumtag);
  }

  std::string thumb;
  thumb = IMAGE_FILES::URLFromFile(url.Get(), "music");
  /*! Look for any embedded cover art
  * FFmpeg rather than TagLib: TagLib reads whole Matroska attachments eagerly, which is slow for
  * large attachments over SMB/NFS. Still unfixed as of TagLib 2.3.1 (it was expected there).
  */
  CMusicEmbeddedCoverLoaderFFmpeg::GetEmbeddedCover(m_fctx, albumtag);

  // now get the AudioCodec -------------------------------------
  bool haveFFmpegInfo = false;
  musicCodecInfo codec_info;
  haveFFmpegInfo = CMusicCodecInfoFFmpeg::GetMusicCodecInfo(url.Get(), codec_info);
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

  float chapter_size = 0;
  bool chapter_error = false;
  for (unsigned int i = 0; m_fctx->chapters && i < m_fctx->nb_chapters; ++i)
  {
    if (!m_fctx->chapters[i] || m_fctx->chapters[i]->start < 0) // null or negative start time
      continue;
    chapter_size = (m_fctx->chapters[i]->end - m_fctx->chapters[i]->start) *
                   av_q2d(m_fctx->chapters[i]->time_base);
    if (chapter_size < 1) // Chapter must have positive time of more than 1 sec
    {
      CLog::Log(LOGWARNING,
                "CAudioBookFileDirectory: Tiny chapter of size {}s detected when scanning {} Most "
                "likely this file needs the chapters correcting",
                chapter_size, url.GetRedacted());
      chapter_error = true;
      continue;
    }

    std::shared_ptr<CFileItem> item(new CFileItem(url.Get(), false));
    *item->GetMusicInfoTag() = albumtag;

    // Chapter play range as FFmpeg reports it. The Matroska tag reader may override it below.
    double start = m_fctx->chapters[i]->start * av_q2d(m_fctx->chapters[i]->time_base);
    double end = m_fctx->chapters[i]->end * av_q2d(m_fctx->chapters[i]->time_base);

    if (isAudioBook)
    {
      AVDictionaryEntry* tag = nullptr;
      std::string chaptitle = StringUtils::Format(
          CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(25010), i + 1);
      std::string chapauthor;
      std::string chapalbum;

      while ((tag = av_dict_get(m_fctx->chapters[i]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
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
    else if (i < matroska.chapters.size())
    {
      // process chapter tags for this track, in file order
      const ChapterTags& chapter = matroska.chapters[i];
      for (const auto& t : chapter.tags)
        CMatroskaTagParser::ParseTag(t.first, t.second, separators, musicsep,
                                     *item->GetMusicInfoTag());

      if (chapter.start >= 0.0)
      {
        start = chapter.start;
        end = chapter.end;
      }
    }

    item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(start));
    item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(end));
    item->GetMusicInfoTag()->SetDuration(
        CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));

    item->GetMusicInfoTag()->SetTrackNumber(i + 1);
    item->GetMusicInfoTag()->SetLoaded(true);

    item->SetLabel(StringUtils::Format("{0:02}. {1} - {2}", i + 1,
                                       item->GetMusicInfoTag()->GetAlbum(),
                                       item->GetMusicInfoTag()->GetTitle()));

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
  CFile file;
  if (!file.Open(url))
    return false;

  uint8_t* buffer = static_cast<uint8_t*>(av_malloc(32768));
  if (!buffer)
    return false;

  m_ioctx = avio_alloc_context(buffer, 32768, 0, &file, cfile_file_read, nullptr, cfile_file_seek);
  if (!m_ioctx)
  {
    av_free(buffer);
    return false;
  }

  m_fctx = avformat_alloc_context();
  if (!m_fctx)
  {
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
    m_ioctx = nullptr;
    return false;
  }
  m_fctx->pb = m_ioctx;
  m_fctx->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (file.IoControl(IOControl::SEEK_POSSIBLE, nullptr) == 0)
    m_ioctx->seekable = 0;

  m_ioctx->max_packet_size = 32768;

  const AVInputFormat* iformat = nullptr;
  av_probe_input_buffer(m_ioctx, &iformat, url.Get().c_str(), nullptr, 0, 0);

  bool contains = false;

  if (avformat_open_input(&m_fctx, url.Get().c_str(), iformat, nullptr) < 0)
  {
    if (m_fctx)
      avformat_close_input(&m_fctx);
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
    m_ioctx = nullptr;
    return false;
  }
  m_fctx->flags |= AVFMT_FLAG_NOPARSE;
  int err = avformat_find_stream_info(m_fctx, NULL);
  if (err < 0)
    CLog::Log(LOGERROR, "Can't detect codec info in file {}", url.GetRedacted());

  contains = m_fctx->nb_chapters > 1;

  return contains;
}
