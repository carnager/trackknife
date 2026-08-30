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

constexpr int transport_refresh_ms = 33;
constexpr int persist_debounce_ms = 1'000;
constexpr int minimum_custom_buffer_ms = 10;
constexpr int maximum_custom_buffer_ms = 10'000;
constexpr auto buffer_profile_settings_key = "playback/buffer-profile";
constexpr auto buffer_capacity_settings_key = "playback/buffer-capacity-ms";
constexpr auto buffer_threshold_settings_key = "playback/buffer-start-threshold-ms";


enum class MpdSearchQueueAction : std::uint8_t { append, next, replace };

class MpdSearchLineEdit final : public QLineEdit {
  public:
    explicit MpdSearchLineEdit(QWidget* parent) : QLineEdit(parent) {}

    void setResultFocusCallback(std::function<void()> callback) {
        result_focus_callback_ = std::move(callback);
    }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(MpdSearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ControlModifier && action_callback_) {
            action_callback_(MpdSearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier && action_callback_) {
            action_callback_(MpdSearchQueueAction::append);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Down && result_focus_callback_) {
            result_focus_callback_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && close_callback_) {
            close_callback_();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

  private:
    std::function<void()> result_focus_callback_;
    std::function<void()> close_callback_;
    std::function<void(MpdSearchQueueAction)> action_callback_;
};

class MpdSearchTableView final : public QTableView {
  public:
    explicit MpdSearchTableView(QWidget* parent) : QTableView(parent) {}

    void setSearchField(QLineEdit* field) { search_field_ = field; }
    void setCloseCallback(std::function<void()> callback) { close_callback_ = std::move(callback); }
    void setActionCallback(std::function<void(int, MpdSearchQueueAction)> callback) {
        action_callback_ = std::move(callback);
    }

    void focusFirstResult() {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        const auto row = results != nullptr ? results->firstResultRow() : -1;
        if (row < 0) {
            return;
        }
        selectionModel()->setCurrentIndex(model()->index(row, 1),
                                          QItemSelectionModel::ClearAndSelect |
                                              QItemSelectionModel::Rows);
        setFocus();
        scrollTo(currentIndex());
    }

    void activateDefault(const MpdSearchQueueAction action) {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        auto row = currentIndex().row();
        if (results == nullptr || !currentIndex().isValid() ||
            results->kindAt(row) == quick::MpdSearchResultModel::ResultKind::section) {
            row = results != nullptr ? results->firstResultRow() : -1;
        }
        if (row >= 0 && action_callback_) {
            action_callback_(row, action);
        }
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            if (close_callback_) {
                close_callback_();
            }
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::ControlModifier) {
            activateDefault(MpdSearchQueueAction::replace);
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            activateDefault(actionForColumn(currentIndex().column()));
            event->accept();
            return;
        }
        if (event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_Backspace || isSearchText(event->text()))) {
            forwardToSearch(event);
            return;
        }
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            moveVertically(event->key() == Qt::Key_Down ? 1 : -1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right && currentIndex().isValid()) {
            const auto current_action =
                currentIndex().column() < quick::MpdSearchResultModel::first_action_column
                    ? quick::MpdSearchResultModel::first_action_column
                    : currentIndex().column();
            const auto next_action =
                std::min(current_action + 1, quick::MpdSearchResultModel::column_count - 1);
            setCurrentIndex(currentIndex().siblingAtColumn(next_action));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left && currentIndex().isValid()) {
            const auto current_action =
                std::max(currentIndex().column(), quick::MpdSearchResultModel::first_action_column);
            const auto previous_action =
                std::max(current_action - 1, quick::MpdSearchResultModel::first_action_column);
            setCurrentIndex(currentIndex().siblingAtColumn(previous_action));
            event->accept();
            return;
        }
        QTableView::keyPressEvent(event);
    }

  private:
    [[nodiscard]] static bool isSearchText(const QString& text) {
        return !text.isEmpty() &&
               std::ranges::all_of(text, [](const QChar character) { return character.isPrint(); });
    }

    [[nodiscard]] static MpdSearchQueueAction actionForColumn(const int column) {
        if (column == quick::MpdSearchResultModel::first_action_column + 1) {
            return MpdSearchQueueAction::next;
        }
        if (column == quick::MpdSearchResultModel::first_action_column + 2) {
            return MpdSearchQueueAction::replace;
        }
        return MpdSearchQueueAction::append;
    }

    void forwardToSearch(QKeyEvent* event) {
        if (search_field_ == nullptr) {
            return;
        }
        const auto cursor = static_cast<int>(
            std::min<qsizetype>(search_field_->text().size(), std::numeric_limits<int>::max()));
        const auto repeat_count = static_cast<quint16>(
            std::clamp(event->count(), 0, static_cast<int>(std::numeric_limits<quint16>::max())));
        if (!search_field_->hasSelectedText()) {
            search_field_->setCursorPosition(cursor);
        }
        search_field_->setFocus();
        QKeyEvent forwarded{event->type(), event->key(),          event->modifiers(),
                            event->text(), event->isAutoRepeat(), repeat_count};
        QApplication::sendEvent(search_field_, &forwarded);
        event->accept();
    }

    void moveVertically(const int direction) {
        const auto* results = qobject_cast<const quick::MpdSearchResultModel*>(model());
        if (results == nullptr) {
            return;
        }
        const auto row = results->nextResultRow(currentIndex().row(), direction);
        if (row >= 0) {
            const auto column = currentIndex().column() >= 0 ? currentIndex().column() : 1;
            setCurrentIndex(model()->index(row, column));
            scrollTo(currentIndex());
        } else if (direction < 0 && search_field_ != nullptr) {
            search_field_->setFocus();
        }
    }

    QLineEdit* search_field_{nullptr};
    std::function<void()> close_callback_;
    std::function<void(int, MpdSearchQueueAction)> action_callback_;
};

class MpdSearchAlbumDelegate final : public QStyledItemDelegate {
  public:
    explicit MpdSearchAlbumDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto kind = static_cast<quick::MpdSearchResultModel::ResultKind>(
            index.data(quick::MpdSearchResultModel::ResultKindRole).toInt());
        if (kind != quick::MpdSearchResultModel::ResultKind::album) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        auto item = option;
        initStyleOption(&item, index);
        const auto text = item.text;
        item.text.clear();
        item.icon = {};
        item.features &= ~QStyleOptionViewItem::HasDecoration;
        const auto* widget = item.widget;
        auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        constexpr int cover_extent = 24;
        constexpr int horizontal_padding = 3;
        const auto cover_rect =
            QRect{item.rect.left() + horizontal_padding, item.rect.center().y() - cover_extent / 2,
                  cover_extent, cover_extent};
        const auto decoration = index.data(Qt::DecorationRole);
        if (decoration.canConvert<QImage>()) {
            const auto image = decoration.value<QImage>();
            if (!image.isNull()) {
                const auto scaled =
                    image.scaled(cover_rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter->drawImage(
                    QStyle::alignedRect(item.direction, Qt::AlignCenter, scaled.size(), cover_rect),
                    scaled);
            }
        } else if (decoration.canConvert<QIcon>()) {
            decoration.value<QIcon>().paint(painter, cover_rect, Qt::AlignCenter);
        }

        const auto text_rect =
            item.rect.adjusted(cover_extent + horizontal_padding * 2, 0, -horizontal_padding, 0);
        const auto foreground = item.palette.color(item.state.testFlag(QStyle::State_Selected)
                                                       ? QPalette::HighlightedText
                                                       : QPalette::Text);
        painter->save();
        painter->setFont(item.font);
        painter->setPen(foreground);
        painter->drawText(
            text_rect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics{item.font}.elidedText(text, Qt::ElideRight, text_rect.width()));
        painter->restore();
    }
};

class MpdSearchActionDelegate final : public QStyledItemDelegate {
  public:
    MpdSearchActionDelegate(QIcon icon, QObject* parent)
        : QStyledItemDelegate(parent), icon_(std::move(icon)),
          view_(qobject_cast<QTableView*>(parent)) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto item = option;
        initStyleOption(&item, index);
        item.text.clear();
        item.icon = {};
        item.features &= ~QStyleOptionViewItem::HasDecoration;
        item.state &= ~QStyle::State_HasFocus;
        const auto* widget = item.widget;
        auto* item_style = widget != nullptr ? widget->style() : QApplication::style();
        item_style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        const auto icon_rect =
            QStyle::alignedRect(item.direction, Qt::AlignCenter, QSize{12, 12}, item.rect);
        auto active = false;
        if (view_ != nullptr && view_->hasFocus()) {
            const auto current = view_->currentIndex();
            const auto active_column =
                current.column() >= quick::MpdSearchResultModel::first_action_column
                    ? current.column()
                    : quick::MpdSearchResultModel::first_action_column;
            active = current.row() == index.row() && active_column == index.column();
        }
        if (active) {
            const auto focus_rect =
                QStyle::alignedRect(item.direction, Qt::AlignCenter, QSize{18, 18}, item.rect)
                    .adjusted(0, 0, -1, -1);
            auto marker = item.palette.color(item.state.testFlag(QStyle::State_Selected)
                                                 ? QPalette::HighlightedText
                                                 : QPalette::Highlight);
            auto fill = marker;
            fill.setAlpha(36);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setBrush(fill);
            painter->setPen(QPen(marker, 1.0));
            painter->drawRoundedRect(focus_rect, 3.0, 3.0);
            painter->restore();
        }
        const auto mode =
            item.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
        painter->save();
        painter->setOpacity(active ? 1.0 : 0.72);
        icon_.paint(painter, icon_rect, Qt::AlignCenter, mode);
        painter->restore();
    }

  private:
    QIcon icon_;
    QTableView* view_{nullptr};
};


struct PlaybackBufferPreference {
    QString profile;
    audio::PlaybackBufferDurationConfig config;
};

