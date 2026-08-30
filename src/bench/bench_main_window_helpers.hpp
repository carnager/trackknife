// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"

#include <QString>
#include <QStringList>

#include <array>
#include <string>

namespace trackknife::bench {

struct TrackColumnSpec {
    int logical;
    const char* id;
    const char* label;
    int default_width;
    int minimum_width;
};

inline constexpr std::array<TrackColumnSpec, local_column_count> track_column_specs{{
    {local_artwork_column, "artwork", "Artwork", 110, 72},
    {local_artist_column, "artist", "Artist", 150, 72},
    {local_track_number_column, "track-number", "Track number", 46, 36},
    {local_title_column, "title", "Title", 220, 96},
    {local_album_column, "album", "Album", 160, 72},
    {local_date_column, "date", "Date", 64, 52},
    {local_length_column, "length", "Length", 68, 56},
}};

[[nodiscard]] QString trackColumnId(int logical);
[[nodiscard]] int trackColumnLogical(const QString& id);
[[nodiscard]] QStringList trackColumnIds();
[[nodiscard]] QString displayText(const std::string& utf8);
[[nodiscard]] std::string utf8Bytes(const QString& text);
[[nodiscard]] QString formatTime(qint64 milliseconds);

} // namespace trackknife::bench
