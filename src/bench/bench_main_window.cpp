// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/local_list_model.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "quick/mpd_output_model.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/operation_journal.hpp"
#include "ui/mpd_connection_dialog.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {


namespace {

constexpr int persist_debounce_ms = 1'000;





constexpr std::size_t probe_batch_size = 8U;
constexpr std::size_t discovery_row_limit = 100'000U;

[[nodiscard]] std::string lowercased_ascii(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
}

// FFmpeg exposes a few format-native fields through generic AVDictionary
// spellings. Those spellings are presentation projections, not additional
// native tag identities. Keep this adapter mapping explicit so ADR-0066's
// freeform-field identity never mistakes one for an independently writable
// property.
[[nodiscard]] std::optional<std::string_view>
probed_semantic_alias(const std::string_view native_name) {
    const auto name = lowercased_ascii(std::string{native_name});
    if (name == "track") {
        return "tracknumber";
    }
    if (name == "disc") {
        return "discnumber";
    }
    if (name == "album_artist") {
        return "albumartist";
    }
    return std::nullopt;
}

void remove_shadowed_probed_metadata(metadata::MetadataDocument& document) {
    std::unordered_set<std::string> embedded_semantic_names;
    embedded_semantic_names.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        if (field.provenance == metadata::FieldProvenance::embedded) {
            embedded_semantic_names.insert(field.canonical_name);
        }
    }
    std::erase_if(document.fields, [&embedded_semantic_names](const auto& field) {
        if (field.provenance != metadata::FieldProvenance::stream) {
            return false;
        }
        const auto semantic = probed_semantic_alias(field.native_name);
        return semantic && embedded_semantic_names.contains(std::string{*semantic});
    });
}

// Case-insensitive first-value lookup over one ordered metadata scope.
[[nodiscard]] std::string probed_tag(const std::span<const formats::ProbedTag> tags,
                                     const std::string_view name) {
    for (const auto& tag : tags) {
        if (lowercased_ascii(tag.name) == name) {
            return tag.value;
        }
    }
    return {};
}

[[nodiscard]] std::string probed_tag(const formats::MediaProbe& probe,
                                     const std::string_view name) {
    return probed_tag(probe.tags, name);
}

void append_metadata_value(metadata::MetadataDocument& document, std::string native_name,
                           std::string value, const metadata::FieldProvenance provenance) {
    if (value.empty()) {
        return;
    }
    const auto canonical_name =
        metadata::resolve_text_property_identity(native_name).canonical_name;
    if (canonical_name.empty()) {
        return;
    }
    document.fields.push_back(metadata::MetadataField{
        .canonical_name = canonical_name,
        .native_name = std::move(native_name),
        .values = {std::move(value)},
        .qualifier = {},
        .provenance = provenance,
    });
}

void prepend_metadata_value(metadata::MetadataDocument& document, std::string native_name,
                            std::string value, const metadata::FieldProvenance provenance) {
    if (value.empty()) {
        return;
    }
    const auto canonical_name =
        metadata::resolve_text_property_identity(native_name).canonical_name;
    if (canonical_name.empty()) {
        return;
    }
    document.fields.insert(document.fields.begin(), metadata::MetadataField{
                                                        .canonical_name = canonical_name,
                                                        .native_name = std::move(native_name),
                                                        .values = {std::move(value)},
                                                        .qualifier = {},
                                                        .provenance = provenance,
                                                    });
}

void append_probed_metadata(metadata::MetadataDocument& document,
                            const std::span<const formats::ProbedTag> tags,
                            const metadata::FieldProvenance provenance) {
    for (const auto& tag : tags) {
        append_metadata_value(document, tag.name, tag.value, provenance);
    }
}

// TagLib is the primary generic property adapter. FFmpeg's container/stream
// projection fills only names that TagLib did not expose, retaining repeated
// values in demuxer order without duplicating the primary representation.
void append_missing_probed_metadata(metadata::MetadataDocument& document,
                                    const std::span<const formats::ProbedTag> tags) {
    std::unordered_set<std::string> primary_names;
    primary_names.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        primary_names.insert(field.canonical_name);
    }
    for (const auto& tag : tags) {
        const auto canonical_name =
            metadata::resolve_text_property_identity(tag.name).canonical_name;
        if (primary_names.contains(canonical_name)) {
            continue;
        }
        const auto semantic = probed_semantic_alias(tag.name);
        if (semantic && primary_names.contains(std::string{*semantic})) {
            continue;
        }
        append_metadata_value(document, tag.name, tag.value, metadata::FieldProvenance::stream);
    }
    remove_shadowed_probed_metadata(document);
}

[[nodiscard]] std::string
metadata_value(const metadata::MetadataDocument& document,
               const std::initializer_list<std::string_view> candidate_names) {
    for (const auto name : candidate_names) {
        if (auto value = document.first_effective_value(name)) {
            return std::move(*value);
        }
    }
    return {};
}

void project_display_metadata(LocalTrackRow& row) {
    row.title = metadata_value(row.metadata, {"title"});
    row.artist = metadata_value(row.metadata, {"artist"});
    row.album = metadata_value(row.metadata, {"album"});
    row.album_artist = metadata_value(row.metadata, {"albumartist"});
    row.date = metadata_value(row.metadata, {"date", "year"});
    row.track_number = metadata_value(row.metadata, {"tracknumber", "track"});
}

[[nodiscard]] int best_audio_sample_rate(const formats::MediaProbe& probe) {
    if (!probe.best_audio_stream) {
        return 0;
    }
    const auto found = std::ranges::find(probe.audio_streams, *probe.best_audio_stream,
                                         &formats::AudioStreamInfo::stream_index);
    return found == probe.audio_streams.end() ? 0 : found->sample_rate;
}

[[nodiscard]] std::optional<std::int64_t> sample_duration_ms(const std::int64_t frames,
                                                             const int sample_rate) {
    if (frames < 0 || sample_rate <= 0) {
        return std::nullopt;
    }
    const auto seconds = frames / sample_rate;
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1'000) {
        return std::nullopt;
    }
    return (seconds * 1'000) + (((frames % sample_rate) * 1'000) / sample_rate);
}

[[nodiscard]] LocalTrackRow
whole_file_row(const formats::MediaProbe& probe, metadata::MetadataDocument document,
               std::optional<core::LocalSourceRevision> source_revision) {
    append_missing_probed_metadata(document, probe.tags);
    LocalTrackRow row;
    row.raw_path = probe.raw_path;
    row.duration_ms = probe.duration_ms;
    row.metadata = std::move(document);
    row.source_revision = source_revision;
    project_display_metadata(row);
    row.probed = true;
    return row;
}

[[nodiscard]] std::string chapter_logical_reference(const formats::MediaProbe& probe,
                                                    const formats::ProbedChapter& chapter) {
    std::string reference{"container-chapter-v1"};
    reference.push_back('\0');
    reference += probe.raw_path;
    reference.push_back('\0');
    reference += std::to_string(probe.best_audio_stream.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(chapter.id);
    reference.push_back('\0');
    reference += std::to_string(chapter.source_index);
    return reference;
}

[[nodiscard]] std::vector<LocalTrackRow>
chapter_rows(const formats::MediaProbe& probe, const metadata::MetadataDocument& document,
             const std::optional<core::LocalSourceRevision>& source_revision) {
    const auto sample_rate = best_audio_sample_rate(probe);
    if (sample_rate <= 0 || probe.chapters.empty()) {
        return {};
    }
    std::vector<LocalTrackRow> rows;
    rows.reserve(probe.chapters.size());
    for (std::size_t index = 0U; index < probe.chapters.size(); ++index) {
        const auto& chapter = probe.chapters[index];
        LocalTrackRow row;
        row.raw_path = probe.raw_path;
        row.logical_reference = chapter_logical_reference(probe, chapter);
        row.segment = formats::SampleRange{.start_sample = chapter.start_sample,
                                           .end_sample = chapter.end_sample};
        row.metadata = document;
        row.source_revision = source_revision;
        append_probed_metadata(row.metadata, chapter.tags, metadata::FieldProvenance::segment);
        if (probed_tag(chapter.tags, "title").empty()) {
            append_metadata_value(row.metadata, "TITLE", "Chapter " + std::to_string(index + 1U),
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"album"}).empty()) {
            append_metadata_value(row.metadata, "ALBUM", metadata_value(document, {"title"}),
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"albumartist"}).empty()) {
            append_metadata_value(row.metadata, "ALBUMARTIST", metadata_value(document, {"artist"}),
                                  metadata::FieldProvenance::segment);
        }
        if (probed_tag(chapter.tags, "tracknumber").empty() &&
            probed_tag(chapter.tags, "track").empty()) {
            append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(index + 1U),
                                  metadata::FieldProvenance::segment);
        }
        row.duration_ms =
            sample_duration_ms(chapter.end_sample - chapter.start_sample, sample_rate);
        project_display_metadata(row);
        row.probed = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

