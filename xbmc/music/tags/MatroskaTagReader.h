/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

class CURL;
struct AVFormatContext;

namespace MUSIC_INFO
{
//! Tags of one chapter and its play range, both from the reader that read the file.
struct ChapterTags
{
  std::map<std::string, std::string> tags;
  double start = 0.0; //!< seconds
  double end = 0.0;
};

//! Matroska tags of a whole file: file (album) level plus one entry per chapter, in file order.
struct MatroskaAlbum
{
  std::map<std::string, std::string> fileTags;
  std::vector<ChapterTags> chapters;
  bool valid = false;
};

/*!
 * \brief Read a Matroska file's album level tags and its chapters.
 *
 * A build has one reader, TagLib's Matroska API or FFmpeg's demuxer, picked in the .cpp from
 * HAS_TAGLIB_MATROSKA - see TagLibVersion.h. Whichever it is reads a file whole: callers must not
 * take some of an album from here and the rest from elsewhere, because the two readers need not
 * agree on how many chapters a file has nor on their order. The tag names both produce mean the
 * same thing; CMatroskaTagParser is where that is settled.
 *
 * \param url The file, for a reader that opens it itself.
 * \param fctx A demuxer context already opened on it, for a reader that does not.
 * \return The album, its valid flag false if the file holds nothing worth an album.
 */
MatroskaAlbum ReadMatroskaTags(const CURL& url, const AVFormatContext* fctx);
} // namespace MUSIC_INFO