[[nodiscard]] QString bufferProfileLabel(const QString& profile) {
    if (profile == QStringLiteral("responsive")) {
        return QStringLiteral("Responsive");
    }
    if (profile == QStringLiteral("resilient")) {
        return QStringLiteral("Resilient");
    }
    if (profile == QStringLiteral("custom")) {
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Balanced");
}

[[nodiscard]] PlaybackBufferPreference loadPlaybackBufferPreference() {
    QSettings settings;
    const auto profile =
        settings.value(QString::fromLatin1(buffer_profile_settings_key), QStringLiteral("balanced"))
            .toString();
    const auto profile_bytes = utf8Bytes(profile);
    if (const auto preset = audio::playback_buffer_preset_from_id(profile_bytes)) {
        return {.profile = profile, .config = audio::playback_buffer_preset_config(*preset)};
    }
    if (profile == QStringLiteral("custom")) {
        bool capacity_ok = false;
        bool threshold_ok = false;
        const auto capacity =
            settings.value(QString::fromLatin1(buffer_capacity_settings_key)).toInt(&capacity_ok);
        const auto threshold =
            settings.value(QString::fromLatin1(buffer_threshold_settings_key)).toInt(&threshold_ok);
        const audio::PlaybackBufferDurationConfig config{
            .capacity = std::chrono::milliseconds{capacity},
            .start_threshold = std::chrono::milliseconds{threshold},
        };
        if (capacity_ok && threshold_ok && capacity >= minimum_custom_buffer_ms &&
            capacity <= maximum_custom_buffer_ms &&
            audio::valid_local_audition_buffer_config(config)) {
            return {.profile = profile, .config = config};
        }
    }
    return {.profile = QStringLiteral("balanced"),
            .config = audio::playback_buffer_preset_config(audio::PlaybackBufferPreset::balanced)};
}


[[nodiscard]] bool playerActive(const audio::LocalAuditionState state) {
    return state == audio::LocalAuditionState::buffering ||
           state == audio::LocalAuditionState::playing ||
           state == audio::LocalAuditionState::draining;
}

[[nodiscard]] core::Result<void> load_and_play(audio::LocalAuditionService& player,
                                               const LocalTrackSource& source) {
    return source.segment ? player.load_selected_segment_and_play(source.raw_path, source.selection,
                                                                  *source.segment)
                          : player.load_selected_and_play(source.raw_path, source.selection);
}

[[nodiscard]] core::Result<void> queue_gapless(audio::LocalAuditionService& player,
                                               const LocalTrackSource& source) {
    return source.segment ? player.queue_gapless_next_selected_segment(
                                source.raw_path, source.selection, *source.segment)
                          : player.queue_gapless_next_selected(source.raw_path, source.selection);
}

[[nodiscard]] LocalTrackSource source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    return LocalTrackSource{.raw_path = snapshot.raw_path,
                            .selection = snapshot.selection,
                            .segment = snapshot.segment};
}

[[nodiscard]] std::optional<LocalTrackSource>
queued_source_from_snapshot(const audio::LocalAuditionSnapshot& snapshot) {
    if (snapshot.next_raw_path.empty()) {
        return std::nullopt;
    }
    return LocalTrackSource{.raw_path = snapshot.next_raw_path,
                            .selection = snapshot.next_selection,
                            .segment = snapshot.next_segment};
}

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

BenchMainWindow::BenchMainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Trackbench"));
    resize(1100, 720);
    setAcceptDrops(true);

    const auto buffer_preference = loadPlaybackBufferPreference();
    selected_buffer_profile_ = buffer_preference.profile;
    audio::LocalAuditionConfig player_config;
    player_config.buffer = buffer_preference.config;
    if (auto player = audio::LocalAuditionService::create(std::move(player_config)); player) {
        player_storage_ = std::move(*player);
        player_ = player_storage_.get();
    }

    buildWorkspace();
    buildTransport();
    connect(&metadata_operation_watcher_, &QFutureWatcherBase::finished, this,
            &BenchMainWindow::finishMetadataOperationJob);
    initializePersistence();

    if (player_ != nullptr) {
        static_cast<void>(player_->refresh_output_devices());
    } else {
        statusBar()->showMessage(
            QStringLiteral("Local playback unavailable: the audio worker failed to start"));
    }
    transport_timer_ = new QTimer(this);
    transport_timer_->setInterval(transport_refresh_ms);
    connect(transport_timer_, &QTimer::timeout, this, &BenchMainWindow::refreshTransport);
    transport_timer_->start();
    refreshActiveContext();
    refreshTransport();
}

BenchMainWindow::~BenchMainWindow() {
    metadata_operation_cancellation_.request_cancellation();
    if (metadata_operation_running_) {
        metadata_operation_watcher_.waitForFinished();
    }
}


void BenchMainWindow::buildMpdStatusControls() {
    auto* separator = new QFrame(statusBar());
    mpd_status_separator_ = separator;
    separator->setObjectName(QStringLiteral("bench-mpd-status-separator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(separator);

    const auto add_action_button = [this](QAction* action, const QString& object_name) {
        auto* button = new QToolButton(statusBar());
        button->setObjectName(object_name);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setDefaultAction(action);
        statusBar()->addPermanentWidget(button);
        return button;
    };

    mpd_repeat_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-repeat"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Repeat"), this);
    mpd_repeat_action_->setObjectName(QStringLiteral("action-mpd-repeat"));
    mpd_repeat_action_->setCheckable(true);
    mpd_repeat_button_ = add_action_button(mpd_repeat_action_, QStringLiteral("bench-mpd-repeat"));
    connect(mpd_repeat_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRepeatEnabled);

    mpd_random_action_ =
        new QAction(QIcon::fromTheme(QStringLiteral("media-playlist-shuffle"),
                                     style()->standardIcon(QStyle::SP_BrowserReload)),
                    QStringLiteral("Random"), this);
    mpd_random_action_->setObjectName(QStringLiteral("action-mpd-random"));
    mpd_random_action_->setCheckable(true);
    mpd_random_button_ = add_action_button(mpd_random_action_, QStringLiteral("bench-mpd-random"));
    connect(mpd_random_action_, &QAction::triggered, mpd_controller_,
            &quick::MpdProbeController::setRandomEnabled);

    mpd_single_action_ = new QAction(QStringLiteral("Cycle MPD single mode"), this);
    mpd_single_action_->setObjectName(QStringLiteral("action-mpd-single"));
    mpd_single_action_->setCheckable(true);
    mpd_single_button_ = new QToolButton(statusBar());
    mpd_single_button_->setObjectName(QStringLiteral("bench-mpd-single"));
    mpd_single_button_->setAutoRaise(true);
    mpd_single_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_single_button_->setDefaultAction(mpd_single_action_);
    statusBar()->addPermanentWidget(mpd_single_button_);
    connect(mpd_single_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->singleMode();
        mpd_controller_->setSingleMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_consume_action_ = new QAction(QStringLiteral("Cycle MPD consume mode"), this);
    mpd_consume_action_->setObjectName(QStringLiteral("action-mpd-consume"));
    mpd_consume_action_->setCheckable(true);
    mpd_consume_button_ = new QToolButton(statusBar());
    mpd_consume_button_->setObjectName(QStringLiteral("bench-mpd-consume"));
    mpd_consume_button_->setAutoRaise(true);
    mpd_consume_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_consume_button_->setDefaultAction(mpd_consume_action_);
    statusBar()->addPermanentWidget(mpd_consume_button_);
    connect(mpd_consume_action_, &QAction::triggered, this, [this] {
        const auto mode = mpd_controller_->consumeMode();
        mpd_controller_->setConsumeMode(mode < 0 || mode >= 2 ? 0 : mode + 1);
    });

    mpd_append_selection_action_ = new QAction(
        QIcon::fromTheme(QStringLiteral("list-add"), style()->standardIcon(QStyle::SP_ArrowRight)),
        QStringLiteral("Append selection to queue"), this);
    mpd_append_selection_action_->setObjectName(QStringLiteral("action-mpd-append-selection"));
    connect(mpd_append_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->addUris(selectedMpdQueueUris(), false); });
    mpd_add_next_selection_action_ = new QAction(
        QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowForward)),
        QStringLiteral("Add selection next"), this);
    mpd_add_next_selection_action_->setObjectName(QStringLiteral("action-mpd-add-next-selection"));
    connect(mpd_add_next_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->addUris(selectedMpdQueueUris(), true); });
    mpd_crop_selection_action_ = new QAction(QStringLiteral("Crop queue to selection"), this);
    mpd_crop_selection_action_->setObjectName(QStringLiteral("action-mpd-crop-selection"));
    connect(mpd_crop_selection_action_, &QAction::triggered, this,
            [this] { mpd_controller_->cropQueueToItems(selectedMpdQueueRows()); });

    mpd_priority_menu_ = new QMenu(QStringLiteral("Priority"), this);
    mpd_priority_menu_->setObjectName(QStringLiteral("bench-mpd-priority-menu"));
    mpd_priority_menu_->menuAction()->setObjectName(QStringLiteral("action-mpd-queue-priority"));
    auto* priority_group = new QActionGroup(mpd_priority_menu_);
    priority_group->setExclusive(true);
    const std::array priority_choices{
        std::pair{QStringLiteral("Normal"), 0},    std::pair{QStringLiteral("Low"), 64},
        std::pair{QStringLiteral("Medium"), 128},  std::pair{QStringLiteral("High"), 192},
        std::pair{QStringLiteral("Maximum"), 255},
    };
    for (const auto& [label, priority] : priority_choices) {
        auto* action =
            mpd_priority_menu_->addAction(QStringLiteral("%1 (%2)").arg(label).arg(priority));
        action->setObjectName(QStringLiteral("action-mpd-queue-priority-%1").arg(priority));
        action->setCheckable(true);
        action->setData(priority);
        priority_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, priority] {
            mpd_controller_->setQueuePriority(selectedMpdQueueRows(), priority);
        });
    }

    mpd_replaygain_button_ = new QToolButton(statusBar());
    mpd_replaygain_button_->setObjectName(QStringLiteral("bench-mpd-replaygain"));
    mpd_replaygain_button_->setIcon(QIcon::fromTheme(
        QStringLiteral("view-media-equalizer"), style()->standardIcon(QStyle::SP_MediaVolume)));
    mpd_replaygain_button_->setText(QStringLiteral("RG: —"));
    mpd_replaygain_button_->setAccessibleName(QStringLiteral("MPD ReplayGain mode"));
    mpd_replaygain_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mpd_replaygain_button_->setAutoRaise(true);
    mpd_replaygain_button_->setPopupMode(QToolButton::InstantPopup);
    auto* replaygain_menu = new QMenu(mpd_replaygain_button_);
    replaygain_menu->setObjectName(QStringLiteral("bench-mpd-replaygain-menu"));
    mpd_replaygain_group_ = new QActionGroup(replaygain_menu);
    mpd_replaygain_group_->setExclusive(true);
    const std::array modes{
        std::pair{QStringLiteral("Off"), QStringLiteral("off")},
        std::pair{QStringLiteral("Track"), QStringLiteral("track")},
        std::pair{QStringLiteral("Album"), QStringLiteral("album")},
        std::pair{QStringLiteral("Automatic"), QStringLiteral("auto")},
    };
    for (const auto& [label, value] : modes) {
        auto* action = replaygain_menu->addAction(label);
        action->setObjectName(QStringLiteral("action-mpd-replaygain-%1").arg(value));
        action->setCheckable(true);
        action->setData(value);
        mpd_replaygain_group_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, value] { mpd_controller_->setReplayGainMode(value); });
    }
    mpd_replaygain_button_->setMenu(replaygain_menu);
    statusBar()->addPermanentWidget(mpd_replaygain_button_);
    refreshMpdStatusControls();
}

