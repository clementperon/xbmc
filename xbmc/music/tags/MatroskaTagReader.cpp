/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MatroskaTagReader.h"

#include "TagLibVersion.h"
#include "URL.h"
#include "utils/StringUtils.h"
#ifdef HAS_TAGLIB_MATROSKA
#include "MusicInfoTagLoaderMatroska.h"
#endif

#include <map>
#include <string>
#include <tuple>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>
}

using namespace MUSIC_INFO;

namespace
{
//! Harvest one FFmpeg metadata dictionary into the map CMatroskaTagParser expects.
void CollectTags(const AVDictionary* metadata, std::map<std::string, std::string>& tags)
{
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)))
    tags[StringUtils::ToUpper(entry->key)] = entry->value;
}

/*!
 * Read with FFmpeg's demuxer, which flattens the SimpleTag hierarchy: TargetTypeValue is lost -
 * album level tags arrive under an ALBUM_ prefix instead - nesting goes with it, only the last of
 * a repeated set of SimpleTags survives (https://trac.ffmpeg.org/ticket/9641), and only the
 * default edition's chapters are reported. A subset of what TagLib returns, not a different thing.
 *
 * Compiled whether or not it is the reader this build uses, so that it cannot rot unnoticed in
 * the builds that have TagLib.
 */
[[maybe_unused]] MatroskaAlbum ReadWithFFmpeg(const CURL& /*url*/, const AVFormatContext* fctx)
{
  MatroskaAlbum album;

  if (!fctx)
    return album;

  CollectTags(fctx->metadata, album.fileTags);

  for (unsigned int i = 0; fctx->chapters && i < fctx->nb_chapters; ++i)
  {
    const AVChapter* chapter = fctx->chapters[i];
    if (!chapter || chapter->start < 0)
      continue;

    ChapterTags& entry = album.chapters.emplace_back();
    CollectTags(chapter->metadata, entry.tags);
    entry.start = chapter->start * av_q2d(chapter->time_base);
    entry.end = chapter->end * av_q2d(chapter->time_base);
  }

  album.valid = true;
  return album;
}

#ifdef HAS_TAGLIB_MATROSKA

/*!
 * Read with TagLib, which follows the whole SimpleTag hierarchy: TargetTypeValue, EditionUID and
 * ChapterUID all survive, so album level tags stay apart from a chapter's own, repeated tags all
 * arrive, and both stay tied to the edition they belong to.
 */
MatroskaAlbum ReadWithTagLib(const CURL& url, const AVFormatContext* /*fctx*/)
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

#endif // TagLib >= 2.3.1
} // unnamed namespace

MatroskaAlbum MUSIC_INFO::ReadMatroskaTags(const CURL& url, const AVFormatContext* fctx)
{
  // TagLib wherever the version floor allows it, FFmpeg below that: fewer tags rather than none.
#ifdef HAS_TAGLIB_MATROSKA
  return ReadWithTagLib(url, fctx);
#else
  return ReadWithFFmpeg(url, fctx);
#endif
}
