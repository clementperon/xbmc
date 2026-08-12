/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FileItem.h"
#include "FileItemList.h"
#include "URL.h"
#include "filesystem/AudioBookFileDirectory.h"
#include "music/Artist.h"
#include "music/tags/MusicInfoTag.h"
#include "music/tags/TagLibVersion.h"
#include "test/TestUtils.h"

#include <string>

#include <gtest/gtest.h>

/*!
 * The fixtures come from tools/testdata/mkmka.py, which is also where what each one holds is
 * written down. These hold for either reader, bar the one marked below.
 */
using namespace XFILE;

namespace
{
constexpr const char* DATA_PATH = "xbmc/filesystem/test/data/audiobook/";

//! Expand one fixture the way a music scan would.
void Expand(const std::string& fixture, CFileItemList& items)
{
  const CURL url(XBMC_REF_FILE_PATH(DATA_PATH + fixture));
  CAudioBookFileDirectory dir;
  ASSERT_TRUE(dir.ContainsFiles(url)) << fixture;
  ASSERT_TRUE(dir.GetDirectory(url, items)) << fixture;
}
} // unnamed namespace

TEST(TestAudioBookFileDirectory, MakesOneTrackPerTaggedChapter)
{
  CFileItemList items;
  Expand("chaptered.mka", items);

  ASSERT_EQ(3, items.Size());
  const auto& first = *items[0]->GetMusicInfoTag();
  EXPECT_EQ("Live At The Test Venue", first.GetAlbum());
  EXPECT_EQ("Opening Number", first.GetTitle());
  EXPECT_EQ("The Test Band", first.GetArtistString());
  EXPECT_EQ(1, first.GetTrackNumber());
  EXPECT_EQ(0, items[0]->GetStartOffset());
  EXPECT_EQ(3000, items[0]->GetEndOffset());

  EXPECT_EQ("Someone's Song", items[1]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Encore", items[2]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(3, items[2]->GetMusicInfoTag()->GetTrackNumber());
  EXPECT_EQ(9000, items[2]->GetEndOffset());
}

/*!
 * A chapter numbered from one keeps its own tags. Every nonzero ChapterUID names a chapter, and a
 * tag whose chapter is not found would be promoted to the album instead - taking the album's title
 * for its own and leaving the track unnamed.
 */
TEST(TestAudioBookFileDirectory, ReadsTagsOfAChapterNumberedFromOne)
{
  CFileItemList items;
  Expand("lowchapteruid.mka", items);

  ASSERT_EQ(2, items.Size());
  EXPECT_EQ("TAG Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("TAG Someone's Song", items[1]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(5, items[0]->GetMusicInfoTag()->GetTrackNumber());
  EXPECT_EQ(6, items[1]->GetMusicInfoTag()->GetTrackNumber());
}

//! A chapter named only by its ChapterDisplay still names its track, rather than taking the album's.
TEST(TestAudioBookFileDirectory, NamesTracksAfterTheirChapterDisplayName)
{
  CFileItemList items;
  Expand("chapternames-only.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Someone's Song", items[1]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Encore", items[2]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Live At The Test Venue", items[0]->GetMusicInfoTag()->GetAlbum());
}

//! Where a chapter has both, its own TITLE tag is the better source and wins.
TEST(TestAudioBookFileDirectory, PrefersAChapterTitleTagToItsDisplayName)
{
  CFileItemList items;
  Expand("precedence.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("TAG Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("TAG Encore", items[2]->GetMusicInfoTag()->GetTitle());
}

/*!
 * The fixture holds a 0.4s chapter between the first and second track. Dropping it must not shift
 * the tags of everything after it onto the wrong chapter, which is what indexing one reader's
 * chapter list by another reader's position did.
 */
TEST(TestAudioBookFileDirectory, DropsAMicroChapterWithoutShiftingTheRest)
{
  CFileItemList items;
  Expand("microchapter.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(0, items[0]->GetStartOffset());
  EXPECT_EQ(3000, items[0]->GetEndOffset());

  EXPECT_EQ("Someone's Song", items[1]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(3400, items[1]->GetStartOffset());
  EXPECT_EQ(6400, items[1]->GetEndOffset());

  EXPECT_EQ("Encore", items[2]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(6400, items[2]->GetStartOffset());
  EXPECT_EQ(9400, items[2]->GetEndOffset());
}

//! PART_NUMBER is what the tagger meant the track to be, so it outranks the position in the file.
TEST(TestAudioBookFileDirectory, KeepsTheTrackNumberAChapterGivesItself)
{
  CFileItemList items;
  Expand("partnumber.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ(5, items[0]->GetMusicInfoTag()->GetTrackNumber());
  EXPECT_EQ(6, items[1]->GetMusicInfoTag()->GetTrackNumber());
  EXPECT_EQ(7, items[2]->GetMusicInfoTag()->GetTrackNumber());
}

/*!
 * A file can hold several editions and only one of them is followed. Tags targeting a chapter of
 * an edition that was not selected describe tracks this file does not produce, so they must reach
 * neither the tracks nor the album.
 */
TEST(TestAudioBookFileDirectory, IgnoresTagsFromAnEditionItDidNotSelect)
{
  CFileItemList items;
  Expand("twoeditions.mka", items);

  ASSERT_EQ(3, items.Size());
  for (int i = 0; i < items.Size(); ++i)
  {
    EXPECT_NE("FOREIGN TITLE", items[i]->GetMusicInfoTag()->GetTitle()) << i;
    for (const auto& c : items[i]->GetMusicInfoTag()->GetContributors())
      EXPECT_NE("Foreign Composer", c.GetArtist()) << i;
  }
}

#ifdef HAS_TAGLIB_MATROSKA
/*!
 * The Matroska spec writes one SimpleTag per value, so three composers are three tags. Keeping only
 * the last is the fidelity FFmpeg's demuxer cannot offer and TagLib can.
 */
TEST(TestAudioBookFileDirectory, KeepsEveryValueOfARepeatedTag)
{
  CFileItemList items;
  Expand("repeated.mka", items);

  ASSERT_EQ(3, items.Size());
  const auto& contributors = items[0]->GetMusicInfoTag()->GetContributors();
  std::vector<std::string> composers;
  for (const auto& c : contributors)
    if (c.GetRoleDesc() == "Composer")
      composers.emplace_back(c.GetArtist());

  EXPECT_EQ((std::vector<std::string>{"Bill Evans", "Miles Davis", "Gil Evans"}), composers);

  /*!
   * The same, for a name written the spaced way. Which spelling a tagger chose must not decide
   * whether a repeat is a second value or a replacement.
   */
  const std::string sort = items[0]->GetMusicInfoTag()->GetArtistSort();
  EXPECT_NE(std::string::npos, sort.find("Band, The")) << sort;
  EXPECT_NE(std::string::npos, sort.find("Other, The")) << sort;
}

#endif // TagLib >= 2.3.1

/*!
 * A file with chapters but no tags is not an album, whichever reader read it. FFmpeg reports the
 * mandatory MuxingApp element as metadata, so a file always arrives carrying something.
 */
TEST(TestAudioBookFileDirectory, RefusesAChapteredFileThatNamesNoAlbum)
{
  const CURL url(XBMC_REF_FILE_PATH(std::string(DATA_PATH) + "untagged.mka"));
  CAudioBookFileDirectory dir;

  EXPECT_FALSE(dir.ContainsFiles(url));
}

/*!
 * A chapter that declares no end looks zero seconds long where it also starts at zero, which is
 * every first chapter of a file whose ChapterTimeEnds are unset - and such files are common enough
 * that scanning a concert lost its opening song (xbmc/xbmc#28902). Ends are worked out from the
 * next chapter's start before anything decides what is too short to be a track, so the whole
 * running order survives, not just the first one.
 */
TEST(TestAudioBookFileDirectory, KeepsEveryChapterOfAFileThatDeclaresNoEnds)
{
  CFileItemList items;
  Expand("noendtimes.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(0, items[0]->GetStartOffset());
  EXPECT_EQ(3000, items[0]->GetEndOffset());
  EXPECT_EQ(3000, items[1]->GetStartOffset());
  EXPECT_EQ(6000, items[1]->GetEndOffset());
  EXPECT_EQ(9000, items[2]->GetEndOffset());
}

/*!
 * A file whose album tags carry no TargetTypeValue is still an album. The spec asks for one and
 * taggers skip it, which is why such a tag is read as describing the file - and a file that says
 * what album it is, is worth expanding however it said it.
 */
TEST(TestAudioBookFileDirectory, ExpandsAnAlbumTaggedWithNoLevel)
{
  CFileItemList items;
  Expand("nolevel.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("Live At The Test Venue", items[0]->GetMusicInfoTag()->GetAlbum());
}

/*!
 * The Segment's Duration is as optional as ChapterTimeEnd, and a file that wrote neither says
 * nothing about where its last chapter stops. That is not a chapter of no length: reading it as
 * one made the last real track of the album vanish from the listing, tags and all. A chapter
 * nothing could close runs to the end of the file, and is the last track.
 */
TEST(TestAudioBookFileDirectory, KeepsTheLastTrackOfAFileThatDeclaresNoLength)
{
  CFileItemList items;
  Expand("noduration.mka", items);

  ASSERT_EQ(3, items.Size());
  EXPECT_EQ("Opening Number", items[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Someone's Song", items[1]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("Encore", items[2]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ(3, items[2]->GetMusicInfoTag()->GetTrackNumber());

  // The starts are the one thing the file did declare, so they stay exact.
  EXPECT_EQ(0, items[0]->GetStartOffset());
  EXPECT_EQ(3000, items[1]->GetStartOffset());
  EXPECT_EQ(6000, items[2]->GetStartOffset());

  // The chapters the next one closes keep their real ends; only the last is left to be worked out.
  EXPECT_EQ(3000, items[0]->GetEndOffset());
  EXPECT_EQ(6000, items[1]->GetEndOffset());
}

/*!
 * Nothing stops a caller handing one instance a second URL. The demuxer context the first file was
 * opened through used to outlive that file: only what had been parsed was re-keyed on the URL, so
 * the second file was described by the first one's cover art and codec details - and, where FFmpeg
 * is the reader, by its chapters and tags too. Both are refreshed together now.
 */
TEST(TestAudioBookFileDirectory, ReadsTheSecondFileWhenReusedAcrossURLs)
{
  CAudioBookFileDirectory dir;

  const CURL first(XBMC_REF_FILE_PATH(std::string(DATA_PATH) + "chaptered.mka"));
  CFileItemList firstItems;
  ASSERT_TRUE(dir.ContainsFiles(first));
  ASSERT_TRUE(dir.GetDirectory(first, firstItems));
  ASSERT_EQ(3, firstItems.Size());
  EXPECT_EQ("Opening Number", firstItems[0]->GetMusicInfoTag()->GetTitle());

  // The same instance, another file, asked for straight away: a caller holding a directory it has
  // already established contains files has no reason to ask that again.
  const CURL second(XBMC_REF_FILE_PATH(std::string(DATA_PATH) + "precedence.mka"));
  CFileItemList secondItems;
  ASSERT_TRUE(dir.GetDirectory(second, secondItems));

  ASSERT_EQ(3, secondItems.Size());
  EXPECT_EQ("TAG Opening Number", secondItems[0]->GetMusicInfoTag()->GetTitle());
  EXPECT_EQ("TAG Encore", secondItems[2]->GetMusicInfoTag()->GetTitle());
}
