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
#include "MatroskaTagLibStream.h"
#include "MatroskaTagMapping.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <memory>
#include <set>

#include <commons/ilog.h>
#include <taglib/audioproperties.h>
#include <taglib/matroskachapteredition.h>
#include <taglib/matroskachapters.h>
#include <taglib/matroskafile.h>
#include <taglib/matroskaproperties.h>
#include <taglib/matroskasimpletag.h>
#include <taglib/matroskatag.h>
#include <taglib/tlist.h>
#include <taglib/tstring.h>
#endif

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>
}

using namespace MUSIC_INFO;
#ifdef HAS_TAGLIB_MATROSKA
using namespace TagLib;
#endif

namespace
{
//! Harvest one FFmpeg metadata dictionary into the map MatroskaTagMapping expects.
void CollectTags(const AVDictionary* metadata, std::map<std::string, std::string>& tags)
{
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)))
    tags[StringUtils::ToUpper(entry->key)] = entry->value;
}

/*!
 * Sort the file's own metadata into the level each tag belongs to.
 *
 * FFmpeg has no TargetTypeValue to carry, so it prefixes a name with the TargetType the file gave
 * that level, and a slash: a TargetTypeValue 50 COMPOSER written with TargetType "ALBUM" arrives as
 * ALBUM/COMPOSER. TargetType is optional and the spec names several per level
 * (https://www.matroska.org/technical/tagging.html), so every name for the two levels that reach a
 * whole file is matched, not "ALBUM" alone.
 *
 * A prefix naming any other level describes something this reader has no place for - a collection,
 * a part - and is dropped rather than filed where it does not belong.
 */
void CollectFileTags(const AVDictionary* metadata, MatroskaAlbum& album)
{
  //! TargetTypeValue 50 and 60: what the file as a whole is part of.
  static constexpr std::array<std::string_view, 11> albumLevel = {
      "ALBUM", "OPERA",  "CONCERT", "MOVIE",  "EPISODE", "EDITION",
      "ISSUE", "VOLUME", "OPUS",    "SEASON", "SEQUEL"};
  //! TargetTypeValue 30. In the file's own metadata it names no chapter, so it describes the file.
  static constexpr std::array<std::string_view, 3> trackLevel = {"TRACK", "SONG", "CHAPTER"};

  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)))
  {
    const std::string key = StringUtils::ToUpper(entry->key);

    /*!
    * Not tags at all: FFmpeg reports MuxingApp as an encoder and DateUTC as a creation time, both
    * mandatory or near enough that every Matroska would look like it carries tags. Neither is
    * mapped to a music field, so dropping them costs nothing and lets what remains answer whether
    * the file was tagged.
    */
    if (key == "ENCODER" || key == "CREATION_TIME")
      continue;

    const size_t slash = key.find('/');
    if (slash == std::string::npos)
    {
      album.fileTags[key] = entry->value;
      continue;
    }

    const std::string_view prefix{key.data(), slash};
    const std::string name = key.substr(slash + 1);
    if (std::find(albumLevel.begin(), albumLevel.end(), prefix) != albumLevel.end())
      album.albumTags[name] = entry->value;
    else if (std::find(trackLevel.begin(), trackLevel.end(), prefix) != trackLevel.end())
      album.fileTags[name] = entry->value;
  }
}

/*!
 * Read with FFmpeg's demuxer, which flattens the SimpleTag hierarchy: TargetTypeValue is lost -
 * album level tags arrive prefixed with the TargetType name instead - nesting goes with it, and
 * only the last of a repeated set of SimpleTags survives (https://trac.ffmpeg.org/ticket/9641).
 *
 * Editions it does not model at all: every EditionEntry's chapters are nested into one list and
 * kept if they start after the last one kept. A second edition rerunning the same timeline
 * vanishes, one starting later does not - so unlike the TagLib reader this one can report a
 * chapter the file will not play, and cannot tell that it has.
 *
 * Compiled whether or not it is the reader this build uses, so that it cannot rot unnoticed in
 * the builds that have TagLib.
 */
MatroskaAlbum ReadWithFFmpegImpl(const AVFormatContext* fctx)
{
  MatroskaAlbum album;

  if (!fctx)
    return album;

  CollectFileTags(fctx->metadata, album);

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

  return album;
}

#ifdef HAS_TAGLIB_MATROSKA

const std::vector<std::string> SupportedArtistMultiValueSeparators = {";", "|"};
const std::vector<std::string> SupportedMultiValueSeparators = {";", "/", "|", ","};

