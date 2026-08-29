// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Qt>

namespace trackknife::ui {

// Shared role and column contract for the album-grouped track presentation
// (QueueItemDelegate/QueueTableView). Any model rendered by the shared
// delegate provides these roles and the six-column layout below; the MPD
// queue model and Trackbench's local list model both implement it.
enum TrackRowRole : int {
    track_source_role = Qt::UserRole + 1, // MPD URI or raw local path bytes
    track_id_role,                        // stable per-occurrence identity
    track_position_role,                  // display position
    track_duration_ms_role,               // qint64 duration in milliseconds
    track_current_role,                   // bool: the playing occurrence
    track_album_artist_role,              // grouping artist with fallbacks
    track_priority_role,                  // uint MPD priority; 0/absent hides
    track_album_artwork_role,             // QImage cover for the group header
    track_album_artwork_key_role,         // artwork cache identity
};

// Column layout the shared delegate paints: 0 marker/cover, 1 title,
// 2 album, 3 date, 4 track number, 5 duration text.
enum TrackRowColumn : int {
    track_marker_column = 0,
    track_title_column = 1,
    track_album_column = 2,
    track_date_column = 3,
    track_number_column = 4,
    track_length_column = 5,
    track_column_count = 6,
};

} // namespace trackknife::ui