void BenchMainWindow::refreshMpdStatusControls() {
    if (mpd_repeat_action_ == nullptr) {
        return;
    }
    const auto visible = isMpdContext();
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    mpd_status_separator_->setVisible(visible);
    mpd_repeat_button_->setVisible(visible);
    mpd_repeat_action_->setVisible(visible);
    mpd_repeat_action_->setEnabled(connected);
    mpd_repeat_action_->setChecked(mpd_controller_->repeatEnabled());
    mpd_repeat_action_->setToolTip(
        QStringLiteral("Repeat: %1")
            .arg(mpd_controller_->repeatEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));
    mpd_random_button_->setVisible(visible);
    mpd_random_action_->setVisible(visible);
    mpd_random_action_->setEnabled(connected);
    mpd_random_action_->setChecked(mpd_controller_->randomEnabled());
    mpd_random_action_->setToolTip(
        QStringLiteral("Random: %1")
            .arg(mpd_controller_->randomEnabled() ? QStringLiteral("On") : QStringLiteral("Off")));

    mpd_single_button_->setVisible(visible);
    mpd_single_action_->setEnabled(command_ready);
    mpd_single_action_->setChecked(mpd_controller_->singleMode() > 0);
    mpd_single_action_->setText(mpd_controller_->singleMode() == 2 ? QStringLiteral("1×")
                                                                   : QStringLiteral("1"));
    mpd_single_action_->setToolTip(
        QStringLiteral("Single: %1")
            .arg(mpd_controller_->singleMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->singleMode() == 1 ? QStringLiteral("On")
                                                      : QStringLiteral("Off")));

    mpd_consume_button_->setVisible(visible);
    mpd_consume_action_->setEnabled(command_ready);
    mpd_consume_action_->setChecked(mpd_controller_->consumeMode() > 0);
    mpd_consume_action_->setText(mpd_controller_->consumeMode() == 2 ? QStringLiteral("C×")
                                                                     : QStringLiteral("C"));
    mpd_consume_action_->setToolTip(
        QStringLiteral("Consume: %1")
            .arg(mpd_controller_->consumeMode() == 2   ? QStringLiteral("One-shot")
                 : mpd_controller_->consumeMode() == 1 ? QStringLiteral("On")
                                                       : QStringLiteral("Off")));

    const auto replaygain_visible = visible && mpd_controller_->supportsReplayGain();
    mpd_replaygain_button_->setVisible(replaygain_visible);
    mpd_replaygain_button_->setEnabled(command_ready);
    const auto replaygain = mpd_controller_->replayGainMode();
    QString replaygain_label = QStringLiteral("Unavailable");
    for (auto* action : mpd_replaygain_group_->actions()) {
        const auto selected = action->data().toString() == replaygain;
        action->setChecked(selected);
        if (selected) {
            replaygain_label = action->text();
        }
    }
    mpd_replaygain_button_->setText(QStringLiteral("RG: %1").arg(replaygain_label));
    mpd_replaygain_button_->setToolTip(QStringLiteral("ReplayGain mode: %1").arg(replaygain_label));
    mpd_replaygain_button_->setAccessibleDescription(
        QStringLiteral("Current ReplayGain mode is %1; activate to choose another mode")
            .arg(replaygain_label));

    if (mpd_search_field_ != nullptr) {
        mpd_search_field_->setVisible(visible);
        mpd_search_field_->setEnabled(connected);
        mpd_search_field_->setToolTip(connected ? QStringLiteral("Search the MPD server library")
                                                : QStringLiteral("Connect to search MPD"));
        resizeMpdSearchField();
    }
    if (!visible && mpd_search_surface_ != nullptr) {
        closeMpdSearch(false);
    }
    if (mpd_search_more_button_ != nullptr) {
        const auto query = mpd_search_field_->text().trimmed();
        const auto more =
            mpd_controller_->hasMoreSearchResults() && query == mpd_controller_->lastSearchQuery();
        mpd_search_more_button_->setVisible(more);
        mpd_search_more_button_->setEnabled(more && command_ready);
        if (mpd_search_surface_->isVisible()) {
            mpd_search_status_->setText(mpd_controller_->libraryStatus());
        }
    }
}

void BenchMainWindow::buildMpdWorkspace() {
    mpd_controller_ = new quick::MpdProbeController(this);
    server_library_model_ = new ui::ServerLibraryTreeModel(this);
    server_library_model_->setArtworkEnabled(true);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(mpd_controller_->queueModel());
    Q_ASSERT(queue_model != nullptr);
    queue_model->setArtworkEnabled(true);

    auto* view = new ui::QueueTableView(tabs_);
    mpd_queue_view_ = view;
    view->setObjectName(QStringLiteral("bench-mpd-queue"));
    view->setProperty("bench-mpd-queue", true);
    view->setAccessibleName(QStringLiteral("MPD Queue"));
    view->setModel(queue_model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
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
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([controller = mpd_controller_](const QModelIndex& index) {
        if (index.isValid()) {
            controller->playQueueItem(index.row());
        }
    });
    view->setReorderCallback(
        [controller = mpd_controller_](const QVariantList& rows, const int insertion_row) {
            controller->moveQueueItems(rows, insertion_row);
        });
    view->setExternalDropCallback([this](QAbstractItemView* source, const QVariantList&,
                                         const int insertion_row, const Qt::DropAction) {
        if (source != server_library_view_ || source->selectionModel() == nullptr) {
            return false;
        }
        QStringList uris;
        QSet<QString> seen;
        const auto indexes = source->selectionModel()->selectedRows(0);
        for (const auto& index : indexes) {
            for (const auto& track : server_library_model_->tracks(index)) {
                const auto uri = displayText(track.uri);
                if (!seen.contains(uri)) {
                    seen.insert(uri);
                    uris.push_back(uri);
                }
            }
        }
        if (uris.isEmpty()) {
            if (indexes.size() == 1 && server_library_model_->canFetchMore(indexes.front())) {
                pending_mpd_library_index_ = indexes.front();
                pending_mpd_library_action_ = MpdLibraryAction::insert;
                pending_mpd_library_insertion_row_ = insertion_row;
                server_library_view_->expand(indexes.front());
                server_library_model_->fetchMore(indexes.front());
                return true;
            }
            statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"),
                                     3'000);
            return false;
        }
        mpd_controller_->addUrisAt(uris, insertion_row);
        return true;
    });
    connect(view, &QTableView::doubleClicked, this,
            [controller = mpd_controller_](const QModelIndex& index) {
                if (index.isValid()) {
                    controller->playQueueItem(index.row());
                }
            });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsInserted, this,
            [this] { refreshSelectionStatus(); });
    connect(queue_model, &QAbstractItemModel::rowsRemoved, this,
            [this] { refreshSelectionStatus(); });

    mpd_view_layout_ = defaultTrackViewLayout();
    applyTrackViewLayout(mpd_queue_view_, mpd_view_layout_, mpd_view_layout_);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                mpd_view_layout_ = captureTrackViewLayout(mpd_queue_view_, mpd_view_layout_);
                mpd_view_layout_persistence_protected_ = false;
                preserved_mpd_view_layout_.clear();
                schedulePersist();
            });
    const auto queue_index = tabs_->addTab(view, QStringLiteral("MPD Queue"));
    tabs_->setTabToolTip(queue_index,
                         QStringLiteral("Authoritative queue on the connected MPD server"));
    if (auto* close = tabs_->tabBar()->tabButton(queue_index, QTabBar::RightSide)) {
        close->hide();
    }
    buildMpdSearch();

    server_library_view_ = new ui::ServerLibraryTreeView(source_stack_);
    server_library_view_->setObjectName(QStringLiteral("bench-mpd-library"));
    server_library_view_->setAccessibleName(QStringLiteral("MPD server library"));
    server_library_view_->setModel(server_library_model_);
    server_library_view_->setHeaderHidden(true);
    server_library_view_->setUniformRowHeights(false);
    server_library_view_->setIconSize(QSize{32, 32});
    server_library_view_->setIndentation(18);
    server_library_view_->setAnimated(true);
    server_library_view_->setExpandsOnDoubleClick(false);
    server_library_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    server_library_view_->setDragEnabled(true);
    server_library_view_->setDragDropMode(QAbstractItemView::DragOnly);
    server_library_view_->setDefaultDropAction(Qt::CopyAction);
    const std::array library_action_icons{
        QIcon::fromTheme(QStringLiteral("list-add"),
                         style()->standardIcon(QStyle::SP_DialogOpenButton)),
        QIcon::fromTheme(QStringLiteral("go-next"), style()->standardIcon(QStyle::SP_ArrowRight)),
        QIcon::fromTheme(QStringLiteral("media-playback-start"),
                         style()->standardIcon(QStyle::SP_MediaPlay)),
    };
    server_library_view_->setItemDelegate(
        new ui::ServerLibraryTreeDelegate(server_library_view_, library_action_icons));
    server_library_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(server_library_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showMpdLibraryContextMenu);
    server_library_view_->setActionCallback([this](const QModelIndex& index, const int action) {
        activateMpdLibraryAction(index, action);
    });
    source_stack_->addWidget(server_library_view_);

    connect(server_library_model_, &ui::ServerLibraryTreeModel::rootRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryRoot);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::branchRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryBranch);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(queue_model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryRootLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptRoot);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryBranchLoaded,
            server_library_model_, &ui::ServerLibraryTreeModel::acceptBranch);
    connect(mpd_controller_, &quick::MpdProbeController::serverLibraryArtworkLoaded, this,
            [this, queue_model](const quint64 token, const QByteArray& bytes) {
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this,
                        [this, watcher, token, queue_model] {
                            const auto image = watcher->result();
                            watcher->deleteLater();
                            server_library_model_->acceptArtwork(token, image);
                            mpd_search_model_->acceptArtwork(token, image);
                            queue_model->acceptArtwork(token, image);
                        });
                watcher->setFuture(QtConcurrent::run([bytes] {
                    auto image = QImage::fromData(bytes);
                    if (!image.isNull()) {
                        image =
                            image.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    return image;
                }));
            });
    connect(mpd_controller_, &quick::MpdProbeController::serverDatabaseChanged,
            server_library_model_, &ui::ServerLibraryTreeModel::reload);
    connect(server_library_model_, &ui::ServerLibraryTreeModel::browseError, this,
            [this](const QString& error) {
                pending_mpd_library_action_.reset();
                pending_mpd_library_index_ = QPersistentModelIndex{};
                pending_mpd_library_insertion_row_ = -1;
                server_library_view_->cancelPendingExpansions();
                statusBar()->showMessage(
                    QStringLiteral("Could not browse the MPD library: %1").arg(error), 5'000);
            });
    connect(server_library_model_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, const int, const int) {
                QTimer::singleShot(0, this, [this] {
                    server_library_view_->completePendingExpansions();
                    completePendingMpdLibraryAction();
                });
            });
    connect(mpd_controller_, &quick::MpdProbeController::notificationRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5'000); });
    connect(mpd_controller_, &quick::MpdProbeController::searchFinished, this,
            &BenchMainWindow::finishMpdSearch);
    connect(mpd_controller_, &quick::MpdProbeController::stateChanged, this, [this] {
        const auto connected = mpd_controller_->connected();
        if (connected && !mpd_was_connected_) {
            server_library_model_->reload();
        }
        mpd_was_connected_ = connected;
        refreshActiveContext();
        refreshTransport();
        refreshSelectionStatus();
        refreshMpdStatusControls();
    });
    auto* output_model = mpd_controller_->outputModel();
    const auto refresh_outputs = [this] {
        if (isMpdContext() && device_menu_ != nullptr) {
            rebuildDeviceMenu();
        }
    };
    connect(output_model, &QAbstractItemModel::modelReset, this, refresh_outputs);
    connect(output_model, &QAbstractItemModel::rowsInserted, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::rowsRemoved, this,
            [refresh_outputs](const QModelIndex&, const int, const int) { refresh_outputs(); });
    connect(output_model, &QAbstractItemModel::dataChanged, this,
            [refresh_outputs](const QModelIndex&, const QModelIndex&, const QList<int>&) {
                refresh_outputs();
            });
}