/*!
* Translate multiple single key tags (Matrosk spec) to delimited a single for internal use.
* Appends " / " + newValue to currentValue if newValue is not already present
* (case-insensitive) among the existing delimited values. The set of delimiters
* used to split currentValue depends on whether tagname refers to an artist tag.
* Returns true if the value was appended, false otherwise.
*/
bool AppendIfNotDuplicate(std::string& currentValue,
                          const std::string& newValue,
                          const std::string& tagname)
{
  const std::vector<std::string>& separators = (tagname.find("ARTIST") != std::string::npos)
                                                   ? SupportedArtistMultiValueSeparators
                                                   : SupportedMultiValueSeparators;

  try
  {
    std::vector<std::string> existingValues = StringUtils::Split(currentValue, separators);

    for (auto& existing : existingValues)
    {
      StringUtils::Trim(existing);
      if (existing.empty())
        continue; // mirrors RemoveEmptyEntries
      if (StringUtils::EqualsNoCase(existing, newValue))
        return false;
    }
  }
  catch (const std::exception& ex)
  {
    CLog::Log(LOGERROR, "AppendIfNotDuplicate: {}", ex.what());
    return false;
  }

  if (currentValue.empty())
    currentValue = newValue;
  else
    currentValue += " / " + newValue;

  return true;
}

/*!
* Record one tag, keeping any value the same name already carries if the name is one that holds
* several. The key is what the map is filed under and the name is what decides that, which differ
* for an album level tag: ALBUM/ARTIST is filed apart from the track's, but takes several values
* for the same reason ARTIST does.
*/
void AddTagValue(std::map<std::string, std::string>& tags,
                 const std::string& key,
                 const std::string& name,
                 const std::string& value)
{
  const auto it = tags.find(key);
  if (it == tags.end())
  {
    tags.emplace(key, value);
    return;
  }

  if (MatroskaTagMapping::HoldsSeveralValues(name))
    AppendIfNotDuplicate(it->second, value, name);
}

//! What the Segment says about the file itself, as opposed to the album it holds.
struct FileProperties
{
  double duration = 0.0; //!< seconds
  std::string segmentTitle;
};

FileProperties ReadFileProperties(TagLib::Matroska::File& file)
{
  const TagLib::Matroska::Properties* audioProps = file.audioProperties();
  if (!audioProps)
    return {};

  // Milliseconds: lengthInSeconds() truncates, and this is what gives a last chapter with no
  // ChapterTimeEnd its end.
  return {audioProps->lengthInMilliseconds() / 1000.0, audioProps->title().to8Bit(true)};
}

//! The edition the chapters were taken from, as the tag pass needs to know it.
struct SelectedEdition
{
  unsigned long long uid = 0; //!< What a tag naming this edition carries.

  //! The chapters of the editions left behind, so that a tag naming one of them can be told apart
  //! from a tag naming a chapter this file does not have at all.
  std::set<unsigned long long> unselectedChapterUids;

  //! Where a chapter's tags live, by the ChapterUID the SimpleTags name it with.
  std::map<unsigned long long, size_t> chapterIndex;
};

/*!
* Collect one edition's chapters in file order, each keeping its display name so that a chapter
* carrying no tags of its own still has something to name its track with.
*
* A file can hold several editions - an ordered presentation cut alongside the full transfer, say -
* of which only one is what gets played. Take the one flagged default, falling back to the first, so
* the tracks come from a single running order instead of every edition's chapters concatenated.
*/
SelectedEdition CollectChapters(TagLib::Matroska::File& file, MatroskaAlbum& album)
{
  SelectedEdition selected;

  const TagLib::Matroska::Chapters* chapters = file.chapters();
  if (!chapters)
    return selected;

  const TagLib::Matroska::Chapters::ChapterEditionList& editions = chapters->chapterEditionList();
  const TagLib::Matroska::ChapterEdition* selectedEdition = nullptr;
  for (const auto& edition : editions)
  {
    if (!selectedEdition || edition.isDefault())
      selectedEdition = &edition;
    if (edition.isDefault())
      break;
  }

  for (const auto& edition : editions)
  {
    if (&edition == selectedEdition)
      continue;
    for (const auto& chapter : edition.chapterList())
      selected.unselectedChapterUids.insert(chapter.uid());
  }

  if (!selectedEdition)
    return selected;

  selected.uid = selectedEdition->uid();
  for (const auto& chapter : selectedEdition->chapterList())
  {
    const unsigned long long chapUid = chapter.uid();

    std::string chapterName;
    if (chapUid > 0 && !chapter.displayList().isEmpty())
    {
      // Match VB behavior: keep the last display name
      for (const auto& display : chapter.displayList())
        chapterName = display.string().toCString(true);
    }

    selected.chapterIndex[chapUid] = album.chapters.size();
    ChapterTags& entry = album.chapters.emplace_back();
    entry.tags.emplace("CHAPTERNAME", chapterName);
    entry.start = static_cast<double>(chapter.timeStart()) / 1000000000.0;
    entry.end = static_cast<double>(chapter.timeEnd()) / 1000000000.0;
  }

  return selected;
}