[[nodiscard]] std::string subsong_logical_reference(const formats::MediaProbe& probe,
                                                    const formats::ProbedSubsong& subsong) {
    std::string reference{"codec-subsong-v1"};
    reference.push_back('\0');
    reference += probe.raw_path;
    reference.push_back('\0');
    reference += std::to_string(subsong.selection.stream_index.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(subsong.selection.subsong_index.value_or(-1));
    reference.push_back('\0');
    reference += std::to_string(subsong.source_index);
    return reference;
}

[[nodiscard]] std::vector<LocalTrackRow>
subsong_rows(const formats::MediaProbe& probe, const metadata::MetadataDocument& document,
             const std::optional<core::LocalSourceRevision>& source_revision) {
    if (probe.subsongs.empty()) {
        return {};
    }
    const auto container_title = probed_tag(probe, "title");
    std::vector<LocalTrackRow> rows;
    rows.reserve(probe.subsongs.size());
    for (std::size_t index = 0U; index < probe.subsongs.size(); ++index) {
        const auto& subsong = probe.subsongs[index];
        LocalTrackRow row;
        row.raw_path = probe.raw_path;
        row.logical_reference = subsong_logical_reference(probe, subsong);
        row.selection = subsong.selection;
        if (subsong.duration_samples && *subsong.duration_samples > 0) {
            row.segment =
                formats::SampleRange{.start_sample = 0, .end_sample = *subsong.duration_samples};
        }
        row.metadata = document;
        row.source_revision = source_revision;
        if (!subsong.name.empty()) {
            append_metadata_value(row.metadata, "TITLE", subsong.name,
                                  metadata::FieldProvenance::segment);
        }
        append_probed_metadata(row.metadata, subsong.tags, metadata::FieldProvenance::segment);
        auto title = metadata_value(row.metadata, {"title"});
        if (title.empty() || title == container_title) {
            prepend_metadata_value(row.metadata, "TITLE", "Subsong " + std::to_string(index + 1U),
                                   metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"album"}).empty()) {
            append_metadata_value(row.metadata, "ALBUM", container_title,
                                  metadata::FieldProvenance::segment);
        }
        if (metadata_value(row.metadata, {"albumartist"}).empty()) {
            append_metadata_value(row.metadata, "ALBUMARTIST",
                                  metadata_value(row.metadata, {"artist"}),
                                  metadata::FieldProvenance::segment);
        }
        append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(index + 1U),
                              metadata::FieldProvenance::segment);
        row.duration_ms = subsong.duration_ms;
        project_display_metadata(row);
        row.probed = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

constexpr int artwork_cache_extent = 128;
constexpr std::uintmax_t artwork_file_limit = 16U * 1024U * 1024U;

// Folder expansion only ingests plausible audio and external cue sheets;
// explicitly opened files always pass regardless (core discovery contract).
// The probe/resolver still gates everything this list lets through.
constexpr std::array<std::string_view, 28> audio_extensions{
    "flac", "mp3", "ogg",  "oga", "opus", "m4a", "mp4", "aac", "wv",  "wav",
    "rf64", "w64", "aiff", "aif", "aifc", "ape", "mpc", "tta", "spx", "mka",
    "wma",  "dsf", "dff",  "cue", "mod",  "xm",  "s3m", "it"};

[[nodiscard]] bool is_cue_path(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    const auto dot = raw_path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
           lowercased_ascii(raw_path.substr(dot + 1U)) == "cue";
}

[[nodiscard]] std::string cue_remark(const formats::CueLogicalTrack& track,
                                     const std::string_view name) {
    const auto found = std::ranges::find_if(
        track.remarks.rbegin(), track.remarks.rend(),
        [name](const formats::CueMetadataField& field) { return field.name == name; });
    return found == track.remarks.rend() ? std::string{} : found->value;
}

[[nodiscard]] std::string cue_logical_reference(const std::string& raw_cue_path,
                                                const formats::CueLogicalTrack& track) {
    std::string reference{"cue-v1"};
    reference.push_back('\0');
    reference += raw_cue_path;
    reference.push_back('\0');
    reference += std::to_string(track.file_index);
    reference.push_back('\0');
    reference += std::to_string(track.track_index);
    return reference;
}

[[nodiscard]] LocalTrackRow
cue_row(const formats::ResolvedCueSheet& sheet, const formats::ResolvedCueTrack& resolved,
        const metadata::MetadataDocument& embedded_document,
        const std::optional<core::LocalSourceRevision>& source_revision) {
    const auto& cue = resolved.cue;
    LocalTrackRow row;
    row.raw_path = resolved.raw_source_path;
    row.logical_reference = cue_logical_reference(sheet.raw_cue_path, cue);
    row.segment = resolved.sample_range;
    row.metadata = embedded_document;
    row.source_revision = source_revision;
    for (const auto& remark : cue.remarks) {
        append_metadata_value(row.metadata, remark.name, remark.value,
                              metadata::FieldProvenance::segment);
    }
    append_metadata_value(row.metadata, "TITLE", cue.title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ARTIST", cue.performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUM", cue.album_title.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ALBUMARTIST", cue.album_performer.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "SONGWRITER", cue.songwriter.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "ISRC", cue.isrc.value_or(std::string{}),
                          metadata::FieldProvenance::sidecar);
    // REM values retain source order at segment scope. The established CUE
    // policy selects the last DATE as the effective sidecar projection.
    append_metadata_value(row.metadata, "DATE", cue_remark(cue, "DATE"),
                          metadata::FieldProvenance::sidecar);
    append_metadata_value(row.metadata, "TRACKNUMBER", std::to_string(cue.track_number),
                          metadata::FieldProvenance::sidecar);
    row.duration_ms = resolved.duration_ms;
    project_display_metadata(row);
    row.probed = true;
    return row;
}

// Folder fallback for albums without an attached picture: the first regular
// file in the track's directory whose lowercased name is a conventional
// cover image.
[[nodiscard]] std::vector<unsigned char> folder_artwork_bytes(const std::string& raw_path) {
    const auto slash = raw_path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    const std::filesystem::path directory{raw_path.substr(0, slash)};
    static constexpr std::array names{"cover.jpg",  "cover.jpeg",  "cover.png",
                                      "folder.jpg", "folder.jpeg", "folder.png",
                                      "front.jpg",  "front.jpeg",  "front.png"};
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::directory_iterator end;
    if (error) {
        return {};
    }
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return {};
        }
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto name = lowercased_ascii(iterator->path().filename().native());
        if (std::ranges::find(names, name) == names.end()) {
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error || size == 0U || size > artwork_file_limit) {
            return {};
        }
        std::ifstream input{iterator->path(), std::ios::binary};
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!input.good() && !input.eof()) {
            return {};
        }
        return bytes;
    }
    return {};
}