void BenchMainWindow::activateMpdLibraryAction(const QModelIndex& index, const int action) {
    if (!index.isValid() || action < static_cast<int>(MpdLibraryAction::append) ||
        action > static_cast<int>(MpdLibraryAction::replace)) {
        return;
    }
    server_library_view_->setCurrentIndex(index);
    if (!mpd_controller_->connected()) {
        statusBar()->showMessage(QStringLiteral("Connect to MPD to use the server library"), 3'000);
        return;
    }
    if (server_library_model_->canFetchMore(index)) {
        pending_mpd_library_index_ = index;
        pending_mpd_library_action_ = static_cast<MpdLibraryAction>(action);
        server_library_view_->expand(index);
        server_library_model_->fetchMore(index);
        return;
    }

    const auto tracks = server_library_model_->tracks(index);
    if (tracks.empty()) {
        statusBar()->showMessage(QStringLiteral("This library entry contains no tracks"), 3'000);
        return;
    }
    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    const auto requested = static_cast<MpdLibraryAction>(action);
    if (requested == MpdLibraryAction::replace) {
        mpd_controller_->replaceQueueWithUris(uris);
    } else {
        mpd_controller_->addUris(uris, requested == MpdLibraryAction::next);
    }
}

void BenchMainWindow::completePendingMpdLibraryAction() {
    if (!pending_mpd_library_action_) {
        return;
    }
    if (!pending_mpd_library_index_.isValid()) {
        pending_mpd_library_action_.reset();
        return;
    }
    const auto index = QModelIndex{pending_mpd_library_index_};
    const auto requested = *pending_mpd_library_action_;
    pending_mpd_library_action_.reset();
    pending_mpd_library_index_ = QPersistentModelIndex{};
    if (requested == MpdLibraryAction::insert) {
        const auto tracks = server_library_model_->tracks(index);
        QStringList uris;
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        if (!uris.isEmpty() && pending_mpd_library_insertion_row_ >= 0) {
            mpd_controller_->addUrisAt(uris, pending_mpd_library_insertion_row_);
        }
        pending_mpd_library_insertion_row_ = -1;
        return;
    }
    pending_mpd_library_insertion_row_ = -1;
    activateMpdLibraryAction(index, static_cast<int>(requested));
}

void BenchMainWindow::showMpdLibraryContextMenu(const QPoint& position) {
    if (mpd_library_context_menu_ == nullptr) {
        return;
    }
    const auto index = server_library_view_->indexAt(position).siblingAtColumn(0);
    if (!index.isValid()) {
        return;
    }
    server_library_view_->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto target = QPersistentModelIndex{index};
    const auto command_ready = mpd_controller_->connected() && !mpd_controller_->commandBusy();
    mpd_library_context_menu_->clear();
    const std::array actions{
        std::pair{QStringLiteral("Append to live queue"), QStringLiteral("list-add")},
        std::pair{QStringLiteral("Insert next in live queue"), QStringLiteral("go-next")},
        std::pair{QStringLiteral("Replace queue and play"), QStringLiteral("media-playback-start")},
    };
    for (int action = 0; action < static_cast<int>(actions.size()); ++action) {
        const auto& [label, icon] = actions[static_cast<std::size_t>(action)];
        auto* command = mpd_library_context_menu_->addAction(QIcon::fromTheme(icon), label);
        command->setObjectName(QStringLiteral("action-mpd-library-%1").arg(action));
        command->setEnabled(command_ready);
        connect(command, &QAction::triggered, this, [this, target, action] {
            if (target.isValid()) {
                activateMpdLibraryAction(target, action);
            }
        });
    }
    if (server_library_model_->hasChildren(index)) {
        mpd_library_context_menu_->addSeparator();
        auto* expand = mpd_library_context_menu_->addAction(server_library_view_->isExpanded(index)
                                                                ? QStringLiteral("Collapse")
                                                                : QStringLiteral("Expand"));
        connect(expand, &QAction::triggered, this, [this, target] {
            if (target.isValid()) {
                server_library_view_->setExpanded(target,
                                                  !server_library_view_->isExpanded(target));
            }
        });
    }
    mpd_library_context_menu_->popup(server_library_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::buildMpdSearch() {
    auto* field = new MpdSearchLineEdit(tabs_);
    mpd_search_field_ = field;
    field->setObjectName(QStringLiteral("bench-mpd-search"));
    field->setAccessibleName(QStringLiteral("Search MPD library"));
    field->setClearButtonEnabled(true);
    field->setPlaceholderText(QStringLiteral("Search MPD…"));
    field->setMinimumWidth(0);
    field->setMaximumWidth(340);
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    field->addAction(QIcon::fromTheme(QStringLiteral("edit-find"),
                                      style()->standardIcon(QStyle::SP_FileDialogContentsView)),
                     QLineEdit::LeadingPosition);
    field->show();
    field->raise();
    resizeMpdSearchField();

    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("bench-mpd-search-surface"));
    surface->setFrameShape(QFrame::StyledPanel);
    surface->setFrameShadow(QFrame::Raised);
    surface->setAutoFillBackground(true);
    surface->hide();
    mpd_search_surface_ = surface;
    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    mpd_search_model_ = new quick::MpdSearchResultModel(surface);
    // The controller answers unsupported/disconnected requests with an empty
    // result, so keeping this enabled also guarantees that a later reconnect
    // cannot leave the search model permanently stuck on placeholders.
    mpd_search_model_->setArtworkEnabled(true);
    mpd_search_model_->setAlbumPlaceholder(QIcon::fromTheme(
        QStringLiteral("media-optical-audio"), style()->standardIcon(QStyle::SP_FileIcon)));
    auto* results = new MpdSearchTableView(surface);
    mpd_search_view_ = results;
    results->setObjectName(QStringLiteral("bench-mpd-search-results"));
    results->setAccessibleName(QStringLiteral("MPD library search results"));
    results->setAccessibleDescription(QStringLiteral(
        "Use Up and Down for results, Left and Right for queue actions, Enter to activate, "
        "Control Enter to replace the queue, and Escape to close search."));
    results->setModel(mpd_search_model_);
    results->setSearchField(field);
    results->setSelectionBehavior(QAbstractItemView::SelectRows);
    results->setSelectionMode(QAbstractItemView::SingleSelection);
    results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results->setAlternatingRowColors(false);
    results->setShowGrid(false);
    results->setWordWrap(false);
    results->setTextElideMode(Qt::ElideRight);
    results->setMouseTracking(true);
    results->setIconSize(QSize{24, 24});
    results->verticalHeader()->hide();
    results->verticalHeader()->setDefaultSectionSize(30);
    results->verticalHeader()->setMinimumSectionSize(30);
    results->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    results->horizontalHeader()->hide();
    results->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    results->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    results->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    results->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    results->horizontalHeader()->setMinimumSectionSize(18);
    results->setColumnWidth(0, 150);
    results->setColumnWidth(2, 190);
    results->setColumnWidth(3, 72);
    for (int column = quick::MpdSearchResultModel::first_action_column;
         column < quick::MpdSearchResultModel::column_count; ++column) {
        results->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        results->horizontalHeader()->resizeSection(column, 28);
    }
    auto* album_delegate = new MpdSearchAlbumDelegate(results);
    album_delegate->setObjectName(QStringLiteral("bench-mpd-search-album-delegate"));
    results->setItemDelegateForColumn(0, album_delegate);
    results->setItemDelegateForColumn(
        4,
        new MpdSearchActionDelegate(QIcon::fromTheme(QStringLiteral("list-add"),
                                                     style()->standardIcon(QStyle::SP_ArrowRight)),
                                    results));
    results->setItemDelegateForColumn(
        5, new MpdSearchActionDelegate(
               QIcon::fromTheme(QStringLiteral("go-next"),
                                style()->standardIcon(QStyle::SP_ArrowForward)),
               results));
    results->setItemDelegateForColumn(
        6,
        new MpdSearchActionDelegate(QIcon::fromTheme(QStringLiteral("media-playback-start"),
                                                     style()->standardIcon(QStyle::SP_MediaPlay)),
                                    results));
    layout->addWidget(results, 1);

    auto* footer = new QWidget(surface);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(6, 2, 4, 2);
    footer_layout->setSpacing(6);
    mpd_search_status_ =
        new QLabel(QStringLiteral("Type at least two characters to search"), footer);
    mpd_search_status_->setObjectName(QStringLiteral("bench-mpd-search-status"));
    mpd_search_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    footer_layout->addWidget(mpd_search_status_, 1);
    mpd_search_more_button_ = new QToolButton(footer);
    mpd_search_more_button_->setObjectName(QStringLiteral("bench-mpd-search-more"));
    mpd_search_more_button_->setText(QStringLiteral("More results"));
    mpd_search_more_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mpd_search_more_button_->setAutoRaise(true);
    mpd_search_more_button_->hide();
    footer_layout->addWidget(mpd_search_more_button_);
    layout->addWidget(footer);

    mpd_search_timer_ = new QTimer(this);
    mpd_search_timer_->setSingleShot(true);
    mpd_search_timer_->setInterval(180);
    connect(field, &QLineEdit::textEdited, this, [this] {
        if (!isMpdContext()) {
            return;
        }
        mpd_search_surface_->show();
        mpd_search_surface_->raise();
        positionMpdSearchSurface();
        mpd_search_timer_->start();
    });
    connect(mpd_search_timer_, &QTimer::timeout, this, &BenchMainWindow::previewMpdSearch);
    connect(mpd_search_more_button_, &QToolButton::clicked, mpd_controller_,
            &quick::MpdProbeController::continueSearch);
    connect(results, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (index.column() >= quick::MpdSearchResultModel::first_action_column) {
            activateMpdSearchResult(
                index.row(), index.column() - quick::MpdSearchResultModel::first_action_column);
        }
    });
    connect(results, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() < quick::MpdSearchResultModel::first_action_column) {
            activateMpdSearchResult(index.row(), 0);
        }
    });
    results->setActionCallback([this](const int row, const MpdSearchQueueAction action) {
        activateMpdSearchResult(row, static_cast<int>(action));
    });
    results->setCloseCallback([this] { closeMpdSearch(); });
    field->setResultFocusCallback([results] { results->focusFirstResult(); });
    field->setCloseCallback([this] { closeMpdSearch(); });
    field->setActionCallback(
        [results](const MpdSearchQueueAction action) { results->activateDefault(action); });
    connect(mpd_search_model_, &quick::MpdSearchResultModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);

    auto* focus_search = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focus_search, &QShortcut::activated, this, [this] {
        tabs_->setCurrentWidget(mpd_queue_view_);
        mpd_search_surface_->show();
        mpd_search_surface_->raise();
        positionMpdSearchSurface();
        mpd_search_field_->setFocus();
        mpd_search_field_->selectAll();
    });

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        QTimer::singleShot(0, this, [this] {
            if (mpd_search_surface_ == nullptr || !mpd_search_surface_->isVisible()) {
                return;
            }
            auto* focused = QApplication::focusWidget();
            const auto inside_field = focused == mpd_search_field_;
            const auto inside_surface =
                focused != nullptr &&
                (focused == mpd_search_surface_ || mpd_search_surface_->isAncestorOf(focused));
            if (!inside_field && !inside_surface) {
                closeMpdSearch(false);
            }
        });
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](const Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) {
                    closeMpdSearch(false);
                }
            });
}

