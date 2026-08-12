/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "FFmpegVfsContext.h"
#include "IFileDirectory.h"
#include "music/tags/MatroskaTagReader.h"

#include <optional>
#include <string>

namespace XFILE
{
class CAudioBookFileDirectory : public IFileDirectory
{
public:
  bool GetDirectory(const CURL& url, CFileItemList& items) override;
  bool Exists(const CURL& url) override;
  bool ContainsFiles(const CURL& url) override;
  bool IsAllowed(const CURL& url) const override { return true; }

protected:
  //! The file ContainsFiles() opened, kept so that GetDirectory() need not open it a second time.
  CFFmpegVfsContext m_demux;
  /*!
   * What ContainsFiles() read to count the tracks, for GetDirectory() to build them from without
   * parsing the file again - and the URL it came from, which is half of it: every method takes a
   * URL and nothing promises two calls bring the same one. Neither half means anything without
   * the other, so they are one value that is set once or not at all.
   *
   * The URL is what m_demux was opened on as well: ContainsFiles() sets both or neither, so a
   * call naming another file reopens the one and re-reads the other together.
   */
  struct CachedRead
  {
    std::string url;
    MUSIC_INFO::MatroskaAlbum album;
  };
  std::optional<CachedRead> m_read;
};
} // namespace XFILE