[[nodiscard]] QImage decoded_artwork(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    auto image = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()));
    if (image.isNull()) {
        return {};
    }
    if (image.width() > artwork_cache_extent || image.height() > artwork_cache_extent) {
        image = image.scaled(artwork_cache_extent, artwork_cache_extent, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

} // namespace





ui::TrackViewLayout
BenchMainWindow::defaultTrackViewLayout(const ui::TrackViewPresentation presentation) const {
    std::vector<ui::TrackViewColumnLayout> columns;
    columns.reserve(track_column_specs.size());
    for (const auto& spec : track_column_specs) {
        auto width = spec.default_width;
        bool visible = true;
        if (presentation == ui::TrackViewPresentation::albums_header_artwork &&
            spec.logical == local_artwork_column) {
            width = 42;
        } else if (presentation == ui::TrackViewPresentation::plain_columns &&
                   spec.logical == local_artwork_column) {
            visible = false;
        } else if (presentation == ui::TrackViewPresentation::compact_queue) {
            visible = spec.logical == local_artist_column ||
                      spec.logical == local_track_number_column ||
                      spec.logical == local_title_column || spec.logical == local_album_column ||
                      spec.logical == local_length_column;
        }
        columns.push_back(ui::TrackViewColumnLayout{
            .id = QString::fromLatin1(spec.id), .width = width, .visible = visible});
    }
    return ui::TrackViewLayout{.schema_version = ui::track_view_layout_schema_version,
                               .presentation = presentation,
                               .columns = std::move(columns)};
}

void BenchMainWindow::applyTrackViewLayout(ListTab& tab, const ui::TrackViewLayout& layout) {
    applyTrackViewLayout(tab.view, tab.view_layout, layout);
}

void BenchMainWindow::applyTrackViewLayout(QTableView* view, ui::TrackViewLayout& state,
                                           const ui::TrackViewLayout& layout) {
    if (view == nullptr) {
        return;
    }
    auto* queue_view = static_cast<ui::QueueTableView*>(view);
    applying_track_view_layout_ = true;
    view->setProperty(ui::track_artwork_column_property, local_artwork_column);
    view->setProperty(ui::track_artist_column_property, local_artist_column);
    view->setProperty(ui::track_number_column_property, local_track_number_column);
    view->setProperty(ui::track_title_column_property, local_title_column);
    view->setProperty(ui::track_album_column_property, local_album_column);
    view->setProperty(ui::track_date_column_property, local_date_column);
    view->setProperty(ui::track_length_column_property, local_length_column);
    view->setProperty(ui::track_separate_number_property, true);
    const auto grouped = layout.presentation == ui::TrackViewPresentation::albums_side_artwork ||
                         layout.presentation == ui::TrackViewPresentation::albums_header_artwork;
    const auto side_artwork = layout.presentation == ui::TrackViewPresentation::albums_side_artwork;
    view->setProperty(ui::track_side_artwork_property, side_artwork);

    auto* previous_delegate = view->itemDelegate();
    view->setItemDelegate(grouped
                              ? static_cast<QAbstractItemDelegate*>(new ui::QueueItemDelegate(view))
                              : static_cast<QAbstractItemDelegate*>(new QStyledItemDelegate(view)));
    if (previous_delegate != nullptr && previous_delegate->parent() == view) {
        previous_delegate->deleteLater();
    }
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    queue_view->setAlbumGroupingEnabled(grouped);

    auto* header = view->horizontalHeader();
    const QSignalBlocker header_blocker{header};
    for (int visual = 0; visual < static_cast<int>(layout.columns.size()); ++visual) {
        const auto logical =
            trackColumnLogical(layout.columns[static_cast<std::size_t>(visual)].id);
        if (logical < 0) {
            continue;
        }
        const auto current_visual = header->visualIndex(logical);
        if (current_visual != visual) {
            header->moveSection(current_visual, visual);
        }
    }
    for (const auto& column : layout.columns) {
        const auto logical = trackColumnLogical(column.id);
        if (logical < 0) {
            continue;
        }
        view->setColumnHidden(logical, !column.visible);
        view->setColumnWidth(logical, column.width);
    }
    queue_view->setAlbumArtworkColumn(side_artwork ? local_artwork_column : -1);
    QHash<int, int> preferred_widths;
    QHash<int, int> minimum_widths;
    for (const auto& column : layout.columns) {
        const auto logical = trackColumnLogical(column.id);
        const auto spec = std::ranges::find(track_column_specs, logical, &TrackColumnSpec::logical);
        if (logical >= 0 && spec != track_column_specs.end()) {
            preferred_widths.insert(logical, column.width);
            minimum_widths.insert(logical, spec->minimum_width);
        }
    }
    queue_view->setAutoFillColumns({local_artist_column, local_title_column, local_album_column},
                                   std::move(preferred_widths), std::move(minimum_widths));
    state = layout;
    applying_track_view_layout_ = false;
    view->viewport()->update();
    refreshTrackViewActions();
}

ui::TrackViewLayout BenchMainWindow::captureTrackViewLayout(const ListTab& tab) const {
    return captureTrackViewLayout(tab.view, tab.view_layout);
}

ui::TrackViewLayout
BenchMainWindow::captureTrackViewLayout(const QTableView* view,
                                        const ui::TrackViewLayout& state) const {
    auto layout = state;
    layout.schema_version = ui::track_view_layout_schema_version;
    layout.columns.clear();
    auto* header = view->horizontalHeader();
    layout.columns.reserve(static_cast<std::size_t>(header->count()));
    for (int visual = 0; visual < header->count(); ++visual) {
        const auto logical = header->logicalIndex(visual);
        layout.columns.push_back(ui::TrackViewColumnLayout{
            .id = trackColumnId(logical),
            .width = std::max(24, header->sectionSize(logical)),
            .visible = !view->isColumnHidden(logical),
        });
    }
    return layout;
}

void BenchMainWindow::setTrackViewPresentation(const ui::TrackViewPresentation presentation) {
    if (isMpdContext()) {
        if (applying_track_view_layout_) {
            return;
        }
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_,
                             defaultTrackViewLayout(presentation));
        schedulePersist();
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, defaultTrackViewLayout(presentation));
    schedulePersist();
}

void BenchMainWindow::setTrackColumnVisible(const QString& column_id, const bool visible) {
    if (isMpdContext()) {
        if (applying_track_view_layout_) {
            return;
        }
        auto layout = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
        const auto visible_count =
            std::ranges::count(layout.columns, true, &ui::TrackViewColumnLayout::visible);
        const auto found =
            std::ranges::find(layout.columns, column_id, &ui::TrackViewColumnLayout::id);
        if (found == layout.columns.end() || (!visible && found->visible && visible_count == 1)) {
            refreshTrackViewActions();
            return;
        }
        found->visible = visible;
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, layout);
        schedulePersist();
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || applying_track_view_layout_) {
        return;
    }
    auto layout = captureTrackViewLayout(*tab);
    const auto visible_count =
        std::ranges::count(layout.columns, true, &ui::TrackViewColumnLayout::visible);
    const auto found = std::ranges::find(layout.columns, column_id, &ui::TrackViewColumnLayout::id);
    if (found == layout.columns.end() || (!visible && found->visible && visible_count == 1)) {
        refreshTrackViewActions();
        return;
    }
    found->visible = visible;
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, layout);
    schedulePersist();
}

void BenchMainWindow::resetTrackViewLayout() {
    if (isMpdContext()) {
        mpd_view_layout_persistence_protected_ = false;
        preserved_mpd_view_layout_.clear();
        applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, defaultTrackViewLayout());
        schedulePersist();
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    tab->view_layout_persistence_protected = false;
    tab->preserved_view_layout.clear();
    applyTrackViewLayout(*tab, defaultTrackViewLayout());
    schedulePersist();
}

void BenchMainWindow::copyTrackViewLayoutToAllTabs() {
    auto layout = ui::TrackViewLayout{};
    if (isMpdContext()) {
        layout = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
    } else if (auto* source = currentListTab(); source != nullptr) {
        layout = captureTrackViewLayout(*source);
    } else {
        return;
    }
    mpd_view_layout_persistence_protected_ = false;
    preserved_mpd_view_layout_.clear();
    applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, layout);
    for (auto& tab : list_tabs_) {
        tab->view_layout_persistence_protected = false;
        tab->preserved_view_layout.clear();
        applyTrackViewLayout(*tab, layout);
    }
    schedulePersist();
}

void BenchMainWindow::refreshTrackViewActions() {
    if (track_presentation_group_ == nullptr) {
        return;
    }
    auto* tab = currentListTab();
    const auto available = tab != nullptr || isMpdContext();
    for (auto* action :
         {track_albums_side_action_, track_albums_header_action_, track_plain_columns_action_,
          track_compact_queue_action_, track_layout_reset_action_, track_layout_copy_action_}) {
        action->setEnabled(available);
    }
    for (auto* action : track_column_actions_) {
        action->setEnabled(available);
    }
    if (!available) {
        return;
    }
    const auto layout = isMpdContext() ? captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_)
                                       : captureTrackViewLayout(*tab);
    const QSignalBlocker side_blocker{track_albums_side_action_};
    const QSignalBlocker header_blocker{track_albums_header_action_};
    const QSignalBlocker plain_blocker{track_plain_columns_action_};
    const QSignalBlocker compact_blocker{track_compact_queue_action_};
    track_albums_side_action_->setChecked(layout.presentation ==
                                          ui::TrackViewPresentation::albums_side_artwork);
    track_albums_header_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::albums_header_artwork);
    track_plain_columns_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::plain_columns);
    track_compact_queue_action_->setChecked(layout.presentation ==
                                            ui::TrackViewPresentation::compact_queue);
    for (const auto& column : layout.columns) {
        auto* action = track_column_actions_.value(column.id, nullptr);
        if (action == nullptr) {
            continue;
        }
        const QSignalBlocker blocker{action};
        action->setChecked(column.visible);
    }
}

void BenchMainWindow::showTrackViewHeaderMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr) {
        return;
    }
    tabs_->setCurrentWidget(view);
    QMenu menu(this);
    menu.addSection(QStringLiteral("Presentation"));
    menu.addAction(track_albums_side_action_);
    menu.addAction(track_albums_header_action_);
    menu.addAction(track_plain_columns_action_);
    menu.addAction(track_compact_queue_action_);
    auto* columns = menu.addMenu(QStringLiteral("Columns"));
    for (const auto& spec : track_column_specs) {
        columns->addAction(track_column_actions_.value(QString::fromLatin1(spec.id)));
    }
    menu.addSeparator();
    menu.addAction(track_layout_reset_action_);
    menu.exec(view->horizontalHeader()->mapToGlobal(position));
}