void BenchMainWindow::previewMpdSearch() {
    if (!isMpdContext() || mpd_search_field_ == nullptr) {
        return;
    }
    const auto query = mpd_search_field_->text().trimmed();
    if (query.size() < 2) {
        mpd_search_model_->replaceTracks({});
        syncMpdSearchView();
    }
    mpd_controller_->searchLibrary(query);
    mpd_search_status_->setText(mpd_controller_->libraryStatus());
    mpd_search_more_button_->hide();
}

void BenchMainWindow::finishMpdSearch(const QString& query, const bool success) {
    if (mpd_search_field_ == nullptr || query != mpd_search_field_->text().trimmed()) {
        return;
    }
    if (success) {
        std::vector<mpd::Track> tracks;
        if (const auto* source =
                qobject_cast<const quick::MpdQueueModel*>(mpd_controller_->libraryModel())) {
            tracks = source->tracksSnapshot();
        }
        mpd_search_model_->replaceSearchResults(mpd_controller_->libraryAlbumsSnapshot(),
                                                std::move(tracks));
    } else {
        mpd_search_model_->replaceTracks({});
    }
    syncMpdSearchView();
    mpd_search_status_->setText(success ? mpd_controller_->libraryStatus()
                                        : QStringLiteral("Search did not complete"));
    const auto more = success && mpd_controller_->hasMoreSearchResults() &&
                      query == mpd_controller_->lastSearchQuery();
    mpd_search_more_button_->setVisible(more);
    mpd_search_more_button_->setEnabled(more && !mpd_controller_->commandBusy());
}

void BenchMainWindow::syncMpdSearchView() {
    mpd_search_view_->clearSpans();
    for (const auto row : mpd_search_model_->sectionRows()) {
        mpd_search_view_->setSpan(row, 0, 1, quick::MpdSearchResultModel::column_count);
    }
    const auto current = mpd_search_view_->currentIndex();
    if (!current.isValid() || mpd_search_model_->kindAt(current.row()) ==
                                  quick::MpdSearchResultModel::ResultKind::section) {
        const auto first = mpd_search_model_->firstResultRow();
        if (first >= 0) {
            mpd_search_view_->setCurrentIndex(mpd_search_model_->index(first, 1));
        }
    }
}

void BenchMainWindow::activateMpdSearchResult(const int row, const int action) {
    if (mpd_search_model_ == nullptr || row < 0 ||
        mpd_search_model_->kindAt(row) == quick::MpdSearchResultModel::ResultKind::section) {
        return;
    }
    const auto requested = static_cast<MpdSearchQueueAction>(action);
    if (const auto album = mpd_search_model_->albumAt(row)) {
        const auto mode = requested == MpdSearchQueueAction::append ? quick::QueueAddMode::append
                          : requested == MpdSearchQueueAction::next ? quick::QueueAddMode::next
                                                                    : quick::QueueAddMode::replace;
        mpd_controller_->addAlbum(*album, mode);
        return;
    }
    const auto uris = mpd_search_model_->urisAt(row);
    if (uris.isEmpty()) {
        return;
    }
    if (requested == MpdSearchQueueAction::append) {
        mpd_controller_->addUris(uris, false);
    } else if (requested == MpdSearchQueueAction::next) {
        mpd_controller_->addUris(uris, true);
    } else {
        mpd_controller_->replaceQueueWithUris(uris);
    }
}

void BenchMainWindow::closeMpdSearch(const bool restore_queue_focus) {
    if (mpd_search_surface_ == nullptr) {
        return;
    }
    mpd_search_surface_->hide();
    if (restore_queue_focus && isMpdContext() && mpd_queue_view_ != nullptr) {
        mpd_queue_view_->setFocus(Qt::ShortcutFocusReason);
    }
}

void BenchMainWindow::resizeMpdSearchField() {
    if (tabs_ == nullptr || mpd_search_field_ == nullptr) {
        return;
    }
    // This is an explicit child overlay instead of QTabWidget's corner-widget
    // slot. The latter retains stale full-width geometry when the Track Lists
    // panel moves between persisted splitter/tab layouts on some styles.
    const auto frame = tabs_->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, tabs_);
    const auto available = std::max(1, tabs_->width() - frame * 2);
    const auto proportional = std::max(1, tabs_->width() * 2 / 5);
    const auto field_width = std::min({340, available, proportional});
    const auto bar = tabs_->tabBar()->geometry();
    const auto field_height = std::min(mpd_search_field_->sizeHint().height(), bar.height());
    mpd_search_field_->setGeometry(tabs_->width() - frame - field_width,
                                   bar.top() + std::max(0, (bar.height() - field_height) / 2),
                                   field_width, field_height);
    mpd_search_field_->raise();

    // Reserve the overlay's horizontal area so tab scroll buttons appear
    // before tab labels can slide beneath the field.
    tabs_->tabBar()->setMaximumWidth(mpd_search_field_->isVisible()
                                         ? std::max(1, tabs_->width() - field_width - frame * 2 - 4)
                                         : QWIDGETSIZE_MAX);
}

void BenchMainWindow::positionMpdSearchSurface() {
    if (mpd_search_surface_ == nullptr || mpd_search_field_ == nullptr) {
        return;
    }
    const auto anchor = mpd_search_field_->mapTo(
        this, QPoint{mpd_search_field_->width(), mpd_search_field_->height()});
    const auto maximum_width = std::max(520, width() - 24);
    const auto surface_width = std::clamp(width() * 3 / 5, 520, maximum_width);
    const auto x =
        std::clamp(anchor.x() - surface_width, 12, std::max(12, width() - 12 - surface_width));
    const auto y = anchor.y() + 4;
    const auto available_bottom = statusBar() != nullptr ? statusBar()->geometry().top() : height();
    const auto surface_height = std::min(480, std::max(220, available_bottom - y - 12));
    mpd_search_surface_->setGeometry(x, y, surface_width, surface_height);
}


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

void BenchMainWindow::buildTransport() {
    auto* bar = addToolBar(QStringLiteral("Transport"));
    bar->setObjectName(QStringLiteral("bench-transport"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize{18, 18});
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    previous_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                   QStringLiteral("Previous"), this);
    connect(previous_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->previous();
        } else {
            playAdjacent(-1);
        }
    });
    play_pause_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaPlay), QStringLiteral("Play"), this);
    play_pause_action_->setShortcut(Qt::Key_Space);
    play_pause_action_->setShortcutContext(Qt::ApplicationShortcut);
    connect(play_pause_action_, &QAction::triggered, this, &BenchMainWindow::togglePlayPause);
    stop_action_ =
        new QAction(style()->standardIcon(QStyle::SP_MediaStop), QStringLiteral("Stop"), this);
    connect(stop_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->stop();
        } else if (player_ != nullptr) {
            static_cast<void>(player_->stop());
        }
    });
    next_action_ = new QAction(style()->standardIcon(QStyle::SP_MediaSkipForward),
                               QStringLiteral("Next"), this);
    connect(next_action_, &QAction::triggered, this, [this] {
        if (isMpdContext()) {
            mpd_controller_->next();
        } else {
            playAdjacent(1);
        }
    });

    auto* header = new QWidget(bar);
    header->setObjectName(QStringLiteral("bench-player-header"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* header_layout = new QGridLayout(header);
    header_layout->setContentsMargins(6, 3, 6, 3);
    header_layout->setHorizontalSpacing(6);
    header_layout->setVerticalSpacing(0);
    header_layout->setColumnStretch(2, 1);

    auto* app_menu_button = new QToolButton(header);
    app_menu_button->setObjectName(QStringLiteral("bench-main-menu"));
    app_menu_button->setText(QStringLiteral("☰"));
    app_menu_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    app_menu_button->setAutoRaise(true);
    app_menu_button->setFixedSize(26, 26);
    app_menu_button->setPopupMode(QToolButton::InstantPopup);
    app_menu_button->setAccessibleName(QStringLiteral("Application menu"));
    header_layout->addWidget(app_menu_button, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    auto* transport = new QWidget(header);
    transport->setObjectName(QStringLiteral("bench-transport-buttons"));
    auto* transport_layout = new QHBoxLayout(transport);
    transport_layout->setContentsMargins(0, 0, 2, 0);
    transport_layout->setSpacing(1);
    const auto add_transport_button = [transport, transport_layout](QAction* action) {
        auto* button = new QToolButton(transport);
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(26, 26);
        button->setIconSize(QSize{18, 18});
        transport_layout->addWidget(button);
    };
    add_transport_button(previous_action_);
    add_transport_button(play_pause_action_);
    add_transport_button(stop_action_);
    add_transport_button(next_action_);
    header_layout->addWidget(transport, 1, 0, Qt::AlignVCenter);

    auto* track_display = new QWidget(header);
    track_display->setObjectName(QStringLiteral("bench-track-display"));
    track_display->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* track_display_layout = new QVBoxLayout(track_display);
    track_display_layout->setContentsMargins(0, 0, 0, 1);
    track_display_layout->setSpacing(0);

    now_playing_ = new QLabel(track_display);
    now_playing_->setObjectName(QStringLiteral("bench-now-playing"));
    now_playing_->setTextFormat(Qt::PlainText);
    now_playing_->setAccessibleName(QStringLiteral("Current artist and title"));
    now_playing_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_);

    now_playing_context_ = new QLabel(track_display);
    now_playing_context_->setObjectName(QStringLiteral("bench-now-playing-context"));
    now_playing_context_->setTextFormat(Qt::PlainText);
    now_playing_context_->setAccessibleName(QStringLiteral("Current album and date"));
    now_playing_context_->setForegroundRole(QPalette::PlaceholderText);
    now_playing_context_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    now_playing_context_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    track_display_layout->addWidget(now_playing_context_);
    header_layout->addWidget(track_display, 0, 2);

    elapsed_ = new QLabel(QStringLiteral("0:00"), header);
    elapsed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    elapsed_->setFixedWidth(elapsed_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(elapsed_, 1, 1, Qt::AlignVCenter);
    seek_ = new ui::LineSlider(header);
    seek_->setObjectName(QStringLiteral("bench-seek"));
    seek_->setAccessibleName(QStringLiteral("Playback position"));
    seek_->setMinimumWidth(200);
    seek_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(seek_, &QSlider::sliderPressed, this, [this] { seeking_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        seeking_ = false;
        seekToMs(seek_->value());
    });
    header_layout->addWidget(seek_, 1, 2, Qt::AlignVCenter);
    duration_ = new QLabel(QStringLiteral("0:00"), header);
    duration_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    duration_->setFixedWidth(
        duration_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    header_layout->addWidget(duration_, 1, 3, Qt::AlignVCenter);

    volume_ = new ui::LineSlider(header);
    volume_->setObjectName(QStringLiteral("bench-volume"));
    volume_->setAccessibleName(QStringLiteral("Volume"));
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(104);
    volume_->setToolTip(QStringLiteral("Volume"));
    connect(volume_, &QSlider::sliderPressed, this, [this] { changing_volume_ = true; });
    connect(volume_, &QSlider::sliderReleased, this, [this] { changing_volume_ = false; });
    connect(volume_, &QSlider::valueChanged, this, [this](const int value) {
        if (isMpdContext()) {
            mpd_controller_->setVolume(value);
        } else if (player_ != nullptr) {
            static_cast<void>(player_->set_volume_percent(value));
        }
    });
    header_layout->addWidget(volume_, 1, 4, Qt::AlignVCenter);

    device_button_ = new QToolButton(header);
    device_button_->setObjectName(QStringLiteral("bench-device"));
    device_button_->setIcon(QIcon::fromTheme(QStringLiteral("audio-speakers"),
                                             style()->standardIcon(QStyle::SP_ComputerIcon)));
    device_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    device_button_->setAutoRaise(true);
    device_button_->setFixedSize(26, 26);
    device_button_->setIconSize(QSize{18, 18});
    device_button_->setPopupMode(QToolButton::InstantPopup);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));
    device_menu_ = new QMenu(device_button_);
    device_menu_->setObjectName(QStringLiteral("bench-device-menu"));
    device_group_ = new QActionGroup(device_menu_);
    device_group_->setExclusive(true);
    device_button_->setMenu(device_menu_);
    rebuildDeviceMenu();
    header_layout->addWidget(device_button_, 1, 5, Qt::AlignVCenter);
    bar->addWidget(header);

    auto* playback_menu = menuBar()->addMenu(QStringLiteral("&Playback"));
    playback_menu->addAction(play_pause_action_);
    playback_menu->addAction(stop_action_);
    playback_menu->addAction(previous_action_);
    playback_menu->addAction(next_action_);
    playback_menu->addSeparator();

    buffer_menu_ = playback_menu->addMenu(QStringLiteral("Playback buffer"));
    buffer_menu_->setObjectName(QStringLiteral("bench-buffer-menu"));
    buffer_group_ = new QActionGroup(buffer_menu_);
    buffer_group_->setExclusive(true);
    const auto add_buffer_preset = [this](const QString& label,
                                          const audio::PlaybackBufferPreset preset) {
        auto* action = buffer_menu_->addAction(label);
        const auto id = audio::playback_buffer_preset_id(preset);
        const auto profile = QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
        const auto config = audio::playback_buffer_preset_config(preset);
        action->setObjectName(QStringLiteral("action-buffer-%1").arg(profile));
        action->setData(profile);
        action->setCheckable(true);
        action->setToolTip(QStringLiteral("%1 ms capacity; playback starts at %2 ms")
                               .arg(config.capacity.count())
                               .arg(config.start_threshold.count()));
        buffer_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, profile, config] {
            configurePlaybackBuffer(profile, static_cast<int>(config.capacity.count()),
                                    static_cast<int>(config.start_threshold.count()));
        });
    };
    add_buffer_preset(QStringLiteral("Responsive"), audio::PlaybackBufferPreset::responsive);
    add_buffer_preset(QStringLiteral("Balanced"), audio::PlaybackBufferPreset::balanced);
    add_buffer_preset(QStringLiteral("Resilient"), audio::PlaybackBufferPreset::resilient);
    buffer_menu_->addSeparator();
    auto* custom_buffer = buffer_menu_->addAction(QStringLiteral("Custom…"));
    custom_buffer->setObjectName(QStringLiteral("action-buffer-custom"));
    custom_buffer->setData(QStringLiteral("custom"));
    custom_buffer->setCheckable(true);
    buffer_group_->addAction(custom_buffer);
    connect(custom_buffer, &QAction::triggered, this,
            &BenchMainWindow::showCustomPlaybackBufferDialog);
    refreshPlaybackBufferChecks();

    auto* refresh_devices = playback_menu->addAction(QStringLiteral("Refresh audio devices"));
    connect(refresh_devices, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });

    auto* app_menu = new QMenu(app_menu_button);
    app_menu->setObjectName(QStringLiteral("bench-main-menu-popup"));
    for (auto* action : menuBar()->actions()) {
        app_menu->addAction(action);
    }
    app_menu_button->setMenu(app_menu);
    menuBar()->hide();
}