/*!
* Give an end to any chapter that declares none, which is out of spec but written anyway: the next
* chapter's start, or the file duration for the last one.
*/
void FillMissingEndTimes(MatroskaAlbum& album, double fileDuration)
{
  for (size_t i = 0; i < album.chapters.size(); ++i)
  {
    if (album.chapters[i].end > 0.0)
      continue;
    album.chapters[i].end =
        (i + 1 < album.chapters.size()) ? album.chapters[i + 1].start : fileDuration;
  }
}

/*!
* Sort every SimpleTag onto the album or onto a chapter.
*
* Album level tags go first so that a TargetTypeValue 30 tag reaching the album finds one already
* there rather than establishing it. A tag naming an edition belongs to that edition alone: files
* with several carry one TITLE each, and taking whichever came first names the album after an
* edition that is not the one being read. A zero EditionUID applies to all editions.
*/
void CollectSimpleTags(const TagLib::Matroska::SimpleTagsList& list,
                       MatroskaAlbum& album,
                       const SelectedEdition& edition)
{
  auto& fileTags = album.fileTags;

  auto namesAnotherEdition = [&edition](const TagLib::Matroska::SimpleTag& tag)
  { return tag.editionUid() != 0 && tag.editionUid() != edition.uid; };
  // An edition, volume or opus tag describes a grouping above the album, which for one file is
  // still the whole file - so it lands on the album rather than nowhere.
  auto isAlbumLevel = [](TagLib::Matroska::SimpleTag::TargetTypeValue level)
  {
    return level == TagLib::Matroska::SimpleTag::Album ||
           level == TagLib::Matroska::SimpleTag::Edition;
  };

  for (const TagLib::Matroska::SimpleTag& tag : list)
  {
    if (!isAlbumLevel(tag.targetTypeValue()) || namesAnotherEdition(tag))
      continue;

    const std::string name = StringUtils::ToUpper(tag.name().to8Bit(true));
    AddTagValue(album.albumTags, name, name, tag.toString().to8Bit(true));
  }

  for (const TagLib::Matroska::SimpleTag& tag : list)
  {
    const TagLib::Matroska::SimpleTag::TargetTypeValue level = tag.targetTypeValue();
    if (isAlbumLevel(level) || namesAnotherEdition(tag))
      continue;

    const std::string name = StringUtils::ToUpper(tag.name().to8Bit(true));
    const std::string value = tag.toString().to8Bit(true);

    /*!
    * A tag with no TargetTypeValue describes the file: taggers that ignore the spec write album
    * metadata that way, and dropping it would lose the lot.
    */
    if (level == TagLib::Matroska::SimpleTag::None)
    {
      AddTagValue(fileTags, name, name, value);
      continue;
    }

    if (level != TagLib::Matroska::SimpleTag::Track)
      continue;

    /*!
    * A tag naming a chapter of an edition that was not selected describes a track this file will
    * not produce. Neither merging it into a chapter it does not describe nor promoting it to the
    * album is right, so it goes no further.
    */
    const unsigned long long chapterUid = tag.chapterUid();
    if (edition.unselectedChapterUids.count(chapterUid) != 0)
      continue;

    /*!
    * A tag with no ChapterUID describes the only track there is - MP3tag writes song tags that
    * way - so a single chapter file takes it. One naming a chapter goes to that chapter, and
    * one naming a chapter this file does not have falls back to the album. Zero is what TagLib
    * reports for a tag carrying no ChapterUID at all; every other value names a chapter.
    */
    ChapterTags* target = nullptr;
    if (album.chapters.size() == 1)
      target = &album.chapters.front();
    else if (chapterUid > 0)
    {
      if (const auto it = edition.chapterIndex.find(chapterUid); it != edition.chapterIndex.end())
        target = &album.chapters[it->second];
    }

    // Either the file has no chapters at all, or names one it does not contain. Either way the tag
    // describes the file rather than a track of it.
    AddTagValue(target ? target->tags : fileTags, name, name, value);
  }
}