void BenchMainWindow::refreshSelectionStatus() {
    if (selection_status_ == nullptr) {
        return;
    }
    if (isMpdContext()) {
        if (properties_action_ != nullptr) {
            properties_action_->setEnabled(false);
        }
        if (mpd_queue_view_->selectionModel() == nullptr) {
            selection_status_->setText(QStringLiteral("MPD Queue"));
            return;
        }
        const auto selected = mpd_queue_view_->selectionModel()->selectedRows();
        if (selected.empty()) {
            selection_status_->setText(
                QStringLiteral("MPD Queue · %1 tracks").arg(mpd_controller_->queueCount()));
            selection_status_->setToolTip(mpd_controller_->status());
            return;
        }
        qint64 duration_ms = 0;
        for (const auto& index : selected) {
            duration_ms += index.data(ui::track_duration_ms_role).toLongLong();
        }
        const auto summary = QStringLiteral("MPD Queue · %1 selected · %2 total")
                                 .arg(selected.size())
                                 .arg(formatTime(duration_ms));
        selection_status_->setText(summary);
        selection_status_->setToolTip(summary);
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        if (properties_action_ != nullptr) {
            properties_action_->setEnabled(false);
        }
        selection_status_->setText(QStringLiteral("No tracks selected"));
        selection_status_->setToolTip({});
        return;
    }

    const auto selected = tab->view->selectionModel()->selectedRows();
    if (properties_action_ != nullptr) {
        properties_action_->setEnabled(!selected.empty());
    }
    if (selected.empty()) {
        selection_status_->setText(QStringLiteral("No tracks selected"));
        selection_status_->setToolTip({});
        return;
    }

    if (selected.size() == 1) {
        const auto row_index = selected.front().row();
        if (row_index < 0 || row_index >= static_cast<int>(tab->model->rows().size())) {
            selection_status_->setText(QStringLiteral("No tracks selected"));
            selection_status_->setToolTip({});
            return;
        }
        const auto& track = tab->model->rows()[static_cast<std::size_t>(row_index)];
        const auto fallback = tab->model->index(row_index, local_title_column).data().toString();
        const auto title = track.title.empty() ? fallback : displayText(track.title);
        QStringList details;
        details.push_back(track.artist.empty()
                              ? title
                              : QStringLiteral("%1 — %2").arg(displayText(track.artist), title));
        if (!track.album.empty() || !track.date.empty()) {
            auto release = displayText(track.album);
            if (!track.date.empty()) {
                release += release.isEmpty() ? displayText(track.date)
                                             : QStringLiteral(" (%1)").arg(displayText(track.date));
            }
            details.push_back(release);
        }
        if (track.duration_ms) {
            details.push_back(formatTime(*track.duration_ms));
        }
        const auto summary = details.join(QStringLiteral(" · "));
        selection_status_->setText(summary);
        selection_status_->setToolTip(
            QString::fromStdString(core::escape_raw_path(track.raw_path)));
        return;
    }

    qint64 total_duration_ms = 0;
    int unknown_durations = 0;
    for (const auto& index : selected) {
        if (index.row() < 0 || index.row() >= static_cast<int>(tab->model->rows().size())) {
            continue;
        }
        const auto& duration =
            tab->model->rows()[static_cast<std::size_t>(index.row())].duration_ms;
        if (duration) {
            total_duration_ms += *duration;
        } else {
            ++unknown_durations;
        }
    }
    auto summary = QStringLiteral("%1 tracks selected · %2 total")
                       .arg(selected.size())
                       .arg(formatTime(total_duration_ms));
    if (unknown_durations > 0) {
        summary += QStringLiteral(" · %1 duration unknown").arg(unknown_durations);
    }
    selection_status_->setText(summary);
    selection_status_->setToolTip(summary);
}



void BenchMainWindow::initializePersistence() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    database_path_ = std::filesystem::path{utf8Bytes(base + QStringLiteral("/lists.sqlite"))};
    persistence_ = new ui::ListPersistenceService(database_path_, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] { persistNow(false); });
    persistence_->initialize([this](ui::PersistedWorkspace workspace, QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restored_track_view_layouts_.clear();
        for (const auto& preset : workspace.view_presets) {
            restored_track_view_layouts_.insert(
                displayText(preset.binding),
                QByteArray{preset.header_state.data(),
                           static_cast<qsizetype>(preset.header_state.size())});
        }
        mpd_profiles_ = std::move(workspace.profiles);
        auto stored_mpd_layout = restored_track_view_layouts_.value(QStringLiteral("mpd:queue"));
        if (stored_mpd_layout.isEmpty()) {
            stored_mpd_layout = restored_track_view_layouts_.value(QStringLiteral("live-queue"));
        }
        if (!stored_mpd_layout.isEmpty()) {
            QString layout_error;
            if (auto decoded = ui::deserializeTrackViewLayout(stored_mpd_layout, trackColumnIds(),
                                                              &layout_error);
                decoded) {
                applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, *decoded);
            } else {
                mpd_view_layout_persistence_protected_ = true;
                preserved_mpd_view_layout_ = stored_mpd_layout;
                statusBar()->showMessage(
                    QStringLiteral("MPD track layout was not loaded (%1); the saved value was "
                                   "preserved")
                        .arg(layout_error),
                    7'000);
            }
        }
        restoreLists(std::move(workspace.lists));
        autoConnectMpd();
        startMetadataOperationRecovery();
    });
}

void BenchMainWindow::restoreLists(std::vector<persistence::ListDocument> documents) {
    lists_restored_ = true;
    for (auto& document : documents) {
        addListTab(std::move(document), false);
    }
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    } else {
        tabs_->setCurrentWidget(list_tabs_.front()->view);
    }
    if (!pending_open_paths_.empty()) {
        auto pending = std::exchange(pending_open_paths_, std::vector<std::string>{});
        openLocalPaths(std::move(pending));
    }
}

void BenchMainWindow::schedulePersist() {
    if (persistence_timer_ != nullptr) {
        persistence_timer_->start();
    }
}

std::vector<persistence::ListDocument> BenchMainWindow::collectDocuments() {
    std::vector<persistence::ListDocument> documents;
    documents.reserve(static_cast<std::size_t>(tabs_->count()));
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view == nullptr) {
            continue;
        }
        const auto id = view->property("bench-document-id").toString();
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            continue;
        }
        auto document = tab->document;
        document.items.clear();
        document.items.reserve(tab->model->rows().size());
        for (const auto& row : tab->model->rows()) {
            persistence::ListItem item{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = row.raw_path,
                .logical_reference = row.logical_reference,
                .segment = row.segment ? std::optional{persistence::ListItemSegment{
                                             .start_sample = row.segment->start_sample,
                                             .end_sample = row.segment->end_sample,
                                         }}
                                       : std::nullopt,
                .source_selection = row.selection.stream_index || row.selection.subsong_index
                                        ? std::optional{persistence::ListItemSourceSelection{
                                              .audio_stream_index = row.selection.stream_index,
                                              .subsong_index = row.selection.subsong_index,
                                          }}
                                        : std::nullopt,
                .duration_ms = row.duration_ms,
                .source_revision = row.source_revision,
                .fields = {},
            };
            if (!row.metadata.fields.empty()) {
                // This remains a presentation cache, but retaining layers is
                // necessary so a verified embedded refresh cannot erase CUE,
                // chapter, or sidecar projections for the same physical file.
                for (const auto& field : row.metadata.fields) {
                    if (field.canonical_name.empty()) {
                        continue;
                    }
                    for (const auto& value : field.values) {
                        item.fields.push_back({
                            .name = field.canonical_name,
                            .value = value,
                            .native_name = field.native_name,
                            .provenance = field.provenance,
                            .language = field.qualifier.language,
                            .description = field.qualifier.description,
                        });
                    }
                }
            } else {
                if (!row.title.empty()) {
                    item.fields.push_back({.name = "title", .value = row.title});
                }
                if (!row.artist.empty()) {
                    item.fields.push_back({.name = "artist", .value = row.artist});
                }
                if (!row.album.empty()) {
                    item.fields.push_back({.name = "album", .value = row.album});
                }
                if (!row.album_artist.empty()) {
                    item.fields.push_back({.name = "albumartist", .value = row.album_artist});
                }
                if (!row.date.empty()) {
                    item.fields.push_back({.name = "date", .value = row.date});
                }
                if (!row.track_number.empty()) {
                    item.fields.push_back({.name = "track", .value = row.track_number});
                }
            }
            document.items.push_back(std::move(item));
        }
        documents.push_back(std::move(document));
    }
    return documents;
}

std::vector<persistence::TrackViewPreset> BenchMainWindow::collectTrackViewLayouts() {
    std::vector<persistence::TrackViewPreset> layouts;
    layouts.reserve(list_tabs_.size() + 1U);
    const auto mpd_bytes = mpd_view_layout_persistence_protected_
                               ? preserved_mpd_view_layout_
                               : ui::serializeTrackViewLayout(
                                     captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_));
    layouts.push_back(persistence::TrackViewPreset{
        .binding = "mpd:queue",
        .header_state =
            std::string{mpd_bytes.constData(), static_cast<std::size_t>(mpd_bytes.size())},
    });
    for (const auto& tab : list_tabs_) {
        const auto id = QString::fromStdString(tab->document.id.to_string());
        const auto bytes = tab->view_layout_persistence_protected
                               ? tab->preserved_view_layout
                               : ui::serializeTrackViewLayout(captureTrackViewLayout(*tab));
        layouts.push_back(persistence::TrackViewPreset{
            .binding = utf8Bytes(QStringLiteral("local:%1").arg(id)),
            .header_state = std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())},
        });
    }
    return layouts;
}

