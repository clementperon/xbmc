/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MatroskaTagMapping.h"

#include "MusicInfoTag.h"
#include "utils/StringUtils.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace MUSIC_INFO;

namespace
{
/*!
 * \brief Apply one tag, reporting whether the name was one this knows.
 *
 * Separate from MapTag(), which handles the names whose field depends on the level before
 * falling through to here for the ones that do not.
 */
bool Map(const std::string& key,
         const std::string& value,
         const std::vector<std::string>& separators,
         const std::string& musicsep,
         CMusicInfoTag& tag);

void AddRole(const std::vector<std::string>& data,
             const std::vector<std::string>& separators,
             CMusicInfoTag& musictag);
void AddCommaDelimitedString(const std::vector<std::string>& data,
                             const std::vector<std::string>& separators,
                             CMusicInfoTag& musictag);
} // namespace

void MUSIC_INFO::MatroskaTagMapping::MapTag(const std::string& key,
                                            const std::string& value,
                                            TagLevel level,
                                            const std::vector<std::string>& separators,
                                            const std::string& musicsep,
                                            CMusicInfoTag& tag)
{
  /*!
  * The names whose field the level decides. Everything else means the same thing wherever it
  * sits, and a caller applies album level tags before track level ones so that a song naming
  * itself wins over the album that named it.
  */
  if (level == TagLevel::Album)
  {
    if (key == "TITLE")
    {
      tag.SetAlbum(value);
      // Nothing else may have named the song yet; a track level TITLE still overrides this.
      tag.SetTitle(value);
      return;
    }
    if (key == "ARTIST")
    {
      tag.SetAlbumArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
      // The album's artist is the song's until the song says otherwise.
      tag.SetArtist(value);
      return;
    }
  }
  else if (level == TagLevel::File && key == "TITLE")
  {
    /*!
    * The Segment title names the file. That is the song when the file holds one, and the album
    * when nothing better named it - a file with no album level tags at all still belongs
    * somewhere.
    */
    tag.SetTitle(value);
    if (tag.GetAlbum().empty())
      tag.SetAlbum(value);
    return;
  }

  Map(key, value, separators, musicsep, tag);
}

namespace
{
bool Map(const std::string& key,
         const std::string& value,
         const std::vector<std::string>& separators,
         const std::string& musicsep,
         CMusicInfoTag& tag)
{
  /*!
  * Matroska Tag spec does not allow storing multi values in a single tag, but some tools
  * do it anyway using a delimiter. So we need to split the value using the separator and
  * then join it back using the music item separator from as.xml if needed.
  *
  * The spaced spellings (ALBUM ARTIST, ARTIST SORT, ...) are what mp3tag writes; the
  * underscored and run-together ones are what the spec and most other taggers use.
  */
  if (key == "ALBUM")
    tag.SetAlbum(value);
  else if (key == "ARTIST")
    // tag.SetArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
    tag.SetArtist(value);
  else if (key == "ARTISTS")
    tag.SetMusicBrainzArtistHints(StringUtils::Split(value, separators));
  else if (key == "ALBUMARTISTS" || key == "ALBUM_ARTISTS" || key == "ALBUM ARTISTS")
    // Split and rejoined like ALBUMARTIST below: a file carrying both spellings would otherwise
    // keep whichever the caller happened to apply last, one of them unnormalised.
    tag.SetAlbumArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "ALBUMARTIST" || key == "ALBUM_ARTIST" || key == "ALBUM ARTIST")
    tag.SetAlbumArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "TITLE")
    tag.SetTitle(value);
  else if (key == "PART_NUMBER" || key == "TRACK")
  {
    try
    {
      tag.SetTrackNumber(std::stoi(value));
    }
    catch (const std::exception&)
    {
    }
  }
  else if (key == "DISC" || key == "DISCNUMBER")
  {
    try
    {
      tag.SetDiscNumber(std::stoi(value));
    }
    catch (const std::exception&)
    {
    }
  }
  else if (key == "GENRE")
    tag.SetGenre(StringUtils::Split(value, musicsep), true);
  else if (key == "COMPILATION")
    tag.SetCompilation(true);
  else if (key == "DATE" || key == "DATE_RELEASED" || key == "YEAR")
    tag.SetReleaseDate(value);
  else if (key == "DATE_RECORDED" || key == "ORIGINALDATE" || key == "ORIGINALYEAR" ||
           key == "ORIGYEAR")
    tag.SetOriginalDate(value);
  else if (key == "MOOD")
    tag.SetMood(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  // genre could be comma delimited or not. Temporarily add the comma just in case.
  // true trims any whitespace around the genre(s)
  else if (key == "COMMENT")
    tag.SetComment(value);
  else if (key == "ARTIST-SORT" || key == "ARTISTSORT" || key == "ARTIST SORT")
    tag.SetArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "ALBUMARTISTSORT" || key == "SORT_ALBUM_ARTIST" || key == "ALBUM ARTIST SORT")
    tag.SetAlbumArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "COMPOSERSORT")
    tag.SetComposerSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "DISCSUBTITLE" || key == "SUBTITLE" || key == "SETSUBTITLE")
    tag.SetDiscSubtitle(value);
  else if (key == "MUSICBRAINZ_ARTISTID")
    tag.SetMusicBrainzArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ_ALBUMID")
    tag.SetMusicBrainzAlbumID(value);
  else if (key == "MUSICBRAINZ_RELEASEGROUPID")
    tag.SetMusicBrainzReleaseGroupID(value);
  else if (key == "MUSICBRAINZ_ALBUMARTISTID")
    tag.SetMusicBrainzAlbumArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ_TRACKID")
    tag.SetMusicBrainzTrackID(value);
  else if (key == "MUSICBRAINZ_ALBUMARTIST")
  {
    // tag.SetAlbumArtist(value);
  }
  else if (key == "MUSICBRAINZ_ALBUMTYPE")
    tag.SetMusicBrainzReleaseType(value);
  else if (key == "MUSICBRAINZ_ALBUMSTATUS")
    tag.SetAlbumReleaseStatus(value);
  else if (key == "ENCODED_BY" || key == "LANGUAGE")
  {
  }
  else if (key == "LABEL" || key == "PUBLISHER")
    tag.SetRecordLabel(value);
  else if (key == "CATALOGNUMBER")
  {
  } // No database field yet
  else if (key == "COPYRIGHT")
  {
  } // Copyright message
  else if (key == "WRITER")
    tag.AddArtistRole("Writer", StringUtils::Split(value, separators));
  else if (key == "PERFORMER")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddRole(tagdata, separators, tag);
  }
  else if (key == "ARRANGER")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddRole(tagdata, separators, tag);
  }
  else if (key == "REMIXED_BY" || key == "REMIXEDBY")
    tag.AddArtistRole("Remixer", StringUtils::Split(value, separators));
  else if (key == "MIXED_BY" || key == "MIXER")
    tag.AddArtistRole("Mixer", StringUtils::Split(value, separators));
  else if (key == "LYRICIST")
    tag.AddArtistRole("Lyricist", StringUtils::Split(value, separators));
  else if (key == "COMPOSER")
    tag.AddArtistRole("Composer", StringUtils::Split(value, separators));
  else if (key == "CONDUCTOR")
    tag.AddArtistRole("Conductor", StringUtils::Split(value, separators));
  else if (key == "ENGINEER")
    tag.AddArtistRole("Engineer", StringUtils::Split(value, separators));
  else if (key == "PRODUCER")
    tag.AddArtistRole("Producer", StringUtils::Split(value, separators));
  else if (key == "BAND")
    tag.AddArtistRole("Band", StringUtils::Split(value, separators));
  // comma separated list of role, person
  else if (key == "INVOLVEDPEOPLE" || key == "ACTOR")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, ",");

    AddCommaDelimitedString(tagdata, separators, tag);
  }
  else if (key == "INSTRUMENTS")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, ",");

    AddCommaDelimitedString(tagdata, separators, tag);
  }
  else
    return false;

  return true;
}

