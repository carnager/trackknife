// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractTableModel>
#include <QHash>
#include <QImage>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// Trackknife's local presentation has a real artwork/status column followed by
// independent metadata columns. Shared semantic roles still let the grouped
// delegate and queue view consume the model without owning local-file state.
enum LocalTrackColumn : int {
    local_artwork_column = ui::track_artwork_column,
    local_artist_column = ui::track_artist_column,
    local_track_number_column = ui::track_number_column,
    local_title_column = ui::track_title_column,
    local_album_column = ui::track_album_column,
    local_date_column = ui::track_date_column,
    local_length_column = ui::track_length_column,
    local_column_count = ui::track_column_count,
};

// One row of a Trackknife working list: raw Linux path bytes plus the
// display metadata enriched by the background probe or restored from the
// persisted document.
struct LocalTrackRow {
    std::string raw_path;
    std::optional<std::string> logical_reference;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string date;
    std::string track_number;
    std::optional<std::int64_t> duration_ms;
    // The display columns above are a projection of this ordered, multi-value
    // document. The observed revision is retained only as stale-read evidence;
    // any future mutation must revalidate it immediately before commit.
    metadata::MetadataDocument metadata;
    std::optional<core::LocalSourceRevision> source_revision;
    // True once a probe ran or persisted metadata was restored; unprobed rows
    // fall back to their file name and are queued for enrichment.
    bool probed{false};

    friend bool operator==(const LocalTrackRow&, const LocalTrackRow&) = default;
};

struct LocalTrackSource {
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;

    friend bool operator==(const LocalTrackSource&, const LocalTrackSource&) = default;
};

// Table model over one Trackknife working list, implementing the shared
// album-grouped semantic role contract (uicommon/track_row_roles.hpp). Its
// seven-column Trackknife projection keeps artwork, artist, number, and title
// independently arrangeable. Rows hold raw path bytes; presentation uses the
// lossless escaped form from the core.
class LocalListModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    explicit LocalListModel(QObject* parent = nullptr);

    void replaceRows(std::vector<LocalTrackRow> rows);
    void appendPaths(std::vector<std::string> raw_paths, int insertion_row = -1);
    void appendRows(std::vector<LocalTrackRow> rows, int insertion_row = -1);
    void removeRowIndexes(std::vector<int> rows);
    // Moves the given rows (ascending, deduplicated) as one block to
    // insertion_row, preserving their relative order.
    void reorderRows(std::vector<int> rows, int insertion_row);
    // Applies probe metadata to the row holding raw_path (hint first, then
    // search); returns false when the row no longer exists.
    bool applyMetadata(const std::string& raw_path, int hint_row, LocalTrackRow metadata);
    // Replaces one still-provisional whole-file row with one enriched row or
    // several logical chapter rows. The first row keeps the original model
    // position and any following rows are inserted directly after it.
    bool applyProbeRows(const std::string& raw_path, int hint_row, std::vector<LocalTrackRow> rows);
    // Refreshes every duplicate/logical occurrence of one verified physical
    // source while retaining annotation, segment, and sidecar layers.
    [[nodiscard]] core::Result<std::size_t>
    applyCommittedMetadata(const std::string& raw_path, const metadata::MetadataDocument& document,
                           const core::LocalSourceRevision& published_revision);
    // Advances every in-memory occurrence of one durably relocated physical
    // source while retaining logical identities and playback selection.
    [[nodiscard]] core::Result<std::size_t>
    applyCommittedRelocation(const std::string& source_raw_path, const std::string& target_raw_path,
                             const core::LocalSourceRevision& previous_revision,
                             const core::LocalSourceRevision& published_revision);
    // Marks the playing occurrence rendered by the shared delegate; an empty
    // path clears it.
    void setCurrentPath(std::string raw_path, int hint_row);
    void setCurrentSource(LocalTrackSource source, int hint_row);
    // The delegate's album-grouping identity for a row; artwork is keyed and
    // painted per group.
    [[nodiscard]] QString groupKey(int row) const;
    [[nodiscard]] bool hasArtwork(const QString& key) const { return artwork_.contains(key); }
    void setArtwork(const QString& key, QImage image);
    [[nodiscard]] const std::vector<LocalTrackRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] std::string rawPath(int row) const;
    [[nodiscard]] LocalTrackSource source(int row) const;
    // Finds the row holding raw_path, preferring the hint row so duplicate
    // occurrences re-anchor to the same position after edits.
    [[nodiscard]] int rowOfPath(const std::string& raw_path, int hint_row) const;
    [[nodiscard]] int rowOfSource(const LocalTrackSource& source, int hint_row) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  private:
    void refreshCurrentRow();
    void emitRowChanged(int row);
    void emitCurrentRowChanged(int row);

    std::vector<LocalTrackRow> rows_;
    QHash<QString, QImage> artwork_;
    LocalTrackSource current_source_;
    int current_row_{-1};
};

} // namespace trackknife::bench