void BenchMainWindow::persistNow(const bool wait) {
    if (persistence_ == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    auto view_layouts = collectTrackViewLayouts();
    if (wait) {
        const auto error =
            persistence_->saveWorkspaceAndWait(std::move(documents), std::move(view_layouts));
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
        return;
    }
    persistence_->saveWorkspace(
        std::move(documents), std::move(view_layouts), [this](QString error) {
            if (!error.isEmpty()) {
                statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
            }
        });
}

BenchMainWindow::ListTab* BenchMainWindow::addListTab(persistence::ListDocument document,
                                                      const bool select) {
    const auto id = QString::fromStdString(document.id.to_string());
    auto* model = new LocalListModel(tabs_);
    std::vector<LocalTrackRow> restored_rows;
    restored_rows.reserve(document.items.size());
    for (const auto& item : document.items) {
        if (item.source != persistence::ListSource::local) {
            continue;
        }
        LocalTrackRow row;
        row.raw_path = item.source_reference;
        row.logical_reference = item.logical_reference;
        if (item.source_selection) {
            row.selection = formats::AudioSourceSelection{
                .stream_index = item.source_selection->audio_stream_index,
                .subsong_index = item.source_selection->subsong_index,
            };
        }
        if (item.segment) {
            row.segment = formats::SampleRange{.start_sample = item.segment->start_sample,
                                               .end_sample = item.segment->end_sample};
        }
        row.duration_ms = item.duration_ms;
        row.source_revision = item.source_revision;
        for (const auto& field : item.fields) {
            const auto canonical_name =
                field.name.empty()
                    ? metadata::resolve_text_property_identity(field.native_name).canonical_name
                    : field.name;
            if (!canonical_name.empty()) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = canonical_name,
                    .native_name = field.native_name.empty() ? field.name : field.native_name,
                    .values = {field.value},
                    .qualifier =
                        metadata::FieldQualifier{
                            .language = field.language,
                            .description = field.description,
                        },
                    .provenance = field.provenance,
                });
            }
        }
        remove_shadowed_probed_metadata(row.metadata);
        project_display_metadata(row);
        row.probed = row.selection.stream_index.has_value() ||
                     row.selection.subsong_index.has_value() || row.segment.has_value() ||
                     row.duration_ms.has_value() || !item.fields.empty();
        restored_rows.push_back(std::move(row));
    }
    model->replaceRows(std::move(restored_rows));

    auto* view = new ui::QueueTableView(tabs_);
    view->setObjectName(QStringLiteral("bench-list-%1").arg(id.left(8)));
    view->setProperty("bench-document-id", id);
    view->setModel(model);
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { refreshSelectionStatus(); });
    view->setProperty("trackknife-hover-row", -1);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackViewHeaderMenu(view, position); });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    connect(view, &QTableView::doubleClicked, this, [this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    view->setReorderCallback([this, id](const QVariantList& rows, const int insertion_row) {
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            return;
        }
        std::vector<int> row_indexes;
        row_indexes.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& row : rows) {
            row_indexes.push_back(row.toInt());
        }
        tab->model->reorderRows(std::move(row_indexes), insertion_row);
        markTabDirty(*tab);
    });
    view->setExternalDropCallback([this, id](QAbstractItemView* source, const QVariantList& rows,
                                             const int insertion_row, const Qt::DropAction action) {
        auto* table = qobject_cast<QTableView*>(source);
        return table != nullptr &&
               transferRows(table, rows, id, action == Qt::MoveAction, insertion_row);
    });
    view->setLocalUrlDropCallback([this, id](const QList<QUrl>& urls, const int insertion_row) {
        std::vector<std::string> raw_paths;
        raw_paths.reserve(static_cast<std::size_t>(urls.size()));
        for (const auto& url : urls) {
            if (!url.isLocalFile()) {
                continue;
            }
            const auto encoded = QFile::encodeName(url.toLocalFile());
            raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
        if (raw_paths.empty()) {
            return false;
        }
        startDiscovery(std::move(raw_paths), id, insertion_row);
        return true;
    });

    const auto index = tabs_->addTab(view, displayText(document.name));
    auto tab = std::make_unique<ListTab>();
    tab->document = std::move(document);
    tab->model = model;
    tab->view = view;
    auto* raw_tab = tab.get();
    list_tabs_.push_back(std::move(tab));
    view->setProperty("bench-tab-pointer", QVariant::fromValue<void*>(raw_tab));
    auto layout = defaultTrackViewLayout();
    const auto binding = QStringLiteral("local:%1").arg(id);
    if (const auto stored = restored_track_view_layouts_.value(binding); !stored.isEmpty()) {
        QString layout_error;
        if (auto decoded = ui::deserializeTrackViewLayout(stored, trackColumnIds(), &layout_error);
            decoded) {
            layout = std::move(*decoded);
        } else {
            raw_tab->view_layout_persistence_protected = true;
            raw_tab->preserved_view_layout = stored;
            statusBar()->showMessage(
                QStringLiteral("Track layout was not loaded (%1); the saved value was preserved")
                    .arg(layout_error),
                7'000);
        }
    }
    applyTrackViewLayout(*raw_tab, layout);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* moved_tab = tabForDocument(id);
                if (moved_tab == nullptr) {
                    return;
                }
                moved_tab->view_layout = captureTrackViewLayout(*moved_tab);
                moved_tab->view_layout_persistence_protected = false;
                moved_tab->preserved_view_layout.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* resized_tab = tabForDocument(id);
                if (resized_tab == nullptr) {
                    return;
                }
                resized_tab->view_layout = captureTrackViewLayout(*resized_tab);
                resized_tab->view_layout_persistence_protected = false;
                resized_tab->preserved_view_layout.clear();
                schedulePersist();
            });
    refreshTabChrome(*raw_tab);
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    enqueueUnprobedRows(*raw_tab);
    syncArtwork(*raw_tab);
    refreshTabActions();
    refreshSelectionStatus();
    return raw_tab;
}

void BenchMainWindow::enqueueUnprobedRows(ListTab& tab) {
    const auto id = QString::fromStdString(tab.document.id.to_string());
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        if (!rows[static_cast<std::size_t>(row)].probed) {
            probe_queue_.push_back(ProbeJob{
                .document_id = id,
                .raw_path = rows[static_cast<std::size_t>(row)].raw_path,
                .hint_row = row,
            });
        }
    }
    pumpProbeQueue();
}

void BenchMainWindow::pumpProbeQueue() {
    if (probe_running_ || probe_queue_.empty()) {
        return;
    }
    std::vector<ProbeJob> batch;
    batch.reserve(probe_batch_size);
    while (!probe_queue_.empty() && batch.size() < probe_batch_size) {
        batch.push_back(std::move(probe_queue_.front()));
        probe_queue_.pop_front();
    }
    probe_running_ = true;
    connect(&probe_watcher_, &QFutureWatcher<std::vector<ProbeOutcome>>::finished, this,
            &BenchMainWindow::finishProbeBatch, Qt::SingleShotConnection);
    probe_watcher_.setFuture(QtConcurrent::run(
        [jobs = std::move(batch), cancellation = probe_cancellation_.token()]() mutable {
            std::vector<ProbeOutcome> outcomes;
            outcomes.reserve(jobs.size());
            for (auto& job : jobs) {
                if (cancellation.is_cancellation_requested()) {
                    break;
                }
                LocalTrackRow fallback;
                std::vector<LocalTrackRow> rows;
                metadata::MetadataDocument document;
                std::optional<core::LocalSourceRevision> source_revision;
                if (auto read = metadata::read_local_metadata(job.raw_path, cancellation); read) {
                    document = std::move(read->document);
                    source_revision = read->source_revision;
                }
                if (auto probe = formats::probe_local_media(job.raw_path, cancellation); probe) {
                    append_missing_probed_metadata(document, probe->tags);
                    fallback = whole_file_row(*probe, document, source_revision);
                    rows = subsong_rows(*probe, document, source_revision);
                    if (rows.empty()) {
                        rows = chapter_rows(*probe, document, source_revision);
                    }
                } else {
                    fallback.raw_path = job.raw_path;
                    fallback.metadata = std::move(document);
                    fallback.source_revision = source_revision;
                    project_display_metadata(fallback);
                    fallback.probed = true;
                }
                // A failed probe still marks the row probed so it is not
                // retried in a loop; the file-name fallback stays visible.
                if (rows.empty()) {
                    rows.push_back(fallback);
                }
                outcomes.push_back(ProbeOutcome{.job = std::move(job),
                                                .rows = std::move(rows),
                                                .whole_file_fallback = std::move(fallback)});
            }
            return outcomes;
        }));
}