void BenchMainWindow::configurePlaybackBuffer(const QString& profile, const int capacity_ms,
                                              const int start_threshold_ms) {
    const audio::PlaybackBufferDurationConfig config{
        .capacity = std::chrono::milliseconds{capacity_ms},
        .start_threshold = std::chrono::milliseconds{start_threshold_ms},
    };
    if (!audio::valid_local_audition_buffer_config(config)) {
        statusBar()->showMessage(QStringLiteral("Invalid playback buffer values"), 5'000);
        refreshPlaybackBufferChecks();
        return;
    }

    auto active_buffer = std::optional<audio::PlaybackBufferDurationConfig>{};
    if (player_ != nullptr) {
        active_buffer = player_->snapshot().active_buffer;
        if (auto changed = player_->set_buffer_config(config); !changed) {
            statusBar()->showMessage(QStringLiteral("Playback buffer unchanged: %1")
                                         .arg(displayText(changed.error().message)),
                                     5'000);
            refreshPlaybackBufferChecks();
            return;
        }
    }

    selected_buffer_profile_ = profile;
    QSettings settings;
    settings.setValue(QString::fromLatin1(buffer_profile_settings_key), profile);
    settings.setValue(QString::fromLatin1(buffer_capacity_settings_key), capacity_ms);
    settings.setValue(QString::fromLatin1(buffer_threshold_settings_key), start_threshold_ms);
    settings.sync();
    refreshPlaybackBufferChecks();

    const bool pending = active_buffer && *active_buffer != config;
    statusBar()->showMessage(
        QStringLiteral("%1 buffer · %2 ms capacity · %3 ms start%4")
            .arg(bufferProfileLabel(profile))
            .arg(capacity_ms)
            .arg(start_threshold_ms)
            .arg(pending ? QStringLiteral(" · applies next track") : QString{}),
        5'000);
}

void BenchMainWindow::showCustomPlaybackBufferDialog() {
    const auto current = loadPlaybackBufferPreference().config;

    QDialog dialog{this};
    dialog.setObjectName(QStringLiteral("bench-custom-buffer-dialog"));
    dialog.setWindowTitle(QStringLiteral("Custom playback buffer"));
    auto* form = new QFormLayout(&dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* capacity = new QSpinBox(&dialog);
    capacity->setObjectName(QStringLiteral("bench-buffer-capacity"));
    capacity->setRange(minimum_custom_buffer_ms, maximum_custom_buffer_ms);
    capacity->setSuffix(QStringLiteral(" ms"));
    capacity->setValue(static_cast<int>(current.capacity.count()));
    form->addRow(QStringLiteral("Capacity"), capacity);

    auto* threshold = new QSpinBox(&dialog);
    threshold->setObjectName(QStringLiteral("bench-buffer-start-threshold"));
    threshold->setRange(1, capacity->value());
    threshold->setSuffix(QStringLiteral(" ms"));
    threshold->setValue(
        std::min(static_cast<int>(current.start_threshold.count()), capacity->value()));
    form->addRow(QStringLiteral("Start playback at"), threshold);
    connect(capacity, &QSpinBox::valueChanged, threshold,
            [threshold](const int value) { threshold->setMaximum(value); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        configurePlaybackBuffer(QStringLiteral("custom"), capacity->value(), threshold->value());
    } else {
        refreshPlaybackBufferChecks();
    }
}

void BenchMainWindow::refreshPlaybackBufferChecks() {
    if (buffer_group_ == nullptr) {
        return;
    }
    for (auto* action : buffer_group_->actions()) {
        action->setChecked(action->data().toString() == selected_buffer_profile_);
    }
}

void BenchMainWindow::rebuildDeviceMenu() {
    device_menu_->clear();

    if (isMpdContext()) {
        device_group_->setExclusive(false);
        auto* output_model = mpd_controller_->outputModel();
        for (int row = 0; row < output_model->rowCount(); ++row) {
            const auto index = output_model->index(row, 0);
            const auto id = output_model->data(index, quick::MpdOutputModel::OutputIdRole).toUInt();
            const auto name = output_model->data(index, quick::MpdOutputModel::NameRole).toString();
            const auto enabled =
                output_model->data(index, quick::MpdOutputModel::EnabledRole).toBool();
            const auto primary =
                output_model->data(index, quick::MpdOutputModel::PrimaryRole).toBool();
            auto* action = device_menu_->addAction(name);
            action->setObjectName(QStringLiteral("action-mpd-output-%1").arg(id));
            action->setCheckable(true);
            action->setChecked(mpd_controller_->supportsExclusiveOutput() ? primary : enabled);
            action->setToolTip(
                output_model->data(index, quick::MpdOutputModel::DetailRole).toString());
            device_group_->addAction(action);
            connect(action, &QAction::triggered, this, [this, id, enabled] {
                if (mpd_controller_->supportsExclusiveOutput()) {
                    mpd_controller_->switchOutput(id);
                } else {
                    mpd_controller_->setOutputEnabled(id, !enabled);
                }
            });
        }
        if (device_menu_->isEmpty()) {
            auto* none = device_menu_->addAction(mpd_controller_->connected()
                                                     ? QStringLiteral("No MPD outputs")
                                                     : QStringLiteral("Connect to MPD"));
            none->setEnabled(false);
        }
        device_button_->setAccessibleName(QStringLiteral("MPD output"));
        return;
    }

    device_group_->setExclusive(true);
    device_button_->setAccessibleName(QStringLiteral("Audio output device"));

    const auto add_choice = [this](const QString& label, std::optional<std::string> target,
                                   const bool enabled = true) {
        auto* action = device_menu_->addAction(label);
        action->setCheckable(true);
        action->setChecked(target == selected_device_);
        action->setEnabled(enabled);
        device_group_->addAction(action);
        connect(action, &QAction::triggered, this, [this, target = std::move(target)] {
            if (player_ != nullptr) {
                static_cast<void>(player_->set_output_target(target));
            }
        });
        return action;
    };

    add_choice(QStringLiteral("System default"), std::nullopt);
    for (const auto& [name, description] : device_choices_) {
        add_choice(displayText(description.empty() ? name : description), name);
    }
    if (selected_device_ && std::ranges::none_of(device_choices_, [this](const auto& choice) {
            return choice.first == *selected_device_;
        })) {
        add_choice(QStringLiteral("%1 (unavailable)").arg(displayText(*selected_device_)),
                   selected_device_, false);
    }
    device_menu_->addSeparator();
    auto* refresh = device_menu_->addAction(QStringLiteral("Refresh audio devices"));
    refresh->setObjectName(QStringLiteral("action-refresh-audio-devices"));
    connect(refresh, &QAction::triggered, this, [this] {
        if (player_ != nullptr) {
            static_cast<void>(player_->refresh_output_devices());
        }
    });
}

void BenchMainWindow::openMpdConnectionDialog() {
    if (auto* existing = findChild<ui::MpdConnectionDialog*>(); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    if (mpd_queue_view_ != nullptr) {
        tabs_->setCurrentWidget(mpd_queue_view_);
    }
    auto* dialog = new ui::MpdConnectionDialog(this, mpd_profiles_, mpd_controller_->profileId());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &ui::MpdConnectionDialog::connectionRequested, this,
        [this](const QString& profile_id, const QString& profile_name, const QString& host,
               const int port, const QString& password, const QString& music_root,
               const bool auto_connect) {
            const auto parsed_id = core::StableId::parse(profile_id.toStdString());
            if (!parsed_id) {
                statusBar()->showMessage(
                    QStringLiteral("Connection profile has an invalid identity"), 5'000);
                return;
            }
            persistence::ConnectionProfile updated{
                .id = *parsed_id,
                .name = utf8Bytes(profile_name),
                .host = utf8Bytes(host),
                .port = static_cast<unsigned>(port),
                .local_music_root =
                    music_root.isEmpty()
                        ? std::nullopt
                        : std::optional<std::string>{QFile::encodeName(music_root).toStdString()},
                .auto_connect = auto_connect,
            };
            if (auto_connect) {
                for (auto& profile : mpd_profiles_) {
                    profile.auto_connect = false;
                }
            }
            const auto existing_profile =
                std::ranges::find(mpd_profiles_, updated.id, &persistence::ConnectionProfile::id);
            if (existing_profile == mpd_profiles_.end()) {
                mpd_profiles_.push_back(std::move(updated));
            } else {
                *existing_profile = std::move(updated);
            }
            const auto profiles = mpd_profiles_;
            if (persistence_ == nullptr) {
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
                return;
            }
            persistence_->saveProfiles(profiles, [this, profiles, profile_id, host, port, password,
                                                  music_root](const QString& error) {
                if (!error.isEmpty()) {
                    statusBar()->showMessage(error, 5'000);
                    return;
                }
                mpd_profiles_ = profiles;
                mpd_controller_->probeProfile(profile_id, host, port, password, music_root);
            });
        });
    dialog->show();
}

void BenchMainWindow::autoConnectMpd() {
    const auto profile =
        std::ranges::find(mpd_profiles_, true, &persistence::ConnectionProfile::auto_connect);
    if (profile == mpd_profiles_.end()) {
        return;
    }
    const auto music_root = profile->local_music_root
                                ? QFile::decodeName(QByteArray{
                                      profile->local_music_root->data(),
                                      static_cast<qsizetype>(profile->local_music_root->size())})
                                : QString{};
    mpd_controller_->probeProfile(QString::fromStdString(profile->id.to_string()),
                                  displayText(profile->host), static_cast<int>(profile->port),
                                  QString{}, music_root);
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

QVariantList BenchMainWindow::selectedMpdQueueRows() const {
    QVariantList rows;
    if (mpd_queue_view_ == nullptr || mpd_queue_view_->selectionModel() == nullptr) {
        return rows;
    }
    auto selected = mpd_queue_view_->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    return rows;
}

QStringList BenchMainWindow::selectedMpdQueueUris() const {
    QStringList uris;
    const auto* model = qobject_cast<const quick::MpdQueueModel*>(mpd_queue_view_->model());
    if (model == nullptr) {
        return uris;
    }
    for (const auto& value : selectedMpdQueueRows()) {
        if (const auto uri = model->uriAt(value.toInt())) {
            uris.push_back(displayText(*uri));
        }
    }
    return uris;
}

void BenchMainWindow::refreshMpdPriorityMenu() {
    if (mpd_priority_menu_ == nullptr) {
        return;
    }
    const auto rows = selectedMpdQueueRows();
    const auto ready = !rows.isEmpty() && mpd_controller_->connected() &&
                       !mpd_controller_->commandBusy() &&
                       mpd_controller_->supportsCommand(QStringLiteral("prioid"));
    mpd_priority_menu_->setEnabled(ready);
    std::optional<unsigned> selected_priority;
    bool priorities_match = !rows.isEmpty();
    for (const auto& value : rows) {
        const auto priority = mpd_queue_view_->model()
                                  ->index(value.toInt(), 0)
                                  .data(quick::MpdQueueModel::PriorityRole);
        const auto numeric = priority.isValid() ? priority.toUInt() : 0U;
        if (!selected_priority) {
            selected_priority = numeric;
        } else if (*selected_priority != numeric) {
            priorities_match = false;
            break;
        }
    }
    for (auto* action : mpd_priority_menu_->actions()) {
        const QSignalBlocker blocker{action};
        action->setChecked(priorities_match && selected_priority &&
                           action->data().toUInt() == *selected_priority);
    }
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

void BenchMainWindow::playRow(ListTab& tab, const int row) {
    if (player_ == nullptr) {
        return;
    }
    const auto source = tab.model->source(row);
    if (source.raw_path.empty()) {
        return;
    }
    if (auto result = load_and_play(*player_, source); !result) {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
        return;
    }
    const auto id = QString::fromStdString(tab.document.id.to_string());
    if (playback_document_id_ != id) {
        if (auto* previous = tabForDocument(playback_document_id_); previous != nullptr) {
            previous->model->setCurrentSource({}, -1);
        }
    }
    playback_document_id_ = id;
    playback_row_ = row;
    playback_source_ = source;
    // A load was just dispatched; block auto-advance until the player state
    // leaves "ended" so the previous track's end cannot skip this one.
    advance_pending_ = true;
    last_requested_next_.reset();
    tab.model->setCurrentSource(source, row);
}

std::optional<std::pair<int, LocalTrackSource>>
BenchMainWindow::adjacentPlaybackRow(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    if (tab == nullptr || playback_source_.raw_path.empty()) {
        return std::nullopt;
    }
    const auto row = tab->model->rowOfSource(playback_source_, playback_row_);
    if (row < 0) {
        return std::nullopt;
    }
    const auto adjacent = row + direction;
    if (adjacent < 0 || adjacent >= tab->model->rowCount()) {
        return std::nullopt;
    }
    return std::make_pair(adjacent, tab->model->source(adjacent));
}

void BenchMainWindow::playAdjacent(const int direction) {
    auto* tab = tabForDocument(playback_document_id_);
    const auto next = adjacentPlaybackRow(direction);
    if (tab == nullptr || !next || player_ == nullptr) {
        return;
    }
    if (auto result = load_and_play(*player_, next->second); result) {
        playback_row_ = next->first;
        playback_source_ = next->second;
        advance_pending_ = true;
        last_requested_next_.reset();
        tab->model->setCurrentSource(next->second, next->first);
    } else {
        statusBar()->showMessage(
            QStringLiteral("Playback failed: %1").arg(displayText(result.error().message)), 5'000);
    }
}

void BenchMainWindow::togglePlayPause() {
    if (isMpdContext()) {
        mpd_controller_->playPause();
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (playerActive(snapshot.state)) {
        static_cast<void>(player_->pause());
    } else {
        static_cast<void>(player_->play());
    }
}

void BenchMainWindow::seekToMs(const qint64 position_ms) {
    if (isMpdContext()) {
        mpd_controller_->seekTo(position_ms);
        return;
    }
    if (player_ == nullptr) {
        return;
    }
    const auto snapshot = player_->snapshot();
    if (!snapshot.format || snapshot.format->sample_rate <= 0) {
        return;
    }
    static_cast<void>(player_->seek_to_sample(position_ms * snapshot.format->sample_rate / 1'000));
}

void BenchMainWindow::refreshMpdTransport() {
    const auto connected = mpd_controller_->connected();
    const auto command_ready = connected && !mpd_controller_->commandBusy();
    const auto has_queue = mpd_controller_->queueCount() > 0;
    const auto active = mpd_controller_->playing();
    previous_action_->setEnabled(command_ready && has_queue);
    next_action_->setEnabled(command_ready && has_queue);
    play_pause_action_->setEnabled(command_ready);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_pause_action_->setIcon(
        style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    stop_action_->setEnabled(command_ready && (active || mpd_controller_->paused()));

    if (connected) {
        now_playing_->setText(mpd_controller_->nowPlayingTitle());
        now_playing_context_->setText(mpd_controller_->nowPlayingDetail());
        now_playing_->setToolTip(QStringLiteral("MPD · %1").arg(mpd_controller_->status()));
        now_playing_context_->setToolTip(mpd_controller_->details());
    } else {
        now_playing_->setText(QStringLiteral("MPD not connected"));
        now_playing_context_->setText(QStringLiteral("File → Connect to MPD…"));
        now_playing_->setToolTip(mpd_controller_->status());
        now_playing_context_->setToolTip(mpd_controller_->details());
    }

    const auto position_ms = mpd_controller_->elapsedMs();
    const auto duration_ms = mpd_controller_->durationMs();
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(command_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(command_ready && mpd_controller_->volume() >= 0);
    if (!changing_volume_ && mpd_controller_->volume() >= 0) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(mpd_controller_->volume());
    }
    device_button_->setEnabled(connected);
    device_button_->setToolTip(
        QStringLiteral("MPD output: %1").arg(mpd_controller_->activeOutputName()));
    device_button_->setAccessibleDescription(mpd_controller_->activeOutputName());
}

void BenchMainWindow::refreshTransport() {
    if (isMpdContext()) {
        refreshMpdTransport();
        return;
    }
    if (player_ == nullptr) {
        for (auto* action : {previous_action_, play_pause_action_, stop_action_, next_action_}) {
            action->setEnabled(false);
        }
        seek_->setEnabled(false);
        volume_->setEnabled(false);
        device_button_->setEnabled(false);
        return;
    }
    const auto snapshot = player_->snapshot();
    // Observable for offscreen tests and diagnostics.
    setProperty("trackbench-player-state", static_cast<int>(snapshot.state));

    // A consumed gapless takeover moves the anchors and highlight without any
    // load; the engine already plays the next row.
    if (snapshot.chain_transitions != last_chain_transitions_) {
        last_chain_transitions_ = snapshot.chain_transitions;
        last_requested_next_.reset();
        if (auto* tab = tabForDocument(playback_document_id_);
            tab != nullptr && !snapshot.raw_path.empty()) {
            const auto transitioned_source = source_from_snapshot(snapshot);
            const auto row = tab->model->rowOfSource(transitioned_source, playback_row_ + 1);
            if (row >= 0) {
                playback_row_ = row;
                playback_source_ = transitioned_source;
                tab->model->setCurrentSource(transitioned_source, row);
            }
        }
    }
    setProperty("trackbench-player-position", static_cast<qlonglong>(snapshot.position_sample));
    setProperty("trackbench-player-buffered", static_cast<qlonglong>(snapshot.buffered_frames));
    setProperty("trackbench-player-buffer-capacity-ms",
                static_cast<qlonglong>(snapshot.configured_buffer.capacity.count()));
    setProperty("trackbench-player-active-buffer-capacity-ms",
                snapshot.active_buffer
                    ? static_cast<qlonglong>(snapshot.active_buffer->capacity.count())
                    : static_cast<qlonglong>(-1));
    setProperty("trackbench-player-buffer-pending",
                snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer);
    setProperty("trackbench-player-underruns", static_cast<qulonglong>(snapshot.underrun_count));
    setProperty("trackbench-player-callbacks",
                static_cast<qlonglong>(snapshot.output.callback_count));
    setProperty("trackbench-player-outputstate", static_cast<int>(snapshot.output.state));
    setProperty("trackbench-player-output-available", snapshot.output_target_available);
    setProperty("trackbench-player-output-suspended", snapshot.output_suspended);
    setProperty("trackbench-player-device-generation",
                static_cast<qulonglong>(snapshot.device_generation));
    setProperty("trackbench-player-default-output",
                snapshot.default_output_target ? displayText(*snapshot.default_output_target)
                                               : QString{});

    // List progression (ADR-0023): a finished track advances once to the next
    // row of its originating list; the end of the list stays "Ended".
    if (snapshot.state == audio::LocalAuditionState::ended) {
        // The guard stays set until the worker actually leaves "ended";
        // resetting it on dispatch would re-fire every timer tick while the
        // next source is still loading and race through the list.
        if (!advance_pending_) {
            advance_pending_ = true;
            if (const auto next = adjacentPlaybackRow(1)) {
                if (auto result = load_and_play(*player_, next->second); result) {
                    playback_row_ = next->first;
                    playback_source_ = next->second;
                    if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                        tab->model->setCurrentSource(next->second, next->first);
                    }
                }
            }
        }
    } else if (snapshot.state == audio::LocalAuditionState::empty) {
        if (!playback_document_id_.isEmpty()) {
            if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
                tab->model->setCurrentSource({}, -1);
            }
            playback_document_id_.clear();
            playback_row_ = -1;
            playback_source_ = {};
        }
        advance_pending_ = false;
    } else if (snapshot.state != audio::LocalAuditionState::loading) {
        advance_pending_ = false;
    }

    const auto error = snapshot.error ? displayText(snapshot.error->message) : QString{};
    if (!error.isEmpty() && error != last_player_error_) {
        last_player_error_ = error;
        statusBar()->showMessage(QStringLiteral("Playback failed: %1").arg(error), 5'000);
    } else if (error.isEmpty()) {
        last_player_error_.clear();
    }

    const auto monitor_error = snapshot.device_monitor_error
                                   ? displayText(snapshot.device_monitor_error->message)
                                   : QString{};
    if (!monitor_error.isEmpty() && monitor_error != last_device_monitor_error_) {
        last_device_monitor_error_ = monitor_error;
        statusBar()->showMessage(
            QStringLiteral("Audio device monitoring failed: %1").arg(monitor_error), 5'000);
    } else if (monitor_error.isEmpty()) {
        last_device_monitor_error_.clear();
    }
    const auto recovery_error = snapshot.output_recovery_error
                                    ? displayText(snapshot.output_recovery_error->message)
                                    : QString{};
    if (!recovery_error.isEmpty() && recovery_error != last_output_recovery_error_) {
        last_output_recovery_error_ = recovery_error;
        statusBar()->showMessage(
            QStringLiteral("Audio output recovery failed: %1").arg(recovery_error), 5'000);
    } else if (recovery_error.isEmpty()) {
        last_output_recovery_error_.clear();
    }
    if (last_device_generation_ != 0U) {
        if (selected_device_available_ && !snapshot.output_target_available) {
            statusBar()->showMessage(QStringLiteral("Audio output unavailable · playback paused"),
                                     5'000);
        } else if (!selected_device_available_ && snapshot.output_target_available &&
                   !snapshot.output_suspended) {
            statusBar()->showMessage(
                QStringLiteral("Audio output available again · press Play to resume"), 5'000);
        } else if (!snapshot.output_target && default_device_ &&
                   snapshot.default_output_target != default_device_) {
            statusBar()->showMessage(QStringLiteral("System audio output changed"), 5'000);
        }
    }

    // Keep the engine's queued continuation in sync with the next list row so
    // transitions are gapless. Re-requests are throttled: the engine drops
    // the queue on seeks and silently rejects format changes, and the drain
    // fallback below covers rejected continuations.
    if (snapshot.format.has_value() && snapshot.state != audio::LocalAuditionState::failed &&
        snapshot.state != audio::LocalAuditionState::empty &&
        snapshot.state != audio::LocalAuditionState::loading) {
        const auto next = adjacentPlaybackRow(1);
        const auto desired = next ? std::optional{next->second} : std::nullopt;
        const auto desired_marker = desired.value_or(LocalTrackSource{});
        const auto queued = queued_source_from_snapshot(snapshot);
        const bool changed = !last_requested_next_ || *last_requested_next_ != desired_marker;
        const bool stale = desired != queued && (!next_request_timer_.isValid() ||
                                                 next_request_timer_.elapsed() > 1'000);
        if (desired != queued && (changed || stale)) {
            if (!desired) {
                static_cast<void>(player_->clear_gapless_next());
            } else {
                static_cast<void>(queue_gapless(*player_, *desired));
            }
            last_requested_next_ = desired_marker;
            next_request_timer_.start();
        }
    }

    const bool active = playerActive(snapshot.state);
    const bool source_ready = snapshot.format.has_value() &&
                              snapshot.state != audio::LocalAuditionState::loading &&
                              snapshot.state != audio::LocalAuditionState::failed;
    previous_action_->setEnabled(adjacentPlaybackRow(-1).has_value());
    next_action_->setEnabled(adjacentPlaybackRow(1).has_value());
    play_pause_action_->setEnabled(source_ready && snapshot.output_target_available &&
                                   !snapshot.output_suspended);
    play_pause_action_->setText(active ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_pause_action_->setIcon(
        style()->standardIcon(active ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    stop_action_->setEnabled(source_ready);

    if (snapshot.state == audio::LocalAuditionState::empty) {
        now_playing_->clear();
        now_playing_context_->clear();
        now_playing_->setToolTip({});
        now_playing_context_->setToolTip({});
    } else {
        const auto slash = snapshot.raw_path.find_last_of('/');
        const auto name = slash == std::string::npos || slash + 1U >= snapshot.raw_path.size()
                              ? snapshot.raw_path
                              : snapshot.raw_path.substr(slash + 1U);
        const auto fallback = QString::fromStdString(core::escape_raw_path(name));
        auto label = fallback;
        auto context = QString{};
        if (auto* tab = tabForDocument(playback_document_id_); tab != nullptr) {
            const auto row = tab->model->rowOfSource(source_from_snapshot(snapshot), playback_row_);
            if (row >= 0) {
                const auto& track = tab->model->rows()[static_cast<std::size_t>(row)];
                const auto title = track.title.empty() ? fallback : displayText(track.title);
                if (!track.artist.empty()) {
                    label = QStringLiteral("%1 — %2").arg(displayText(track.artist), title);
                } else {
                    label = title;
                }
                QStringList details;
                if (!track.album.empty()) {
                    details.push_back(displayText(track.album));
                }
                if (!track.date.empty()) {
                    details.push_back(displayText(track.date));
                }
                context = details.join(QStringLiteral(" · "));
            }
        }
        now_playing_->setText(label);
        now_playing_context_->setText(context);
        const auto path_tooltip = QString::fromStdString(core::escape_raw_path(snapshot.raw_path));
        now_playing_->setToolTip(path_tooltip);
        now_playing_context_->setToolTip(context.isEmpty() ? path_tooltip : context);
    }

    qint64 position_ms = 0;
    qint64 duration_ms = 0;
    if (snapshot.format && snapshot.format->sample_rate > 0) {
        position_ms = snapshot.position_sample * 1'000 / snapshot.format->sample_rate;
        if (snapshot.end_sample) {
            duration_ms = *snapshot.end_sample * 1'000 / snapshot.format->sample_rate;
        }
    }
    elapsed_->setText(formatTime(position_ms));
    duration_->setText(formatTime(duration_ms));
    const auto bounded = std::clamp<qint64>(duration_ms, 0, std::numeric_limits<int>::max());
    seek_->setEnabled(source_ready && bounded > 0);
    seek_->setRange(0, static_cast<int>(bounded));
    if (!seeking_) {
        const QSignalBlocker blocker{seek_};
        seek_->setValue(
            static_cast<int>(std::clamp<qint64>(position_ms, 0, std::numeric_limits<int>::max())));
    }
    volume_->setEnabled(true);
    if (!changing_volume_) {
        const QSignalBlocker blocker{volume_};
        volume_->setValue(snapshot.volume_percent);
    }

    std::vector<std::pair<std::string, std::string>> choices;
    choices.reserve(snapshot.devices.size());
    for (const auto& device : snapshot.devices) {
        choices.emplace_back(device.name, device.description);
    }
    const bool device_menu_changed = choices != device_choices_;
    const bool selection_changed = snapshot.output_target != selected_device_;
    const bool availability_changed =
        snapshot.output_target_available != selected_device_available_;
    const bool default_changed = snapshot.default_output_target != default_device_;
    device_choices_ = std::move(choices);
    selected_device_ = snapshot.output_target;
    default_device_ = snapshot.default_output_target;
    selected_device_available_ = snapshot.output_target_available;
    last_device_generation_ = snapshot.device_generation;
    if (device_menu_changed || selection_changed || availability_changed || default_changed) {
        rebuildDeviceMenu();
    }
    QString device_label = QStringLiteral("System default");
    if (selected_device_) {
        const auto found = std::ranges::find(device_choices_, *selected_device_,
                                             &std::pair<std::string, std::string>::first);
        device_label = found == device_choices_.end()
                           ? displayText(*selected_device_)
                           : displayText(found->second.empty() ? found->first : found->second);
    } else if (default_device_) {
        const auto found = std::ranges::find(device_choices_, *default_device_,
                                             &std::pair<std::string, std::string>::first);
        const auto default_label =
            found == device_choices_.end()
                ? displayText(*default_device_)
                : displayText(found->second.empty() ? found->first : found->second);
        device_label += QStringLiteral(" — %1").arg(default_label);
    }
    if (!snapshot.output_target_available) {
        device_label += QStringLiteral(" (unavailable)");
    }
    device_button_->setEnabled(true);
    const bool buffer_pending =
        snapshot.active_buffer && *snapshot.active_buffer != snapshot.configured_buffer;
    auto audio_tooltip =
        QStringLiteral("Audio output: %1\nBuffer: %2 · %3 ms capacity · %4 ms start%5\n"
                       "Underruns: %6")
            .arg(device_label)
            .arg(bufferProfileLabel(selected_buffer_profile_))
            .arg(snapshot.configured_buffer.capacity.count())
            .arg(snapshot.configured_buffer.start_threshold.count())
            .arg(buffer_pending ? QStringLiteral(" · applies next track") : QString{})
            .arg(snapshot.underrun_count);
    if (!snapshot.output_target_available) {
        audio_tooltip += QStringLiteral("\nPlayback is paused until an output is available");
    } else if (snapshot.output_suspended) {
        audio_tooltip += QStringLiteral("\nReconnecting the audio output");
    }
    if (snapshot.output.node_id) {
        audio_tooltip += QStringLiteral("\nPipeWire node: %1").arg(*snapshot.output.node_id);
    }
    device_button_->setToolTip(audio_tooltip);
    device_button_->setAccessibleDescription(device_label);
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