void AddRole(const std::vector<std::string>& data,
             const std::vector<std::string>& separators,
             CMusicInfoTag& musictag)
{
  if (!data.empty())
  {
    for (size_t i = 0; i + 1 < data.size(); i += 2)
    {
      std::vector<std::string> roles = StringUtils::Split(data[i], separators);
      for (auto& role : roles)
      {
        StringUtils::Trim(role);
        StringUtils::ToCapitalize(role);
        musictag.AddArtistRole(role, StringUtils::Split(data[i + 1], separators));
      }
    }
  }
}

void AddCommaDelimitedString(const std::vector<std::string>& data,
                             const std::vector<std::string>& separators,
                             CMusicInfoTag& musictag)
{
  if (!data.empty())
  {
    for (size_t i = 0; i + 1 < data.size(); i += 2)
    {
      std::vector<std::string> roles = StringUtils::Split(data[i], separators);
      for (auto& role : roles)
      {
        StringUtils::Trim(role);
        StringUtils::ToCapitalize(role);
        musictag.AddArtistRole(role, StringUtils::Split(data[i + 1], ","));
      }
    }
  }
}
} // namespace

/*!
* Every spelling MapTag() accepts for a field that holds several values. The run-together, spaced
* and underscored forms of one name sit together: a tagger's choice of spelling must not decide
* whether a repeat is a second value or a replacement.
*/
bool MUSIC_INFO::MatroskaTagMapping::HoldsSeveralValues(const std::string& name)
{
  constexpr std::array<const char*, 33> names = {"ALBUM ARTIST",
                                                 "ALBUM ARTIST SORT",
                                                 "ALBUM ARTISTS",
                                                 "ALBUMARTIST",
                                                 "ALBUMARTISTS",
                                                 "ALBUMARTISTSORT",
                                                 "ALBUM_ARTIST",
                                                 "ALBUM_ARTISTS",
                                                 "ARRANGER",
                                                 "ARTIST",
                                                 "ARTIST SORT",
                                                 "ARTIST-SORT",
                                                 "ARTISTS",
                                                 "ARTISTSORT",
                                                 "BAND",
                                                 "COMPOSER",
                                                 "COMPOSERSORT",
                                                 "CONDUCTOR",
                                                 "ENGINEER",
                                                 "GENRE",
                                                 "LYRICIST",
                                                 "MIXED_BY",
                                                 "MIXER",
                                                 "MOOD",
                                                 "MUSICBRAINZ_ALBUMARTISTID",
                                                 "MUSICBRAINZ_ARTISTID",
                                                 "PERFORMER",
                                                 "PRODUCER",
                                                 "REMIXED",
                                                 "REMIXEDBY",
                                                 "REMIXED_BY",
                                                 "SORT_ALBUM_ARTIST",
                                                 "WRITER"};

  return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