/*!
* Name a track after its chapter's display name where nothing better named it.
*
* A chapter carrying only a ChapterDisplay name still names its track - taggers that write chapter
* names rather than per-chapter tags are common. The TargetTypeValue 30 TITLE read above says the
* same thing more precisely, so it keeps precedence and this only fills the gap.
*/
void FillTitlesFromDisplayNames(MatroskaAlbum& album)
{
  for (auto& chapter : album.chapters)
  {
    const auto chapterName = chapter.tags.find("CHAPTERNAME");
    if (chapterName == chapter.tags.end() || chapterName->second.empty())
      continue;
    chapter.tags.emplace("TITLE", chapterName->second);
  }
}

/*!
 * Read with TagLib, which follows the whole SimpleTag hierarchy: TargetTypeValue, EditionUID and
 * ChapterUID all survive, so album level tags stay apart from a chapter's own, repeated tags all
 * arrive, and both stay tied to the edition they belong to.
 */
MatroskaAlbum ReadWithTagLib(const CURL& url)
{
  MatroskaAlbum album;

  MatroskaTagLibStream matroskaStream(url.Get());
  if (!matroskaStream.open())
    return album;

  std::unique_ptr<TagLib::Matroska::File> matroskaFile;
  Matroska::Tag* matroskatag = nullptr;
  try
  {
    // MatroskaTagLibStream provides a 512 KiB read-ahead buffer and deferred seeks
    matroskaFile = std::make_unique<TagLib::Matroska::File>(&matroskaStream, true,
                                                            TagLib::AudioProperties::Fast);
    if (matroskaFile->isValid())
      matroskatag = matroskaFile->tag(true);
    if (!matroskatag)
      return album;

    const FileProperties properties = ReadFileProperties(*matroskaFile);

    /*!
    * The Segment title names the file rather than its album, and FFmpeg's demuxer reports it as an
    * unprefixed title. Say the same, so that a file holding one song is titled the same way
    * whichever reader read it.
    */
    if (!properties.segmentTitle.empty())
      album.fileTags["TITLE"] = properties.segmentTitle;

    const SelectedEdition edition = CollectChapters(*matroskaFile, album);
    FillMissingEndTimes(album, properties.duration);

    CollectSimpleTags(matroskatag->simpleTagsList(), album, edition);
    FillTitlesFromDisplayNames(album);

    // bufferedStream and matroskaFile are destroyed when scope exits.
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "ReadWithTagLib: Exception while reading Matroska tags: {} {}",
              url.GetRedacted(), e.what());
  }

  return album;
}

#endif // TagLib >= 2.3.1
} // unnamed namespace

namespace
{
//! Below this a chapter is an artefact rather than a song.
constexpr long long MinimumTrackMilliseconds = 1000;
} // namespace

bool MUSIC_INFO::IsTrack(double start, double end)
{
  /*!
  * Rounded to milliseconds, which is as fine as a chapter is ever written. Both readers scale
  * nanosecond ticks into seconds before subtracting, and the difference of two such doubles lands
  * just under the whole second often enough to drop chapters that are exactly one second long.
  */
  return std::llround((end - start) * 1000.0) >= MinimumTrackMilliseconds;
}

MatroskaAlbum MUSIC_INFO::ReadMatroskaTagsWithFFmpeg(const CURL& /*url*/,
                                                     const AVFormatContext* fctx)
{
  return ReadWithFFmpegImpl(fctx);
}

#ifdef HAS_TAGLIB_MATROSKA
MatroskaAlbum MUSIC_INFO::ReadMatroskaTagsWithTagLib(const CURL& url,
                                                     const AVFormatContext* /*fctx*/)
{
  return ReadWithTagLib(url);
}
#endif

MatroskaAlbum MUSIC_INFO::ReadMatroskaTags(const CURL& url, const AVFormatContext* fctx)
{
  // TagLib wherever the version floor allows it, FFmpeg below that: fewer tags rather than none.
#ifdef HAS_TAGLIB_MATROSKA
  return ReadMatroskaTagsWithTagLib(url, fctx);
#else
  return ReadMatroskaTagsWithFFmpeg(url, fctx);
#endif
}
