// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/cue_sheet.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "uicommon/local_folder_tree_model.hpp"

#include <QFileDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(TRACKKNIFE_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
extern "C" void __tsan_release(void* address);
#endif

namespace trackknife::bench {
namespace {

constexpr std::size_t probe_batch_size = 8U;
constexpr std::size_t discovery_row_limit = 100'000U;

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
    connect(&artwork_watcher_, &QFutureWatcher<void>::finished, this,
            &BenchMainWindow::finishArtworkLoad, Qt::SingleShotConnection);
    artwork_outcome_ =
        std::make_shared<ArtworkOutcome>(ArtworkOutcome{.key = std::move(job.key), .image = {}});
    artwork_watcher_.setFuture(QtConcurrent::run([raw_path = std::move(job.raw_path),
                                                  outcome = artwork_outcome_,
                                                  cancellation = probe_cancellation_.token()] {
        if (!cancellation.is_cancellation_requested()) {
            if (auto embedded = formats::load_embedded_artwork(raw_path, cancellation); embedded) {
                outcome->image = decoded_artwork(*embedded);
            }
            if (outcome->image.isNull() && !cancellation.is_cancellation_requested()) {
                outcome->image = decoded_artwork(folder_artwork_bytes(raw_path));
            }
        }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
        __tsan_release(outcome.get());
#endif
    }));
}

void BenchMainWindow::finishArtworkLoad() {
    artwork_running_ = false;
    auto outcome = artwork_outcome_;
    if (!outcome) {
        pumpArtworkQueue();
        return;
    }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    __tsan_acquire(outcome.get());
#endif
    // Failed lookups are cached as null so a missing cover is asked once, not
    // on every metadata refresh.
    artwork_cache_.insert(outcome->key, outcome->image);
    if (!outcome->image.isNull()) {
        for (int index = 0; index < tabs_->count(); ++index) {
            auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
            if (view == nullptr) {
                continue;
            }
            auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
            if (tab != nullptr && !tab->model->hasArtwork(outcome->key)) {
                tab->model->setArtwork(outcome->key, outcome->image);
            }
        }
    }
    pumpArtworkQueue();
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

} // namespace trackknife::bench
