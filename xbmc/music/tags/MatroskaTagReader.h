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

/*!
 * \brief Matroska tags of a whole file: album level, plus one entry per chapter in file order.
 *
 * Every chapter the file declares is here, with a play range both readers fill. Whether a chapter
 * is short enough not to be a track is the library's call and is made by the caller, so that the
 * two readers describe the same file the same way.
 */
struct MatroskaAlbum
{
  std::map<std::string, std::string> fileTags;
  std::vector<ChapterTags> chapters;

  //! Whether the file carries album level tags, which is what makes it worth expanding.
  bool hasAlbumTags() const { return !fileTags.empty(); }
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
 * \return What the file holds. Both members are empty for a file the reader could not read, so
 *         what counts as an album is the caller's to decide.
 */
MatroskaAlbum ReadMatroskaTags(const CURL& url, const AVFormatContext* fctx);

/*!
 * \brief Read with FFmpeg's demuxer, whether or not it is the reader this build uses.
 *
 * ReadMatroskaTags() is what production calls. This names the FFmpeg reader directly so that the
 * fallback can be tested from a build that has TagLib, which is every build the CI makes - it is
 * otherwise compiled and never run.
 *
 * \param fctx A demuxer context already opened on the file. Required: this reader has no other
 *             way to reach it.
 */
MatroskaAlbum ReadMatroskaTagsWithFFmpeg(const CURL& url, const AVFormatContext* fctx);
} // namespace MUSIC_INFO
