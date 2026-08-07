/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "music/Artist.h"
#include "music/tags/MusicInfoTag.h"
#include "music/tags/MusicInfoTagLoaderMatroska.h"
#include "music/tags/TagLibVersion.h"
#include "test/TestUtils.h"

#include <string>

#include <gtest/gtest.h>

/*!
 * The single file path: a Matroska holding one song rather than an album, which never reaches
 * CAudioBookFileDirectory. The fixture comes from tools/testdata/mkmka.py.
 */
#ifdef HAS_TAGLIB_MATROSKA

using namespace MUSIC_INFO;

TEST(TestMusicInfoTagLoaderMatroska, ReadsAFileWithNoChapters)
{
  const std::string path = XBMC_REF_FILE_PATH("xbmc/filesystem/test/data/audiobook/singlefile.mka");

  CMusicInfoTag tag;
  CMusicInfoTagLoaderMatroska loader;
  ASSERT_TRUE(loader.Load(path, tag, nullptr));

  EXPECT_TRUE(tag.Loaded());
  EXPECT_EQ("Live At The Test Venue", tag.GetAlbum());
  EXPECT_EQ("Live At The Test Venue", tag.GetTitle());
  EXPECT_EQ("The Test Band", tag.GetArtistString());
  EXPECT_EQ("2026", tag.GetReleaseDate());

  bool composed = false;
  for (const auto& c : tag.GetContributors())
    composed = composed || (c.GetRoleDesc() == "Composer" && c.GetArtist() == "Bill Evans");
  EXPECT_TRUE(composed);
}

#endif // TagLib >= 2.3.1