void BenchMainWindow::finishProbeBatch() {
    probe_running_ = false;
    auto outcomes = probe_watcher_.result();
    bool applied = false;
    bool logical_track_limit_hit = false;
    QSet<QString> touched_documents;
    for (auto& outcome : outcomes) {
        auto* tab = tabForDocument(outcome.job.document_id);
        if (tab == nullptr) {
            continue;
        }
        const auto current_count = static_cast<std::size_t>(tab->model->rowCount());
        const auto projected_count =
            current_count == 0U ? 0U : current_count - 1U + outcome.rows.size();
        if (outcome.rows.size() > 1U && current_count > 0U &&
            projected_count > discovery_row_limit) {
            outcome.rows.clear();
            outcome.rows.push_back(std::move(outcome.whole_file_fallback));
            logical_track_limit_hit = true;
        }
        if (tab->model->applyProbeRows(outcome.job.raw_path, outcome.job.hint_row,
                                       std::move(outcome.rows))) {
            applied = true;
            touched_documents.insert(outcome.job.document_id);
        }
    }
    if (applied) {
        schedulePersist();
    }
    if (logical_track_limit_hit) {
        statusBar()->showMessage(QStringLiteral("Logical-track expansion hit the local row limit"),
                                 5'000);
    }
    for (const auto& document_id : touched_documents) {
        if (auto* tab = tabForDocument(document_id); tab != nullptr) {
            syncArtwork(*tab);
        }
    }
    pumpProbeQueue();
}

void BenchMainWindow::syncArtwork(ListTab& tab) {
    const auto& rows = tab.model->rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& track = rows[static_cast<std::size_t>(row)];
        if (track.album.empty() && track.artist.empty() && track.album_artist.empty()) {
            continue;
        }
        const auto key = tab.model->groupKey(row);
        if (const auto cached = artwork_cache_.constFind(key);
            cached != artwork_cache_.constEnd()) {
            if (!cached->isNull() && !tab.model->hasArtwork(key)) {
                tab.model->setArtwork(key, *cached);
            }
            continue;
        }
        if (!artwork_pending_.contains(key)) {
            artwork_pending_.insert(key);
            artwork_queue_.push_back(ArtworkJob{.key = key, .raw_path = track.raw_path});
        }
    }
    pumpArtworkQueue();
}

void BenchMainWindow::pumpArtworkQueue() {
    if (artwork_running_ || artwork_queue_.empty()) {
        return;
    }
    auto job = std::move(artwork_queue_.front());
    artwork_queue_.pop_front();
    artwork_running_ = true;
    connect(&artwork_watcher_, &QFutureWatcher<ArtworkOutcome>::finished, this,
            &BenchMainWindow::finishArtworkLoad, Qt::SingleShotConnection);
    artwork_watcher_.setFuture(QtConcurrent::run(
        [job = std::move(job), cancellation = probe_cancellation_.token()]() mutable {
            ArtworkOutcome outcome{.key = std::move(job.key), .image = {}};
            if (cancellation.is_cancellation_requested()) {
                return outcome;
            }
            if (auto embedded = formats::load_embedded_artwork(job.raw_path, cancellation);
                embedded) {
                outcome.image = decoded_artwork(*embedded);
            }
            if (outcome.image.isNull() && !cancellation.is_cancellation_requested()) {
                outcome.image = decoded_artwork(folder_artwork_bytes(job.raw_path));
            }
            return outcome;
        }));
}

void BenchMainWindow::finishArtworkLoad() {
    artwork_running_ = false;
    auto outcome = artwork_watcher_.result();
    // Failed lookups are cached as null so a missing cover is asked once, not
    // on every metadata refresh.
    artwork_cache_.insert(outcome.key, outcome.image);
    if (!outcome.image.isNull()) {
        for (int index = 0; index < tabs_->count(); ++index) {
            auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
            if (view == nullptr) {
                continue;
            }
            auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
            if (tab != nullptr && !tab->model->hasArtwork(outcome.key)) {
                tab->model->setArtwork(outcome.key, outcome.image);
            }
        }
    }
    pumpArtworkQueue();
}

BenchMainWindow::ListTab* BenchMainWindow::currentListTab() {
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    if (view == nullptr) {
        return nullptr;
    }
    return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
}

bool BenchMainWindow::isMpdContext() const {
    return tabs_ != nullptr && mpd_queue_view_ != nullptr &&
           tabs_->currentWidget() == mpd_queue_view_;
}

void BenchMainWindow::refreshActiveContext() {
    const auto mpd = isMpdContext();
    const auto authority = mpd ? QStringLiteral("mpd") : QStringLiteral("local");
    const auto context_changed = property("trackbench-active-authority").toString() != authority;
    setProperty("trackbench-active-authority", authority);
    if (source_stack_ != nullptr) {
        auto* source =
            mpd ? static_cast<QWidget*>(server_library_view_) : static_cast<QWidget*>(folder_view_);
        if (source != nullptr) {
            source_stack_->setCurrentWidget(source);
        }
    }
    if (source_heading_ != nullptr) {
        source_heading_->setText(mpd ? QStringLiteral("MPD Library") : QStringLiteral("Folders"));
    }
    if (connect_mpd_action_ != nullptr) {
        connect_mpd_action_->setEnabled(!mpd_controller_->busy());
    }
    if (disconnect_mpd_action_ != nullptr) {
        disconnect_mpd_action_->setEnabled(mpd_controller_->connected());
    }
    if (buffer_menu_ != nullptr) {
        buffer_menu_->setEnabled(!mpd);
    }
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(!mpd && !metadata_operation_running_ &&
                                             metadata_operation_snapshot_ != nullptr);
    }
    if (context_changed && device_menu_ != nullptr) {
        rebuildDeviceMenu();
    }
    refreshMpdStatusControls();
    if (seek_ != nullptr) {
        refreshTransport();
    }
}

BenchMainWindow::ListTab* BenchMainWindow::tabForDocument(const QString& document_id) {
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view != nullptr && view->property("bench-document-id").toString() == document_id) {
            return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
        }
    }
    return nullptr;
}

bool BenchMainWindow::transferRows(QTableView* source, const QVariantList& rows,
                                   const QString& target_id, const bool move,
                                   const int insertion_row) {
    auto* target = tabForDocument(target_id);
    if (target == nullptr || source == nullptr) {
        return false;
    }
    auto* source_tab = static_cast<ListTab*>(source->property("bench-tab-pointer").value<void*>());
    if (source_tab == nullptr || source_tab == target) {
        return false;
    }
    std::vector<LocalTrackRow> transferred;
    std::vector<int> source_rows;
    transferred.reserve(static_cast<std::size_t>(rows.size()));
    source_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        const auto row_index = row.toInt();
        if (row_index < 0 || row_index >= static_cast<int>(source_tab->model->rows().size())) {
            continue;
        }
        transferred.push_back(source_tab->model->rows()[static_cast<std::size_t>(row_index)]);
        source_rows.push_back(row_index);
    }
    if (transferred.empty()) {
        return false;
    }
    target->model->appendRows(std::move(transferred), insertion_row);
    markTabDirty(*target);
    syncArtwork(*target);
    if (move) {
        source_tab->model->removeRowIndexes(std::move(source_rows));
        markTabDirty(*source_tab);
    }
    return true;
}

void BenchMainWindow::markTabDirty(ListTab& tab) {
    tab.document.dirty = true;
    refreshTabChrome(tab);
    schedulePersist();
}

void BenchMainWindow::refreshTabChrome(ListTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    const auto name = displayText(tab.document.name);
    tabs_->setTabText(index, name + (tab.document.dirty ? QStringLiteral(" *") : QString{}));
    const auto kind = tab.document.kind == persistence::ListKind::scratch
                          ? QStringLiteral("Persistent scratch list")
                          : QStringLiteral("Named Trackbench working list");
    tabs_->setTabToolTip(index,
                         QStringLiteral("%1%2%3").arg(
                             kind, tab.document.pinned ? QStringLiteral(" · pinned") : QString{},
                             tab.document.dirty ? QStringLiteral(" · modified") : QString{}));
    tab.view->setAccessibleName(QStringLiteral("%1 track list").arg(name));
    if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!tab.document.pinned);
    }
}

void BenchMainWindow::refreshTabActions() {
    const auto* tab = currentListTab();
    const bool available = tab != nullptr;
    for (auto* action :
         {duplicate_tab_action_, pin_tab_action_, save_tab_action_, rename_tab_action_}) {
        if (action != nullptr) {
            action->setEnabled(available);
        }
    }
    if (pin_tab_action_ != nullptr) {
        const QSignalBlocker blocker{pin_tab_action_};
        pin_tab_action_->setChecked(available && tab->document.pinned);
    }
    if (close_tab_action_ != nullptr) {
        const auto properties_tab =
            qobject_cast<MetadataPropertiesDialog*>(tabs_->currentWidget()) != nullptr;
        close_tab_action_->setEnabled(properties_tab || (available && !tab->document.pinned));
    }
}

