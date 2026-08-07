/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ImusicInfoTagLoader.h"
#include "MusicInfoTag.h"
#include "TagLibVersion.h"
#include "utils/EmbeddedArt.h"

#include <string>

#ifdef HAS_TAGLIB_MATROSKA
#include "MatroskaTagLibStream.h"

#include <map>
#include <tuple>
#include <vector>
#endif

namespace MUSIC_INFO
{
class CMusicInfoTagLoaderMatroska : public IMusicInfoTagLoader
{
public:
  CMusicInfoTagLoaderMatroska() = default;
  ~CMusicInfoTagLoaderMatroska() override = default;

  bool Load(const std::string& strFileName,
            CMusicInfoTag& tag,
            EmbeddedArt* art = nullptr) override;

#ifdef HAS_TAGLIB_MATROSKA
  // Static overload for external callers (e.g. AudioBookFileDirectory) —
  // opens its own MatroskaTagLibStream internally.
  // If coverTag is non-null, embedded cover art info is set on it.
  static void GetMatroskaMusicTags(
      const std::string& fileName,
      std::map<std::string, std::string>& fileTags,
      std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
      std::vector<std::tuple<unsigned long long, std::string, double, double, unsigned long long>>&
          chapterOrder,
      CMusicInfoTag* coverTag = nullptr);

private:
  // Internal overload used by Load() — reuses an already-open stream
  static void GetMatroskaMusicTags(
      const std::string& fileName,
      MatroskaTagLibStream& matroskaStream,
      std::map<std::string, std::string>& fileTags,
      std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
      std::vector<std::tuple<unsigned long long, std::string, double, double, unsigned long long>>&
          chapterOrder,
      CMusicInfoTag* coverTag = nullptr,
      EmbeddedArt* art = nullptr);
#endif // TagLib >= 2.3.1
};
} // namespace MUSIC_INFO
