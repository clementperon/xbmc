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
#include "test/TestUtils.h"

#include <string>

#include <gtest/gtest.h>

/*!
 * The single file path: a Matroska holding one song rather than an album, which never reaches
 * CAudioBookFileDirectory. The fixture comes from tools/testdata/mkmka.py, and what is asserted
 * holds for either reader.
 */
using namespace MUSIC_INFO;

TEST(TestMusicInfoTagLoaderMatroska, ReadsAFileWithNoChapters)
{
  const std::string path = XBMC_REF_FILE_PATH("xbmc/filesystem/test/data/audiobook/singlefile.mka");

  CMusicInfoTag tag;
  CMusicInfoTagLoaderMatroska loader;
  ASSERT_TRUE(loader.Load(path, tag, nullptr));

  EXPECT_TRUE(tag.Loaded());
  EXPECT_EQ("Live At The Test Venue", tag.GetAlbum());
  // The Segment title names the file, so it is the song; the album level TITLE is the album.
  EXPECT_EQ("So What", tag.GetTitle());
  // The file tags it at TargetTypeValue 50, which is the album's artist rather than the track's.
  EXPECT_EQ("The Test Band", tag.GetAlbumArtistString());
  /*!
   * The album's artist is the song's until the song says otherwise. A file that names an artist
   * only at album level - which is most of them - would otherwise scan in with none at all.
   */
  EXPECT_EQ("The Test Band", tag.GetArtistString());
  EXPECT_EQ("2026", tag.GetReleaseDate());

  bool composed = false;
  for (const auto& c : tag.GetContributors())
    composed = composed || (c.GetRoleDesc() == "Composer" && c.GetArtist() == "Bill Evans");
  EXPECT_TRUE(composed);
}

/*!
 * With no Segment title nothing names the song, and a file that says only what album it belongs
 * to would reach the library untitled. The album title stands in.
 */
TEST(TestMusicInfoTagLoaderMatroska, TitlesASongAfterItsAlbumWhenTheFileNamesNoSong)
{
  const std::string path =
      XBMC_REF_FILE_PATH("xbmc/filesystem/test/data/audiobook/singlefile-notitle.mka");

  CMusicInfoTag tag;
  CMusicInfoTagLoaderMatroska loader;
  ASSERT_TRUE(loader.Load(path, tag, nullptr));

  EXPECT_TRUE(tag.Loaded());
  EXPECT_EQ("Live At The Test Venue", tag.GetAlbum());
  EXPECT_EQ("Live At The Test Venue", tag.GetTitle());
}

/*!
 * A file whose only real chapter follows one too short to be a track is not an album, so this
 * loader reads it whole. The song it describes is the track, not the artefact before it.
 */
TEST(TestMusicInfoTagLoaderMatroska, SkipsAChapterTooShortToBeTheSong)
{
  const std::string path =
      XBMC_REF_FILE_PATH("xbmc/filesystem/test/data/audiobook/onetrackplusartefact.mka");

  CMusicInfoTag tag;
  CMusicInfoTagLoaderMatroska loader;
  ASSERT_TRUE(loader.Load(path, tag, nullptr));

  EXPECT_TRUE(tag.Loaded());
  EXPECT_EQ("Someone's Song", tag.GetTitle());
}
