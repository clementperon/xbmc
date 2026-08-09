/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "File.h"
#include "IFileDirectory.h"
#include "music/tags/MatroskaTagReader.h"

#include <memory>
#include <optional>
#include <string>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace XFILE
{
class CAudioBookFileDirectory : public IFileDirectory
{
public:
  ~CAudioBookFileDirectory(void) override;
  bool GetDirectory(const CURL& url, CFileItemList& items) override;
  bool Exists(const CURL& url) override;
  bool ContainsFiles(const CURL& url) override;
  bool IsAllowed(const CURL& url) const override { return true; }

protected:
  //! The file m_ioctx reads through: its callbacks hold a pointer to it, so it has to outlive
  //! m_fctx, which the destructor closes before any member goes.
  std::unique_ptr<CFile> m_file;
  AVIOContext* m_ioctx = nullptr;
  AVFormatContext* m_fctx = nullptr;
  /*!
   * What ContainsFiles() read to count the tracks, for GetDirectory() to build them from without
   * parsing the file again - and the URL it came from, which is half of it: every method takes a
   * URL and nothing promises two calls bring the same one. Neither half means anything without
   * the other, so they are one value that is set once or not at all.
   */
  struct CachedRead
  {
    std::string url;
    MUSIC_INFO::MatroskaAlbum album;
  };
  std::optional<CachedRead> m_read;
};
} // namespace XFILE