void BenchMainWindow::closeTabAt(const int index) {
    if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
        properties->close();
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
    if (view == nullptr) {
        return;
    }
    if (view == mpd_queue_view_) {
        statusBar()->showMessage(QStringLiteral("MPD Queue is a permanent authority tab"), 3'000);
        return;
    }
    auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (tab == nullptr || tab->document.pinned) {
        statusBar()->showMessage(QStringLiteral("Unpin this list before closing it"), 3'000);
        return;
    }
    if (tab->document.dirty) {
        QMessageBox confirmation{
            QMessageBox::Question,
            QStringLiteral("Close unsaved list"),
            QStringLiteral("Discard the unsaved contents of “%1”?")
                .arg(displayText(tab->document.name)),
            QMessageBox::Yes | QMessageBox::No,
            this,
        };
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    tabs_->removeTab(index);
    view->deleteLater();
    std::erase_if(list_tabs_,
                  [tab](const std::unique_ptr<ListTab>& owned) { return owned.get() == tab; });
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    }
    schedulePersist();
    refreshTabActions();
}

void BenchMainWindow::closeCurrentTab() { closeTabAt(tabs_->currentIndex()); }

void BenchMainWindow::createList() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString{}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    addListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = utf8Bytes(name),
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    schedulePersist();
}

void BenchMainWindow::duplicateCurrentTab() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    const auto found =
        std::ranges::find(documents, tab->document.id, &persistence::ListDocument::id);
    if (found == documents.end()) {
        return;
    }
    auto duplicate = *found;
    duplicate.id = core::StableId::random();
    duplicate.name = utf8Bytes(QStringLiteral("%1 copy").arg(displayText(found->name)));
    duplicate.pinned = false;
    duplicate.dirty = true;
    auto* duplicated_tab = addListTab(std::move(duplicate), true);
    if (duplicated_tab != nullptr) {
        applyTrackViewLayout(*duplicated_tab, captureTrackViewLayout(*tab));
    }
    schedulePersist();
}

void BenchMainWindow::toggleCurrentTabPinned() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    tab->document.pinned = !tab->document.pinned;
    refreshTabChrome(*tab);
    refreshTabActions();
    schedulePersist();
}

void BenchMainWindow::saveCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    if (tab->document.kind == persistence::ListKind::scratch) {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Save working list"),
                                                QStringLiteral("Name:"), QLineEdit::Normal,
                                                displayText(tab->document.name), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        tab->document.name = utf8Bytes(name);
        tab->document.kind = persistence::ListKind::saved;
    }
    tab->document.dirty = false;
    refreshTabChrome(*tab);
    schedulePersist();
}

void BenchMainWindow::renameCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    const auto current_name = displayText(tab->document.name);
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, current_name, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name == current_name) {
        return;
    }
    tab->document.name = utf8Bytes(name);
    markTabDirty(*tab);
}

void BenchMainWindow::showTabContextMenu(const QPoint& position) {
    const auto index = tabs_->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }
    tabs_->setCurrentIndex(index);
    refreshTabActions();
    tab_context_menu_->popup(tabs_->tabBar()->mapToGlobal(position));
}


void BenchMainWindow::showTrackContextMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr || view->selectionModel() == nullptr || track_context_menu_ == nullptr) {
        return;
    }
    const auto target = view->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    tabs_->setCurrentWidget(view);

    const auto* grouped_delegate = qobject_cast<const ui::QueueItemDelegate*>(view->itemDelegate());
    const auto relative_y = position.y() - view->visualRect(target).top();
    if (grouped_delegate != nullptr && grouped_delegate->isAlbumHeaderHit(target, relative_y)) {
        const auto [first, last] = grouped_delegate->albumRowRange(target);
        const QItemSelection album{view->model()->index(first, 0),
                                   view->model()->index(last, view->model()->columnCount() - 1)};
        view->selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                  QItemSelectionModel::Rows);
        view->selectionModel()->setCurrentIndex(view->model()->index(first, local_title_column),
                                                QItemSelectionModel::NoUpdate);
    } else {
        if (!view->selectionModel()->isRowSelected(target.row(), target.parent())) {
            view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        }
        view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();

    const auto mpd_queue = view == mpd_queue_view_;
    const auto has_selection = !view->selectionModel()->selectedRows().isEmpty();
    const auto command_ready =
        !mpd_queue || (mpd_controller_->connected() && !mpd_controller_->commandBusy());
    track_context_menu_->clear();
    play_selected_action_->setEnabled(view->currentIndex().isValid() && command_ready);
    remove_selected_action_->setEnabled(has_selection && command_ready);
    track_context_menu_->addAction(play_selected_action_);
    if (mpd_queue) {
        const auto has_uris = !selectedMpdQueueUris().isEmpty();
        mpd_add_next_selection_action_->setEnabled(command_ready && has_uris);
        mpd_append_selection_action_->setEnabled(command_ready && has_uris);
        mpd_crop_selection_action_->setEnabled(command_ready && has_selection);
        track_context_menu_->addAction(mpd_add_next_selection_action_);
        track_context_menu_->addAction(mpd_append_selection_action_);
        track_context_menu_->addSeparator();
        track_context_menu_->addAction(remove_selected_action_);
        track_context_menu_->addAction(mpd_crop_selection_action_);
        refreshMpdPriorityMenu();
        track_context_menu_->addMenu(mpd_priority_menu_);
        track_context_menu_->popup(view->viewport()->mapToGlobal(position));
        return;
    }

    track_context_menu_->addAction(properties_action_);

    auto* source_tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (source_tab != nullptr && list_tabs_.size() > 1U) {
        auto* copy_menu = track_context_menu_->addMenu(QStringLiteral("Copy to list"));
        copy_menu->setObjectName(QStringLiteral("bench-track-copy-menu"));
        auto* move_menu = track_context_menu_->addMenu(QStringLiteral("Move to list"));
        move_menu->setObjectName(QStringLiteral("bench-track-move-menu"));
        for (const auto& destination : list_tabs_) {
            if (destination.get() == source_tab) {
                continue;
            }
            const auto target_id = QString::fromStdString(destination->document.id.to_string());
            const auto label = displayText(destination->document.name);
            auto* copy = copy_menu->addAction(label);
            connect(copy, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, false); });
            auto* move = move_menu->addAction(label);
            connect(move, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, true); });
        }
    }
    track_context_menu_->addSeparator();
    track_context_menu_->addAction(remove_selected_action_);
    track_context_menu_->popup(view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::showFolderContextMenu(const QPoint& position) {
    if (folder_context_menu_ == nullptr) {
        return;
    }
    const auto target = folder_view_->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    folder_view_->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
    const auto directory = folder_model_->isDirectory(target);
    folder_add_to_list_action_->setText(directory ? QStringLiteral("Add folder to current list")
                                                  : QStringLiteral("Add file to current list"));
    folder_toggle_expanded_action_->setText(
        folder_view_->isExpanded(target) ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    folder_toggle_expanded_action_->setEnabled(directory);
    folder_context_menu_->clear();
    folder_context_menu_->addAction(folder_add_to_list_action_);
    if (directory) {
        folder_context_menu_->addAction(folder_toggle_expanded_action_);
    }
    folder_context_menu_->popup(folder_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::playCurrentRow() {
    if (isMpdContext()) {
        if (mpd_queue_view_->currentIndex().isValid()) {
            mpd_controller_->playQueueItem(mpd_queue_view_->currentIndex().row());
        }
        return;
    }
    auto* tab = currentListTab();
    if (tab != nullptr && tab->view->currentIndex().isValid()) {
        playRow(*tab, tab->view->currentIndex().row());
    }
}


void BenchMainWindow::removeSelectedRows() {
    if (isMpdContext()) {
        if (mpd_queue_view_->selectionModel() == nullptr) {
            return;
        }
        QVariantList rows;
        for (const auto& index : mpd_queue_view_->selectionModel()->selectedRows()) {
            rows.push_back(index.row());
        }
        if (!rows.isEmpty()) {
            mpd_controller_->removeQueueItems(rows);
        }
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        return;
    }
    std::vector<int> rows;
    const auto selection = tab->view->selectionModel()->selectedRows();
    rows.reserve(static_cast<std::size_t>(selection.size()));
    for (const auto& index : selection) {
        rows.push_back(index.row());
    }
    if (rows.empty()) {
        return;
    }
    tab->model->removeRowIndexes(std::move(rows));
    markTabDirty(*tab);
}

void BenchMainWindow::transferSelectedRows(QTableView* source, const QString& target_id,
                                           const bool move) {
    if (source == nullptr || source->selectionModel() == nullptr) {
        return;
    }
    auto selected = source->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    if (!rows.isEmpty()) {
        static_cast<void>(transferRows(source, rows, target_id, move, -1));
    }
}

void BenchMainWindow::openFilesDialog() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("Open files"));
    std::vector<std::string> raw_paths;
    raw_paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& file : files) {
        const auto encoded = QFile::encodeName(file);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    openLocalPaths(std::move(raw_paths));
}

void BenchMainWindow::openFolderDialog() {
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Open folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    openLocalPaths({{encoded.constData(), static_cast<std::size_t>(encoded.size())}});
}

void BenchMainWindow::addFolderRoot() {
    const auto folder =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Add library folder"));
    if (folder.isEmpty()) {
        return;
    }
    const auto encoded = QFile::encodeName(folder);
    folder_model_->addRoot({encoded.constData(), static_cast<std::size_t>(encoded.size())});
    QSettings settings;
    auto roots = settings.value(QStringLiteral("library/roots")).toList();
    roots.push_back(QByteArray{encoded.constData(), encoded.size()});
    settings.setValue(QStringLiteral("library/roots"), roots);
}

void BenchMainWindow::openLocalPaths(std::vector<std::string> raw_paths) {
    if (raw_paths.empty()) {
        return;
    }
    if (!lists_restored_) {
        pending_open_paths_.insert(pending_open_paths_.end(),
                                   std::make_move_iterator(raw_paths.begin()),
                                   std::make_move_iterator(raw_paths.end()));
        return;
    }
    auto* tab = currentListTab();
    if (tab == nullptr && !list_tabs_.empty()) {
        tab = list_tabs_.front().get();
        tabs_->setCurrentWidget(tab->view);
    }
    if (tab == nullptr) {
        return;
    }
    startDiscovery(std::move(raw_paths), QString::fromStdString(tab->document.id.to_string()), -1);
}

void BenchMainWindow::startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                                     const int insertion_row) {
    if (discovery_running_) {
        statusBar()->showMessage(QStringLiteral("A folder scan is already running"), 3'000);
        return;
    }
    discovery_running_ = true;
    discovery_target_document_ = std::move(target_document_id);
    discovery_insertion_row_ = insertion_row;
    connect(&discovery_watcher_, &QFutureWatcher<DiscoveryOutcome>::finished, this,
            &BenchMainWindow::finishDiscovery, Qt::SingleShotConnection);
    discovery_watcher_.setFuture(QtConcurrent::run([paths = std::move(raw_paths),
                                                    cancellation = probe_cancellation_.token()] {
        auto discovered =
            core::discover_local_sources(std::span{paths.data(), paths.size()}, cancellation,
                                         discovery_row_limit, std::span{audio_extensions});
        DiscoveryOutcome outcome{.rows = {},
                                 .issues = std::move(discovered.issues),
                                 .cancelled = discovered.cancelled,
                                 .truncated = discovered.truncated};
        std::vector<std::optional<formats::ResolvedCueSheet>> resolved_cues(
            discovered.raw_files.size());
        std::unordered_set<std::string> cue_sources;
        std::unordered_map<std::string, metadata::LocalMetadataRead> cue_metadata;

        // Resolve cues first so an audio file referenced by a successfully
        // expanded sheet is not also inserted as one duplicate whole-file
        // row when both came from the same intake batch.
        for (std::size_t index = 0U; index < discovered.raw_files.size(); ++index) {
            if (cancellation.is_cancellation_requested()) {
                outcome.cancelled = true;
                break;
            }
            const auto& raw_path = discovered.raw_files[index];
            if (!is_cue_path(raw_path)) {
                continue;
            }
            auto resolved = formats::resolve_external_cue_sheet(raw_path, cancellation);
            if (!resolved) {
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path, .error = std::move(resolved.error())});
                continue;
            }
            for (const auto& physical : resolved->physical_sources) {
                cue_sources.insert(physical);
                if (!cue_metadata.contains(physical)) {
                    if (auto read = metadata::read_local_metadata(physical, cancellation); read) {
                        cue_metadata.emplace(physical, std::move(*read));
                    }
                }
            }
            resolved_cues[index] = std::move(*resolved);
        }

        bool row_limit_reached = false;
        for (std::size_t index = 0U;
             index < discovered.raw_files.size() && !outcome.cancelled && !row_limit_reached;
             ++index) {
            const auto& raw_path = discovered.raw_files[index];
            if (is_cue_path(raw_path)) {
                if (!resolved_cues[index]) {
                    continue;
                }
                for (const auto& resolved_track : resolved_cues[index]->tracks) {
                    if (outcome.rows.size() == discovery_row_limit) {
                        outcome.truncated = true;
                        row_limit_reached = true;
                        outcome.issues.push_back(core::LocalSourceIssue{
                            .raw_path = raw_path,
                            .error =
                                core::Error{
                                    .code = core::ErrorCode::limit_exceeded,
                                    .message = "CUE expansion reached the local row limit",
                                    .context = {},
                                },
                        });
                        break;
                    }
                    const auto embedded = cue_metadata.find(resolved_track.raw_source_path);
                    if (embedded == cue_metadata.end()) {
                        outcome.rows.push_back(
                            cue_row(*resolved_cues[index], resolved_track, {}, std::nullopt));
                    } else {
                        outcome.rows.push_back(cue_row(*resolved_cues[index], resolved_track,
                                                       embedded->second.document,
                                                       embedded->second.source_revision));
                    }
                }
                continue;
            }

            std::error_code canonical_error;
            const auto canonical =
                std::filesystem::canonical(std::filesystem::path{raw_path}, canonical_error);
            if (!canonical_error && cue_sources.contains(canonical.native())) {
                continue;
            }
            if (outcome.rows.size() == discovery_row_limit) {
                outcome.truncated = true;
                row_limit_reached = true;
                outcome.issues.push_back(core::LocalSourceIssue{
                    .raw_path = raw_path,
                    .error =
                        core::Error{
                            .code = core::ErrorCode::limit_exceeded,
                            .message = "Local intake reached the row limit",
                            .context = {},
                        },
                });
                continue;
            }
            LocalTrackRow row;
            row.raw_path = raw_path;
            outcome.rows.push_back(std::move(row));
        }
        return outcome;
    }));
}

void BenchMainWindow::finishDiscovery() {
    discovery_running_ = false;
    auto result = discovery_watcher_.result();
    auto* tab = tabForDocument(discovery_target_document_);
    if (tab == nullptr) {
        tab = currentListTab();
    }
    if (tab == nullptr) {
        return;
    }
    if (!result.rows.empty()) {
        tab->model->appendRows(std::move(result.rows), discovery_insertion_row_);
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
    }
    if (!result.issues.empty()) {
        statusBar()->showMessage(
            QStringLiteral("%1 entr%2 could not be opened")
                .arg(result.issues.size())
                .arg(result.issues.size() == 1U ? QStringLiteral("y") : QStringLiteral("ies")),
            5'000);
    }
    if (result.truncated) {
        statusBar()->showMessage(QStringLiteral("Folder scan hit the file limit"), 5'000);
    }
}


void BenchMainWindow::closeEvent(QCloseEvent* event) {
    std::vector<QPointer<MetadataPropertiesDialog>> properties_tabs;
    for (auto index = 0; index < tabs_->count(); ++index) {
        if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
            properties_tabs.emplace_back(properties);
        }
    }
    for (const auto& properties : properties_tabs) {
        if (properties != nullptr && !properties->close()) {
            event->ignore();
            return;
        }
    }
    probe_cancellation_.request_cancellation();
    metadata_operation_cancellation_.request_cancellation();
    probe_queue_.clear();
    artwork_queue_.clear();
    if (probe_running_) {
        probe_watcher_.waitForFinished();
    }
    if (artwork_running_) {
        artwork_watcher_.waitForFinished();
    }
    if (discovery_running_) {
        discovery_watcher_.waitForFinished();
    }
    if (metadata_operation_running_) {
        metadata_operation_watcher_.waitForFinished();
        metadata_operation_running_ = false;
    }
    persistNow(true);
    if (transport_timer_ != nullptr) {
        transport_timer_->stop();
    }
    player_ = nullptr;
    player_storage_.reset();
    event->accept();
}

void BenchMainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    resizeMpdSearchField();
    if (mpd_search_surface_ != nullptr && mpd_search_surface_->isVisible()) {
        positionMpdSearchSurface();
    }
}

bool BenchMainWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == tabs_ || (tabs_ != nullptr && watched == tabs_->tabBar())) &&
        event->type() == QEvent::Resize) {
        resizeMpdSearchField();
        QMetaObject::invokeMethod(this, [this] { resizeMpdSearchField(); }, Qt::QueuedConnection);
        if (mpd_search_surface_ != nullptr && mpd_search_surface_->isVisible()) {
            positionMpdSearchSurface();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void BenchMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void BenchMainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::string> raw_paths;
    const auto urls = event->mimeData()->urls();
    raw_paths.reserve(static_cast<std::size_t>(urls.size()));
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const auto encoded = QFile::encodeName(url.toLocalFile());
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (raw_paths.empty()) {
        return;
    }
    event->acceptProposedAction();
    openLocalPaths(std::move(raw_paths));
}

} // namespace trackknife::bench
