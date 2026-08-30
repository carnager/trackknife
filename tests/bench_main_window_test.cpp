// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_list_model.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/operation_journal.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/line_slider.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::bench {

namespace {

constexpr std::uint32_t wave_sample_rate = 44'100U;
// The live progression test reaches the user's default PipeWire sink. Keep its
// non-silent probe roughly 32 dB below the previous level so it is detectable
// by the decoder without being an intrusive test sound.
constexpr std::int32_t test_wave_peak = 256;

void append_u16(std::vector<unsigned char>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<unsigned char>& bytes, const std::uint32_t value) {
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<unsigned char>((value >> (byte * 8U)) & 0xFFU));
    }
}

// Writes a real 16-bit mono PCM WAV with a non-silent deterministic pattern so
// playback has actual audio to drain.
void write_wave(const QString& path, const std::uint32_t frames) {
    std::vector<unsigned char> bytes;
    bytes.reserve(44U + frames * 2U);
    const auto data_bytes = frames * 2U;
    for (const char character : {'R', 'I', 'F', 'F'}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, 36U + data_bytes);
    for (const char character : {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, wave_sample_rate);
    append_u32(bytes, wave_sample_rate * 2U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
    for (const char character : {'d', 'a', 't', 'a'}) {
        bytes.push_back(static_cast<unsigned char>(character));
    }
    append_u32(bytes, data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const auto sample = static_cast<std::int32_t>((frame * 37U) % static_cast<std::uint32_t>(
                                                                          2 * test_wave_peak + 1)) -
                            test_wave_peak;
        append_u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample)));
    }
    std::ofstream output{QFile::encodeName(path).toStdString(), std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

bool materialize_audio_fixture(const QString& encoded_name, const QString& output_path) {
    QFile source{QStringLiteral(TRACKKNIFE_AUDIO_FIXTURE_DIR) + QLatin1Char('/') + encoded_name};
    if (!source.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto decoded = QByteArray::fromBase64(source.readAll());
    QFile output{output_path};
    return !decoded.isEmpty() && output.open(QIODevice::WriteOnly) &&
           output.write(decoded) == decoded.size();
}

} // namespace

class BenchMainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void transportUsesStackedNowPlayingAndCompactDeviceButton();
    void unifiesMpdAndLocalAuthoritiesInOneWorkspace();
    void mpdSearchProjectsControllerResults();
    void mpdQueueAndLibraryMenusExposeServerActions();
    void playbackBufferProfilesPersistAndExposeDiagnostics();
    void statusBarSummarizesTrackSelection();
    void committedMetadataRefreshesDuplicatesAndPreservesCueOverlay();
    void metadataReadyPlanAppliesAndRefreshesHistory();
    void metadataApplyCancellationPreservesDraftForFreshPreview();
    void metadataTransformationChainPreviewsAndStagesOneUndo();
    void metadataOperationHistoryUndoesRetainedBackup();
    void metadataStartupPresentsReconciliation();
    void folderDiscoveryAdmitsWave64();
    void contextMenusTargetSelectionsListsAndFolders();
    void panelLayoutPersistsAndPreservesFutureState();
    void trackViewLayoutMatchesGroupedQueueAndPersists();
    void localReorderPreservesVisibleRowGeometry();
    void noncontiguousLocalReorderKeepsTheModelResetBoundaryIntact();
    void persistsPinnedDuplicatedAndDirtyTabs();
    void richMetadataValuesAndIdentitiesSurviveListRestart();
    void metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits();
    void cueSheetsExpandIntoPersistentSegmentRows();
    void containerChaptersExpandIntoPersistentSegmentRows();
    void codecNativeSubsongsExpandAndPersistDecoderSelections();
    void autoAdvancesOncePerFinishedTrack();

  private:
    QTemporaryDir settings_directory_;
};

void BenchMainWindowTest::initTestCase() {
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("trackbench-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
    QStandardPaths::setTestModeEnabled(true);
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

void BenchMainWindowTest::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QSettings settings;
    settings.clear();
    settings.sync();
    QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.removeRecursively();
}

void BenchMainWindowTest::transportUsesStackedNowPlayingAndCompactDeviceButton() {
    BenchMainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);

    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("bench-now-playing"));
    auto* now_playing_context =
        window.findChild<QLabel*>(QStringLiteral("bench-now-playing-context"));
    auto* seek = window.findChild<QSlider*>(QStringLiteral("bench-seek"));
    auto* volume = window.findChild<QSlider*>(QStringLiteral("bench-volume"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    auto* app_menu = window.findChild<QToolButton*>(QStringLiteral("bench-main-menu"));
    auto* header = window.findChild<QWidget*>(QStringLiteral("bench-player-header"));
    auto* transport = window.findChild<QWidget*>(QStringLiteral("bench-transport-buttons"));

    QVERIFY(now_playing != nullptr);
    QVERIFY(now_playing_context != nullptr);
    QVERIFY(seek != nullptr);
    QVERIFY(volume != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(app_menu != nullptr);
    QVERIFY(header != nullptr);
    QVERIFY(transport != nullptr);
    QVERIFY(window.findChild<QComboBox*>(QStringLiteral("bench-device")) == nullptr);
    QCOMPARE(now_playing->accessibleName(), QStringLiteral("Current artist and title"));
    QCOMPARE(now_playing_context->accessibleName(), QStringLiteral("Current album and date"));
    QCOMPARE(seek->accessibleName(), QStringLiteral("Playback position"));
    QVERIFY(dynamic_cast<ui::LineSlider*>(seek) != nullptr);
    QVERIFY(dynamic_cast<ui::LineSlider*>(volume) != nullptr);
    QCOMPARE(device->accessibleName(), QStringLiteral("Audio output device"));
    QCOMPARE(app_menu->accessibleName(), QStringLiteral("Application menu"));
    QCOMPARE(app_menu->text(), QStringLiteral("☰"));
    QCOMPARE(app_menu->popupMode(), QToolButton::InstantPopup);
    QVERIFY(app_menu->menu() != nullptr);
    QCOMPARE(app_menu->menu()->objectName(), QStringLiteral("bench-main-menu-popup"));
    QCOMPARE(app_menu->menu()->actions().size(), 4);
    QVERIFY(!window.menuBar()->isVisible());
    QCOMPARE(device->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(!device->icon().isNull());
    QVERIFY(device->menu() != nullptr);
    QCOMPARE(device->menu()->objectName(), QStringLiteral("bench-device-menu"));
    QVERIFY(!device->menu()->actions().empty());
    QCOMPARE(device->menu()->actions().front()->text(), QStringLiteral("System default"));
    QVERIFY(window.property("trackbench-player-output-available").isValid());
    QVERIFY(window.property("trackbench-player-output-suspended").isValid());
    QVERIFY(window.property("trackbench-player-device-generation").isValid());
    QVERIFY(window.property("trackbench-player-default-output").isValid());
    QCOMPARE(transport->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly).size(),
             4);

    const QRect label_geometry{now_playing->mapTo(&window, QPoint{}), now_playing->size()};
    const QRect context_geometry{now_playing_context->mapTo(&window, QPoint{}),
                                 now_playing_context->size()};
    const QRect seek_geometry{seek->mapTo(&window, QPoint{}), seek->size()};
    const QRect volume_geometry{volume->mapTo(&window, QPoint{}), volume->size()};
    const QRect device_geometry{device->mapTo(&window, QPoint{}), device->size()};
    QVERIFY(label_geometry.bottom() <= context_geometry.top());
    QVERIFY(context_geometry.bottom() <= seek_geometry.top());
    QCOMPARE(label_geometry.center().x(), seek_geometry.center().x());
    QCOMPARE(context_geometry.center().x(), seek_geometry.center().x());
    QCOMPARE(volume_geometry.center().y(), seek_geometry.center().y());
    QCOMPARE(device_geometry.center().y(), seek_geometry.center().y());
    QVERIFY(header->height() <= 72);
}

void BenchMainWindowTest::unifiesMpdAndLocalAuthoritiesInOneWorkspace() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* mpd_queue = dynamic_cast<ui::QueueTableView*>(
        window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue")));
    auto* folder_view = window.findChild<QTreeView*>(QStringLiteral("bench-folder-tree"));
    auto* mpd_library = window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library"));
    auto* heading = window.findChild<QLabel*>(QStringLiteral("bench-folders-heading"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* plain = window.findChild<QAction*>(QStringLiteral("action-track-layout-plain"));
    auto* copy_layout = window.findChild<QAction*>(QStringLiteral("action-copy-track-layout"));
    auto* buffer_menu = window.findChild<QMenu*>(QStringLiteral("bench-buffer-menu"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("bench-mpd-search"));
    auto* search_surface = window.findChild<QWidget*>(QStringLiteral("bench-mpd-search-surface"));
    auto* repeat = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-repeat"));
    auto* random = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-random"));
    auto* single = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-single"));
    auto* consume = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-consume"));
    auto* replaygain = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-replaygain"));
    QVERIFY(tabs != nullptr);
    QVERIFY(mpd_queue != nullptr);
    QVERIFY(folder_view != nullptr);
    QVERIFY(mpd_library != nullptr);
    QVERIFY(heading != nullptr);
    QVERIFY(properties != nullptr);
    QVERIFY(plain != nullptr);
    QVERIFY(copy_layout != nullptr);
    QVERIFY(buffer_menu != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(search != nullptr);
    QVERIFY(search_surface != nullptr);
    QVERIFY(repeat != nullptr);
    QVERIFY(random != nullptr);
    QVERIFY(single != nullptr);
    QVERIFY(consume != nullptr);
    QVERIFY(replaygain != nullptr);
    QCOMPARE(replaygain->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QVERIFY(replaygain->text().startsWith(QStringLiteral("RG: ")));
    QVERIFY(replaygain->menu() != nullptr);
    QCOMPARE(replaygain->menu()->actions().size(), 4);
    QTRY_COMPARE(tabs->count(), 2);
    auto* local_queue = dynamic_cast<ui::QueueTableView*>(tabs->currentWidget());
    QVERIFY(local_queue != nullptr);
    QVERIFY(local_queue != mpd_queue);
    QCOMPARE(mpd_queue->model()->columnCount(), local_column_count);
    QCOMPARE(local_queue->model()->columnCount(), local_column_count);
    QCOMPARE(mpd_queue->property("trackknife-hover-row").toInt(), -1);
    QCOMPARE(local_queue->property("trackknife-hover-row").toInt(), -1);
    for (int column = 0; column < local_column_count; ++column) {
        QCOMPARE(mpd_queue->model()->headerData(column, Qt::Horizontal).toString(),
                 local_queue->model()->headerData(column, Qt::Horizontal).toString());
        QCOMPARE(mpd_queue->isColumnHidden(column), local_queue->isColumnHidden(column));
        QCOMPARE(mpd_queue->horizontalHeader()->visualIndex(column),
                 local_queue->horizontalHeader()->visualIndex(column));
        QCOMPARE(mpd_queue->columnWidth(column), local_queue->columnWidth(column));
    }
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(mpd_queue->itemDelegate()) != nullptr);
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(local_queue->itemDelegate()) != nullptr);
    QVERIFY(tabs->cornerWidget(Qt::TopRightCorner) == nullptr);
    QCOMPARE(search->parentWidget(), tabs);
    QVERIFY(!search->isVisible());
    QVERIFY(!repeat->isVisible());
    QVERIFY(!random->isVisible());
    QVERIFY(!single->isVisible());
    QVERIFY(!consume->isVisible());

    const auto mpd_index = tabs->indexOf(mpd_queue);
    QVERIFY(mpd_index >= 0);
    QCOMPARE(tabs->tabText(mpd_index), QStringLiteral("MPD Queue"));
    auto* mpd_close = tabs->tabBar()->tabButton(mpd_index, QTabBar::RightSide);
    QVERIFY(mpd_close == nullptr || !mpd_close->isVisible());
    tabs->setCurrentWidget(mpd_queue);
    QTRY_COMPARE(window.property("trackbench-active-authority").toString(), QStringLiteral("mpd"));
    QCOMPARE(heading->text(), QStringLiteral("MPD Library"));
    QVERIFY(mpd_library->isVisible());
    QVERIFY(!folder_view->isVisible());
    QVERIFY(!properties->isEnabled());
    QVERIFY(!buffer_menu->isEnabled());
    QCOMPARE(device->accessibleName(), QStringLiteral("MPD output"));
    QVERIFY(plain->isEnabled());
    QVERIFY(search->isVisible());
    QVERIFY(repeat->isVisible());
    QVERIFY(random->isVisible());
    QVERIFY(single->isVisible());
    QVERIFY(consume->isVisible());
    QCOMPARE(single->text(), QStringLiteral("1"));
    QCOMPARE(consume->text(), QStringLiteral("C"));
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(search_surface->isVisible());
    QVERIFY(tabs->cornerWidget(Qt::TopRightCorner) == nullptr);
    QCOMPARE(search->parentWidget(), tabs);
    const auto field_right = search->mapTo(tabs, QPoint{search->width(), 0}).x();
    QVERIFY2(
        field_right <= tabs->width(),
        qPrintable(
            QStringLiteral("search right %1 exceeds tabs width %2; field geometry %3,%4 %5x%6; "
                           "parent %7 geometry %8,%9 %10x%11")
                .arg(field_right)
                .arg(tabs->width())
                .arg(search->x())
                .arg(search->y())
                .arg(search->width())
                .arg(search->height())
                .arg(search->parentWidget() != nullptr
                         ? search->parentWidget()->metaObject()->className()
                         : "none")
                .arg(search->parentWidget() != nullptr ? search->parentWidget()->x() : -1)
                .arg(search->parentWidget() != nullptr ? search->parentWidget()->y() : -1)
                .arg(search->parentWidget() != nullptr ? search->parentWidget()->width() : -1)
                .arg(search->parentWidget() != nullptr ? search->parentWidget()->height() : -1)));

    plain->trigger();
    copy_layout->trigger();
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(mpd_queue->itemDelegate()) == nullptr);
    tabs->setCurrentWidget(local_queue);
    QTRY_COMPARE(window.property("trackbench-active-authority").toString(),
                 QStringLiteral("local"));
    QCOMPARE(heading->text(), QStringLiteral("Folders"));
    QVERIFY(folder_view->isVisible());
    QVERIFY(!mpd_library->isVisible());
    QVERIFY(buffer_menu->isEnabled());
    QCOMPARE(device->accessibleName(), QStringLiteral("Audio output device"));
    QVERIFY(qobject_cast<ui::QueueItemDelegate*>(local_queue->itemDelegate()) == nullptr);
    QVERIFY(!search->isVisible());
    QVERIFY(!search_surface->isVisible());
    QVERIFY(!repeat->isVisible());
    QVERIFY(!random->isVisible());
    QVERIFY(!single->isVisible());
    QVERIFY(!consume->isVisible());
}

void BenchMainWindowTest::mpdSearchProjectsControllerResults() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* mpd_queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    auto* field = window.findChild<QLineEdit*>(QStringLiteral("bench-mpd-search"));
    auto* surface = window.findChild<QWidget*>(QStringLiteral("bench-mpd-search-surface"));
    auto* results = window.findChild<QTableView*>(QStringLiteral("bench-mpd-search-results"));
    auto* result_model = window.findChild<quick::MpdSearchResultModel*>();
    auto* controller = window.findChild<quick::MpdProbeController*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(mpd_queue != nullptr);
    QVERIFY(field != nullptr);
    QVERIFY(surface != nullptr);
    QVERIFY(results != nullptr);
    QVERIFY(result_model != nullptr);
    QVERIFY(controller != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    tabs->setCurrentWidget(mpd_queue);

    auto* library = qobject_cast<quick::MpdQueueModel*>(controller->libraryModel());
    QVERIFY(library != nullptr);
    mpd::Metadata metadata{{
        {"Artist", "Search Artist"},
        {"AlbumArtist", "Search Artist"},
        {"Album", "Search Album"},
        {"Date", "2026"},
        {"Track", "1"},
        {"Title", "Search Result"},
    }};
    library->replaceTracks({mpd::Track{
        .uri = "search/result.flac",
        .metadata = std::move(metadata),
        .musicbrainz = {},
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    field->setText(QStringLiteral("Search"));
    QVERIFY(QMetaObject::invokeMethod(controller, "searchFinished", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Search")), Q_ARG(bool, true)));
    QTRY_VERIFY(result_model->firstResultRow() >= 0);
    const auto first = result_model->firstResultRow();
    QCOMPARE(result_model->index(first, 1).data().toString(), QStringLiteral("Search Result"));
    QCOMPARE(result_model->index(first, 4).data(Qt::ToolTipRole).toString(),
             QStringLiteral("Append to queue (Enter)"));

    QSignalSpy artwork_requests{result_model, &quick::MpdSearchResultModel::artworkRequested};
    result_model->replaceTracks({mpd::Track{
        .uri = "search/album-cover.flac",
        .metadata = mpd::Metadata{{
            {"Artist", "Cover Artist"},
            {"AlbumArtist", "Cover Artist"},
            {"Album", "Cover Album"},
            {"Date", "2026"},
            {"Track", "1"},
            {"Title", "Covered Track"},
        }},
        .musicbrainz = {},
        .queue_id = std::nullopt,
        .queue_position = std::nullopt,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    }});
    QTRY_VERIFY(!artwork_requests.isEmpty());
    const auto album_row = result_model->firstResultRow();
    QVERIFY(album_row >= 0);
    QImage cover{8, 8, QImage::Format_ARGB32_Premultiplied};
    cover.fill(Qt::blue);
    result_model->acceptArtwork(artwork_requests.front().front().toULongLong(), cover);
    QVERIFY(!result_model->index(album_row, 0).data(Qt::DecorationRole).value<QImage>().isNull());
    QCOMPARE(results->iconSize(), QSize(24, 24));
    QCOMPARE(results->rowHeight(album_row), 30);
    QVERIFY(!results->wordWrap());
    QCOMPARE(results->textElideMode(), Qt::ElideRight);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(surface->isVisible());
    results->viewport()->repaint();
    const auto album_cell = results->visualRect(result_model->index(album_row, 0));
    QVERIFY(!album_cell.isEmpty());
    const auto album_render = results->viewport()->grab(album_cell).toImage();
    auto blue_left = album_render.width();
    auto blue_right = -1;
    auto blue_top = album_render.height();
    auto blue_bottom = -1;
    for (int y = 0; y < album_render.height(); ++y) {
        for (int x = 0; x < album_render.width(); ++x) {
            if (album_render.pixelColor(x, y) == QColor(Qt::blue)) {
                blue_left = std::min(blue_left, x);
                blue_right = std::max(blue_right, x);
                blue_top = std::min(blue_top, y);
                blue_bottom = std::max(blue_bottom, y);
            }
        }
    }
    QVERIFY(blue_right >= blue_left);
    QVERIFY(blue_bottom >= blue_top);
    const auto painted_cover_width = blue_right - blue_left + 1;
    const auto painted_cover_height = blue_bottom - blue_top + 1;
    QVERIFY(painted_cover_width <= album_render.height());
    QVERIFY(painted_cover_height <= album_render.height());
    QVERIFY(std::abs(painted_cover_width - painted_cover_height) <= 1);
    results->setCurrentIndex(result_model->index(album_row, 1));
    results->setFocus();
    QTRY_VERIFY(surface->isVisible());
    QTest::keyClick(results, Qt::Key_Right);
    QCOMPARE(results->currentIndex().column(),
             quick::MpdSearchResultModel::first_action_column + 1);
    QTest::keyClick(results, Qt::Key_Left);
    QCOMPARE(results->currentIndex().column(), quick::MpdSearchResultModel::first_action_column);
    QTest::keyClick(results, Qt::Key_Left);
    QCOMPARE(results->currentIndex().column(), quick::MpdSearchResultModel::first_action_column);
    QTest::keyClick(results, Qt::Key_Right);
    QCOMPARE(results->currentIndex().column(),
             quick::MpdSearchResultModel::first_action_column + 1);
    QTest::keyClick(results, Qt::Key_Right);
    QCOMPARE(results->currentIndex().column(),
             quick::MpdSearchResultModel::first_action_column + 2);
    QTest::keyClick(results, Qt::Key_Right);
    QCOMPARE(results->currentIndex().column(),
             quick::MpdSearchResultModel::first_action_column + 2);
    mpd_queue->setFocus();
    QTRY_VERIFY(!surface->isVisible());
}

void BenchMainWindowTest::mpdQueueAndLibraryMenusExposeServerActions() {
    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* queue = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    auto* priority_menu = window.findChild<QMenu*>(QStringLiteral("bench-mpd-priority-menu"));
    auto* library = dynamic_cast<ui::ServerLibraryTreeView*>(
        window.findChild<QTreeView*>(QStringLiteral("bench-mpd-library")));
    auto* library_model = window.findChild<ui::ServerLibraryTreeModel*>();
    auto* library_menu = window.findChild<QMenu*>(QStringLiteral("bench-mpd-library-context-menu"));
    auto* replaygain = window.findChild<QToolButton*>(QStringLiteral("bench-mpd-replaygain"));
    QVERIFY(tabs != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(track_menu != nullptr);
    QVERIFY(priority_menu != nullptr);
    QVERIFY(library != nullptr);
    QVERIFY(library_model != nullptr);
    QVERIFY(library_menu != nullptr);
    QVERIFY(replaygain != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    tabs->setCurrentWidget(queue);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(controller->queueModel());
    QVERIFY(queue_model != nullptr);
    queue_model->replaceTracks({mpd::Track{
        .uri = "queue/prioritized.flac",
        .metadata = mpd::Metadata{{{"Title", "Prioritized"}}},
        .musicbrainz = {},
        .queue_id = 17U,
        .queue_position = 0U,
        .duration = std::chrono::milliseconds{180'000},
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = 192U,
        .unknown_structural_pairs = {},
    }});
    queue->scrollTo(queue_model->index(0, 0));
    const auto queue_position = queue->visualRect(queue_model->index(0, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(queue, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, queue_position)));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-add-next-selection"))));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-append-selection"))));
    QVERIFY(track_menu->actions().contains(
        window.findChild<QAction*>(QStringLiteral("action-mpd-crop-selection"))));
    QVERIFY(track_menu->actions().contains(priority_menu->menuAction()));
    auto* high = window.findChild<QAction*>(QStringLiteral("action-mpd-queue-priority-192"));
    QVERIFY(high != nullptr);
    QVERIFY(high->isChecked());
    QVERIFY(!priority_menu->isEnabled()); // Disconnected capability gate.
    track_menu->close();

    QCOMPARE(replaygain->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(replaygain->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QCOMPARE(replaygain->accessibleName(), QStringLiteral("MPD ReplayGain mode"));

    QSignalSpy root_requests{library_model, &ui::ServerLibraryTreeModel::rootRequested};
    library_model->reload();
    QTRY_COMPARE(root_requests.size(), 1);
    const auto token = root_requests.front().at(0).toULongLong();
    const auto tag = root_requests.front().at(1).toString();
    library_model->acceptRoot(token, tag, {QStringLiteral("Library Artist")}, {});
    const auto root = library_model->index(0, 0);
    QVERIFY(root.isValid());
    QVERIFY(root.flags().testFlag(Qt::ItemIsDragEnabled));
    QVERIFY(library->dragEnabled());
    QVERIFY(dynamic_cast<ui::ServerLibraryTreeDelegate*>(library->itemDelegate()) != nullptr);
    library->scrollTo(root);
    QTRY_VERIFY(!library->visualRect(root).isEmpty());

    QSignalSpy branch_requests{library_model, &ui::ServerLibraryTreeModel::branchRequested};
    library_model->fetchMore(root);
    QTRY_COMPARE(branch_requests.size(), 1);
    const auto branch_token = branch_requests.front().front().toULongLong();
    library_model->acceptBranch(branch_token,
                                {mpd::Track{
                                    .uri = "library/artist/album/01.flac",
                                    .metadata = mpd::Metadata{{
                                        {"Artist", "Library Artist"},
                                        {"AlbumArtist", "Library Artist"},
                                        {"Album", "Library Album"},
                                        {"Date", "2026"},
                                        {"Track", "1"},
                                        {"Title", "Library Track"},
                                    }},
                                    .musicbrainz = {},
                                    .queue_id = std::nullopt,
                                    .queue_position = std::nullopt,
                                    .duration = std::chrono::milliseconds{180'000},
                                    .last_modified = std::nullopt,
                                    .audio_format = std::nullopt,
                                    .priority = std::nullopt,
                                    .unknown_structural_pairs = {},
                                }},
                                {});
    QVERIFY(library_model->rowCount(root) > 0);
    QVERIFY(!library->isExpanded(root));
    QStyleOptionViewItem branch_option;
    branch_option.initFrom(library);
    branch_option.rect = library->visualRect(root);
    const auto disclosure = library->style()->subElementRect(QStyle::SE_TreeViewDisclosureItem,
                                                             &branch_option, library);
    QVERIFY(!disclosure.isEmpty());
    QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier, disclosure.center());
    QTRY_VERIFY(library->isExpanded(root));
    QCoreApplication::processEvents();
    QVERIFY(library->isExpanded(root)); // Disclosure click toggles exactly once.

    const auto library_position = library->visualRect(root).center();
    QVERIFY(QMetaObject::invokeMethod(library, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, library_position)));
    QCOMPARE(library_menu->actions().at(0)->text(), QStringLiteral("Append to live queue"));
    QCOMPARE(library_menu->actions().at(1)->text(), QStringLiteral("Insert next in live queue"));
    QCOMPARE(library_menu->actions().at(2)->text(), QStringLiteral("Replace queue and play"));
    library_menu->close();
}

void BenchMainWindowTest::playbackBufferProfilesPersistAndExposeDiagnostics() {
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/buffer-profile"), QStringLiteral("responsive"));
        settings.sync();
    }

    BenchMainWindow window;
    window.show();
    QCoreApplication::processEvents();

    auto* menu = window.findChild<QMenu*>(QStringLiteral("bench-buffer-menu"));
    auto* responsive = window.findChild<QAction*>(QStringLiteral("action-buffer-responsive"));
    auto* balanced = window.findChild<QAction*>(QStringLiteral("action-buffer-balanced"));
    auto* resilient = window.findChild<QAction*>(QStringLiteral("action-buffer-resilient"));
    auto* custom = window.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    auto* device = window.findChild<QToolButton*>(QStringLiteral("bench-device"));
    QVERIFY(menu != nullptr);
    QVERIFY(responsive != nullptr);
    QVERIFY(balanced != nullptr);
    QVERIFY(resilient != nullptr);
    QVERIFY(custom != nullptr);
    QVERIFY(device != nullptr);
    QVERIFY(responsive->isChecked());
    QVERIFY(!balanced->isChecked());
    QCOMPARE(responsive->toolTip(), QStringLiteral("250 ms capacity; playback starts at 50 ms"));
    QCOMPARE(custom->text(), QStringLiteral("Custom…"));
    QTRY_COMPARE(window.property("trackbench-player-buffer-capacity-ms").toLongLong(), 250);
    QCOMPARE(window.property("trackbench-player-active-buffer-capacity-ms").toLongLong(), -1);
    QVERIFY(!window.property("trackbench-player-buffer-pending").toBool());
    QCOMPARE(window.property("trackbench-player-underruns").toULongLong(), 0ULL);

    resilient->trigger();
    QVERIFY(resilient->isChecked());
    QTRY_COMPARE(window.property("trackbench-player-buffer-capacity-ms").toLongLong(), 2'000);
    QTRY_VERIFY(device->toolTip().contains(QStringLiteral("Resilient")));
    QVERIFY(device->toolTip().contains(QStringLiteral("Underruns: 0")));

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("resilient"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 2'000);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 250);

    QTimer::singleShot(0, [] {
        auto* dialog = QApplication::activeModalWidget();
        QVERIFY(dialog != nullptr);
        auto* capacity = dialog->findChild<QSpinBox*>(QStringLiteral("bench-buffer-capacity"));
        auto* threshold =
            dialog->findChild<QSpinBox*>(QStringLiteral("bench-buffer-start-threshold"));
        QVERIFY(capacity != nullptr);
        QVERIFY(threshold != nullptr);
        capacity->setValue(1'234);
        threshold->setValue(234);
        auto* custom_dialog = qobject_cast<QDialog*>(dialog);
        QVERIFY(custom_dialog != nullptr);
        custom_dialog->accept();
    });
    custom->trigger();
    QVERIFY(custom->isChecked());
    QTRY_COMPARE(window.property("trackbench-player-buffer-capacity-ms").toLongLong(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-profile")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-capacity-ms")).toInt(), 1'234);
    QCOMPARE(settings.value(QStringLiteral("playback/buffer-start-threshold-ms")).toInt(), 234);
    QVERIFY(window.close());

    BenchMainWindow restored;
    restored.show();
    auto* restored_custom = restored.findChild<QAction*>(QStringLiteral("action-buffer-custom"));
    QVERIFY(restored_custom != nullptr);
    QVERIFY(restored_custom->isChecked());
    QTRY_COMPARE(restored.property("trackbench-player-buffer-capacity-ms").toLongLong(), 1'234);
}

void BenchMainWindowTest::statusBarSummarizesTrackSelection() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("selection-first.wav"));
    const auto second_path = media.filePath(QStringLiteral("selection-second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({first_raw, second_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("bench-selection-status"));
    QVERIFY(tabs != nullptr);
    QVERIFY(status != nullptr);
    QCOMPARE(status->accessibleName(), QStringLiteral("Selected track information"));
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);

    LocalTrackRow first_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "First title",
        .artist = "First artist",
        .album = "First album",
        .album_artist = {},
        .date = "2026",
        .track_number = {},
        .duration_ms = 61'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    LocalTrackRow second_metadata{
        .raw_path = {},
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Second title",
        .artist = "Second artist",
        .album = "Second album",
        .album_artist = {},
        .date = "2025",
        .track_number = {},
        .duration_ms = 120'000,
        .metadata = {},
        .source_revision = std::nullopt,
    };
    QVERIFY(model->applyMetadata(first_raw, 0, std::move(first_metadata)));
    QVERIFY(model->applyMetadata(second_raw, 1, std::move(second_metadata)));

    view->selectionModel()->select(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QCOMPARE(status->text(),
             QStringLiteral("First artist — First title · First album (2026) · 1:01"));
    QCOMPARE(status->toolTip(), QString::fromStdString(core::escape_raw_path(first_raw)));

    view->selectionModel()->select(model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(status->text(), QStringLiteral("2 tracks selected · 3:01 total"));

    view->clearSelection();
    QCOMPARE(status->text(), QStringLiteral("No tracks selected"));
}

void BenchMainWindowTest::committedMetadataRefreshesDuplicatesAndPreservesCueOverlay() {
    const std::string source{"/music/shared.flac"};
    const auto field = [](std::string canonical_name, std::string value,
                          const metadata::FieldProvenance provenance) {
        return metadata::MetadataField{
            .canonical_name = canonical_name,
            .native_name = canonical_name,
            .values = {std::move(value)},
            .qualifier = {},
            .provenance = provenance,
        };
    };
    LocalTrackRow whole{
        .raw_path = source,
        .logical_reference = std::nullopt,
        .selection = {},
        .segment = std::nullopt,
        .title = "Old",
        .artist = {},
        .album = "Old album",
        .album_artist = {},
        .date = {},
        .track_number = {},
        .duration_ms = 1'000,
        .metadata =
            metadata::MetadataDocument{
                .fields = {field("title", "Old", metadata::FieldProvenance::embedded),
                           field("album", "Old album", metadata::FieldProvenance::embedded)},
                .unsupported_native_objects = {}},
        .source_revision = std::nullopt,
        .probed = true,
    };
    auto cue = whole;
    cue.logical_reference = std::string{"cue-v1\0sheet\0track", 18U};
    cue.segment = formats::SampleRange{.start_sample = 0, .end_sample = 44'100};
    cue.title = "CUE title";
    cue.metadata.fields.push_back(field("title", "CUE title", metadata::FieldProvenance::sidecar));
    LocalTrackRow unrelated = whole;
    unrelated.raw_path = "/music/other.flac";
    unrelated.title = "Other";
    unrelated.metadata.fields.front().values = {"Other"};

    LocalListModel model;
    model.replaceRows({whole, cue, unrelated});
    const metadata::MetadataDocument committed{
        .fields = {field("title", "New", metadata::FieldProvenance::embedded),
                   field("album", "New album", metadata::FieldProvenance::embedded)},
        .unsupported_native_objects = {},
    };
    const core::LocalSourceRevision revision{.device = 1,
                                             .inode = 2,
                                             .size = 3,
                                             .modification_time_seconds = 4,
                                             .modification_time_nanoseconds = 5};
    const auto refreshed = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(refreshed.has_value());
    QCOMPARE(*refreshed, 2U);
    QCOMPARE(model.rows()[0].title, std::string{"New"});
    QCOMPARE(model.rows()[0].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].title, std::string{"CUE title"});
    QCOMPARE(model.rows()[1].album, std::string{"New album"});
    QCOMPARE(model.rows()[1].metadata.fields.back().provenance, metadata::FieldProvenance::sidecar);
    QCOMPARE(model.rows()[0].source_revision, std::optional{revision});
    QCOMPARE(model.rows()[2].title, std::string{"Other"});

    auto legacy = cue;
    legacy.metadata.fields = {
        field("title", "Flattened CUE", metadata::FieldProvenance::cached_snapshot)};
    model.replaceRows({legacy});
    const auto rejected = model.applyCommittedMetadata(source, committed, revision);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, core::ErrorCode::conflict);
    QCOMPARE(model.rows()[0], legacy);
}

void BenchMainWindowTest::metadataReadyPlanAppliesAndRefreshesHistory() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("apply-ready.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({raw_path});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties_action = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties_action != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    QTRY_VERIFY(!window.property("trackbench-metadata-operation-running").toBool());
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows().front().probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties_action->isEnabled());
    properties_action->trigger();

    auto* properties = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(properties != nullptr);
    QTRY_COMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(properties));
    QVERIFY(!properties->isWindow());
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Tags · 1 track")));
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-preview-write-plan"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, QStringLiteral("Applied from ready preview"),
                                     Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    QDialog* plan = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((plan = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-write-plan"))) != nullptr,
                             5'000);
    auto* apply = plan->findChild<QPushButton*>(QStringLiteral("bench-metadata-write-plan-apply"));
    auto* plan_summary =
        plan->findChild<QLabel*>(QStringLiteral("bench-metadata-write-plan-summary"));
    QVERIFY(apply != nullptr);
    QVERIFY(plan_summary != nullptr);
    QVERIFY(apply->isEnabled());
    QVERIFY(plan_summary->text().contains(QStringLiteral("1 ready")));
    QVERIFY(plan_summary->text().contains(QStringLiteral("0 blocking issues")));
    QTest::mouseClick(apply, Qt::LeftButton);

    QDialog* apply_dialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((apply_dialog = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-apply"))) != nullptr,
                             5'000);
    auto* apply_table =
        apply_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-apply-table"));
    auto* apply_summary =
        apply_dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-apply-summary"));
    QVERIFY(apply_table != nullptr);
    QVERIFY(apply_summary != nullptr);
    QPushButton* close = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((close = apply_dialog->findChild<QPushButton*>(
                                  QStringLiteral("bench-metadata-apply-close"))) != nullptr,
                             5'000);
    QCOMPARE(apply_table->model()->rowCount(), 1);
    const auto apply_state = apply_table->model()->index(0, 0).data().toString();
    const auto apply_details = apply_table->model()->index(0, 2).data().toString();
    QVERIFY2(apply_state == QStringLiteral("Applied"), qPrintable(apply_details));
    QVERIFY(apply_summary->text().contains(QStringLiteral("1 applied")));
    QVERIFY(apply_summary->text().contains(QStringLiteral("complete")));
    QTRY_COMPARE(list_model->rows().front().title, std::string{"Applied from ready preview"});
    const auto reread = metadata::read_local_metadata(raw_path);
    QVERIFY(reread.has_value());
    QCOMPARE(reread->document.effective_values("title"),
             (std::vector<std::string>{"Applied from ready preview"}));
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties")) == nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackbench-metadata-operation-running").toBool(),
                             5'000);
    QTRY_COMPARE(window.property("trackbench-metadata-retained-backups").toULongLong(), 1ULL);
}

void BenchMainWindowTest::metadataApplyCancellationPreservesDraftForFreshPreview() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<MetadataPropertiesSource> sources;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("cancel-%1.flac").arg(index));
        QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
        const auto encoded = QFile::encodeName(path);
        const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        const auto read = metadata::read_local_metadata(raw_path);
        QVERIFY(read.has_value());
        sources.push_back(MetadataPropertiesSource{
            .source =
                metadata::StagedMetadataSource{
                    .raw_path = raw_path,
                    .source_revision = read->source_revision,
                    .baseline = read->document,
                },
            .track_label = QStringLiteral("Cancel %1").arg(index),
        });
    }

    std::atomic_size_t admitted{0U};
    std::optional<operations::MetadataApplyResult> observed;
    auto* properties = new MetadataPropertiesDialog(
        sources.size(),
        [sources](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index < sources.size() ? std::optional{sources[index]} : std::nullopt;
        },
        {},
        [&admitted] {
            return MetadataWritePlanApplier{
                [&admitted](const metadata::MetadataWritePlan& plan,
                            const operations::MetadataApplyProgressCallback& progress,
                            const core::CancellationToken& cancellation) {
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&admitted](const metadata::MetadataWritePlanSource& source,
                                    const core::CancellationToken& token)
                            -> core::Result<operations::MetadataCommitResult> {
                            admitted.fetch_add(1U, std::memory_order_relaxed);
                            while (!token.is_cancellation_requested()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                            }
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "cancelled " + source.raw_path,
                                .context = {},
                            });
                        },
                        progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [&observed](const operations::MetadataApplyResult& result) { observed = result; });
    properties->show();

    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* preview =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-preview-write-plan"));
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(preview != nullptr);
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    QVERIFY(title_row.has_value());
    QVERIFY(aggregate_model->setData(aggregate_model->index(*title_row, 2),
                                     QStringLiteral("Cancelled batch title"), Qt::EditRole));
    QTRY_VERIFY(preview->isEnabled());
    QTest::mouseClick(preview, Qt::LeftButton);

    QDialog* plan = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((plan = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-write-plan"))) != nullptr,
                             5'000);
    auto* apply = plan->findChild<QPushButton*>(QStringLiteral("bench-metadata-write-plan-apply"));
    QVERIFY(apply != nullptr);
    QTest::mouseClick(apply, Qt::LeftButton);

    QDialog* apply_dialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((apply_dialog = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-apply"))) != nullptr,
                             5'000);
    auto* cancel =
        apply_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-apply-cancel"));
    auto* table =
        apply_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-apply-table"));
    auto* summary =
        apply_dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-apply-summary"));
    QVERIFY(cancel != nullptr);
    QVERIFY(table != nullptr);
    QVERIFY(summary != nullptr);
    QTRY_COMPARE(admitted.load(std::memory_order_relaxed), 2U);
    QTest::mouseClick(cancel, Qt::LeftButton);

    QPushButton* close = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((close = apply_dialog->findChild<QPushButton*>(
                                  QStringLiteral("bench-metadata-apply-close"))) != nullptr,
                             5'000);
    QCOMPARE(table->model()->rowCount(), 3);
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        QCOMPARE(table->model()->index(row, 0).data().toString(), QStringLiteral("Cancelled"));
    }
    QVERIFY(summary->text().contains(QStringLiteral("0 applied")));
    QVERIFY(summary->text().contains(QStringLiteral("3 cancelled")));
    QVERIFY(observed.has_value());
    QCOMPARE(observed->cancelled_source_count(), 3U);
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-apply")) == nullptr);
    QTRY_VERIFY(preview->isEnabled());
    QTRY_COMPARE(
        aggregate_model->data(aggregate_model->index(*title_row, 2), Qt::EditRole).toString(),
        QStringLiteral("Cancelled batch title"));
    delete properties;
}

void BenchMainWindowTest::metadataTransformationChainPreviewsAndStagesOneUndo() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("transform.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), path));
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    auto read = metadata::read_local_metadata(raw_path);
    QVERIFY(read.has_value());
    const auto title = std::ranges::find(read->document.fields, std::string_view{"title"},
                                         &metadata::MetadataField::canonical_name);
    QVERIFY(title != read->document.fields.end());
    title->values = {"chain Title"};

    const MetadataPropertiesSource source{
        .source =
            metadata::StagedMetadataSource{
                .raw_path = raw_path,
                .source_revision = read->source_revision,
                .baseline = read->document,
            },
        .track_label = QStringLiteral("Transformation fixture"),
    };
    std::vector<persistence::SavedMetadataTransformationChain> saved_chains;
    MetadataTransformationStore transformation_store{
        .load =
            [&saved_chains](MetadataTransformationStore::LoadCompletion completion) {
                completion(saved_chains, {});
            },
        .save =
            [&saved_chains](persistence::SavedMetadataTransformationChain chain,
                            MetadataTransformationStore::Completion completion) {
                const auto found = std::ranges::find(
                    saved_chains, chain.id, &persistence::SavedMetadataTransformationChain::id);
                if (found == saved_chains.end()) {
                    saved_chains.push_back(std::move(chain));
                } else {
                    *found = std::move(chain);
                }
                completion({});
            },
        .remove =
            [&saved_chains](core::StableId id, MetadataTransformationStore::Completion completion) {
                std::erase_if(saved_chains, [id](const auto& chain) { return chain.id == id; });
                completion({});
            },
    };
    auto* properties = new MetadataPropertiesDialog(
        1U,
        [source](const std::size_t index) -> std::optional<MetadataPropertiesSource> {
            return index == 0U ? std::optional{source} : std::nullopt;
        },
        {}, {}, {}, transformation_store);
    properties->show();

    QTableView* files = nullptr;
    QTRY_VERIFY((files = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-files"))) != nullptr);
    QTableView* fields = nullptr;
    QTRY_VERIFY((fields = properties->findChild<QTableView*>(
                     QStringLiteral("bench-metadata-fields"))) != nullptr);
    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    auto* transform =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-transform"));
    auto* undo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* write_plan =
        properties->findChild<QPushButton*>(QStringLiteral("bench-metadata-preview-write-plan"));
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QVERIFY(transform != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(write_plan != nullptr);
    QTRY_VERIFY(transform->isEnabled());
    QTest::mouseClick(transform, Qt::LeftButton);

    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    auto* kind =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-kind"));
    auto* target =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-target"));
    auto* target_completer = dialog->findChild<QCompleter*>(
        QStringLiteral("bench-metadata-transformation-target-completer"));
    auto* input =
        dialog->findChild<QLineEdit*>(QStringLiteral("bench-metadata-transformation-input"));
    auto* number_start =
        dialog->findChild<QSpinBox*>(QStringLiteral("bench-metadata-transformation-number-start"));
    auto* number_padding = dialog->findChild<QSpinBox*>(
        QStringLiteral("bench-metadata-transformation-number-padding"));
    auto* add =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-add"));
    auto* preview =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-preview"));
    auto* stage =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    auto* preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    auto* preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(kind != nullptr);
    QVERIFY(target != nullptr);
    QVERIFY(target_completer != nullptr);
    QVERIFY(input != nullptr);
    QVERIFY(number_start != nullptr);
    QVERIFY(number_padding != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    QCOMPARE(kind->count(), 14);
    QCOMPARE(kind->itemText(6), QStringLiteral("Capitalize first character"));
    QCOMPARE(kind->itemText(11), QStringLiteral("Remove exact matching values"));
    QCOMPARE(kind->itemText(12), QStringLiteral("Replace exact matching values"));
    QCOMPARE(kind->itemText(13), QStringLiteral("Number by selected-file order"));
    target->setText(QStringLiteral("custom"));
    QTRY_VERIFY(target_completer->model()->rowCount() > 0);
    QCOMPARE(target_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("CUSTOM_FIELD"));
    target->clear();

    kind->setCurrentIndex(6);
    target->setText(QStringLiteral("Album Artist"));
    QTest::mouseClick(add, Qt::LeftButton);
    QTest::mouseClick(preview, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QCOMPARE(preview_table->model()->rowCount(), 0);
    QTRY_VERIFY(preview_summary->text().contains(
        QStringLiteral("every existing Album Artist value already starts with its uppercase "
                       "form")));

    kind->setCurrentIndex(6);
    target->setText(QStringLiteral("Title"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(10);
    target->setText(QStringLiteral("Comment"));
    input->setText(QStringLiteral("%artist% — %title%"));
    QTest::mouseClick(add, Qt::LeftButton);
    kind->setCurrentIndex(13);
    target->setText(QStringLiteral("Track Number"));
    number_start->setValue(7);
    number_padding->setValue(2);
    QTest::mouseClick(add, Qt::LeftButton);

    auto* save_as = dialog->findChild<QPushButton*>(
        QStringLiteral("bench-metadata-transformation-save-as-new"));
    auto* saved =
        dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    QVERIFY(save_as != nullptr);
    QVERIFY(saved != nullptr);
    QVERIFY(save_as->isEnabled());
    QTest::mouseClick(save_as, Qt::LeftButton);
    QCOMPARE(saved_chains.size(), std::size_t{1U});
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);

    dialog->close();
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);
    auto* script_panel =
        properties->findChild<QWidget*>(QStringLiteral("bench-metadata-transformation-panel"));
    auto* script_list =
        properties->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-list"));
    auto* script_status =
        properties->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-status"));
    QVERIFY(script_panel != nullptr);
    QVERIFY(script_list != nullptr);
    QVERIFY(script_status != nullptr);
    QTRY_COMPARE(script_list->count(), 1);
    QCOMPARE(script_list->currentRow(), 0);
    QCOMPARE(script_list->item(0)->checkState(), Qt::Unchecked);
    script_list->item(0)->setCheckState(Qt::Checked);
    QTRY_VERIFY(saved_chains.front().automatic);
    QTRY_VERIFY(script_status->text().contains(QStringLiteral("1 of 1 checked")));

    QTRY_VERIFY(transform->isEnabled());
    QCOMPARE(transform->text(), QStringLiteral("Edit selected script…"));
    QTest::mouseClick(transform, Qt::LeftButton);
    QTRY_VERIFY((dialog = properties->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-transformation"))) != nullptr);
    saved = dialog->findChild<QComboBox*>(QStringLiteral("bench-metadata-transformation-saved"));
    auto* steps =
        dialog->findChild<QListWidget*>(QStringLiteral("bench-metadata-transformation-steps"));
    preview =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-preview"));
    stage = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-transformation-stage"));
    preview_table =
        dialog->findChild<QTreeView*>(QStringLiteral("bench-metadata-transformation-table"));
    preview_summary =
        dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-transformation-summary"));
    QVERIFY(saved != nullptr);
    QVERIFY(steps != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(stage != nullptr);
    QVERIFY(preview_table != nullptr);
    QVERIFY(preview_summary != nullptr);
    QCOMPARE(saved->count(), 2);
    QCOMPARE(saved->currentIndex(), 1);
    QCOMPARE(steps->count(), 4);
    QTest::mouseClick(preview, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(preview_table->model() != nullptr, 5'000);
    QTRY_COMPARE(preview_table->model()->rowCount(), 3);
    QCOMPARE(preview_table->model()->headerData(0, Qt::Horizontal).toString(),
             QStringLiteral("Field"));
    QCOMPARE(preview_table->model()->headerData(1, Qt::Horizontal).toString(),
             QStringLiteral("Old"));
    QCOMPARE(preview_table->model()->headerData(2, Qt::Horizontal).toString(),
             QStringLiteral("New"));
    QCOMPARE(preview_table->model()->index(0, 0).data().toString(), QStringLiteral("Title"));
    QCOMPARE(preview_table->model()->index(0, 2).data().toString(), QStringLiteral("Chain Title"));
    QCOMPARE(preview_table->model()->index(1, 0).data().toString(), QStringLiteral("Comment"));
    QCOMPARE(preview_table->model()->index(1, 2).data().toString(),
             QStringLiteral("First Artist; Second Artist — Chain Title"));
    QCOMPARE(preview_table->model()->index(2, 0).data().toString(), QStringLiteral("Track Number"));
    QCOMPARE(preview_table->model()->index(2, 2).data().toString(), QStringLiteral("07"));
    const auto first_change = preview_table->model()->index(0, 0);
    QCOMPARE(preview_table->model()->rowCount(first_change), 1);
    QCOMPARE(preview_table->model()->index(0, 0, first_change).data().toString(),
             QStringLiteral("File"));
    QVERIFY(preview_table->model()
                ->index(0, 2, first_change)
                .data()
                .toString()
                .contains(QStringLiteral("step 2")));
    QVERIFY(preview_summary->text().contains(QStringLiteral("3 final cell changes")));
    QVERIFY(stage->isEnabled());
    QTest::mouseClick(stage, Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-transformation")) ==
                nullptr);

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto comment_column = grid_model->fieldColumn(QStringLiteral("comment"));
    const auto track_number_column = grid_model->fieldColumn(QStringLiteral("track number"));
    QVERIFY(title_column.has_value());
    QVERIFY(comment_column.has_value());
    QVERIFY(track_number_column.has_value());
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{3U});
    QCOMPARE(grid_model->index(0, *title_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Chain Title")}));
    QCOMPARE(grid_model->index(0, *comment_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("First Artist; Second Artist — Chain Title")}));
    QCOMPARE(
        grid_model->index(0, *track_number_column).data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("07")}));

    QTRY_VERIFY(undo->isEnabled());
    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    QTRY_VERIFY(redo->isEnabled());
    QTest::mouseClick(redo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{3U});

    QTest::mouseClick(undo, Qt::LeftButton);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{0U});
    const auto album_column = grid_model->fieldColumn(QStringLiteral("album"));
    QVERIFY(album_column.has_value());
    const std::array automatic_items{std::size_t{0U}};
    QVERIFY(grid_model->replaceFieldValues(
        automatic_items, static_cast<std::size_t>(*album_column - 1), {"Automatic workflow"}));
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});

    QTRY_VERIFY(write_plan->isEnabled());
    QTest::mouseClick(write_plan, Qt::LeftButton);
    QDialog* plan_dialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((plan_dialog = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-write-plan"))) != nullptr,
                             5'000);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    auto* plan_table =
        plan_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-write-plan-table"));
    QVERIFY(plan_table != nullptr);
    QCOMPARE(plan_table->model()->rowCount(), 4);
    QStringList planned_fields;
    for (auto row = 0; row < plan_table->model()->rowCount(); ++row) {
        planned_fields.push_back(
            plan_table->model()->index(row, 2).data().toString().toCaseFolded());
    }
    QVERIFY(planned_fields.contains(QStringLiteral("album")));
    QVERIFY(planned_fields.contains(QStringLiteral("title")));
    QVERIFY(planned_fields.contains(QStringLiteral("comment")));
    QVERIFY(planned_fields.contains(QStringLiteral("track number")));
    QVERIFY(plan_dialog->findChild<QPushButton*>(
                QStringLiteral("bench-metadata-write-plan-apply")) != nullptr);
    auto* plan_buttons = plan_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-write-plan-buttons"));
    QVERIFY(plan_buttons != nullptr);
    QTest::mouseClick(plan_buttons->button(QDialogButtonBox::Close), Qt::LeftButton);
    QTRY_VERIFY(properties->findChild<QDialog*>(QStringLiteral("bench-metadata-write-plan")) ==
                nullptr);
    QTest::mouseClick(write_plan, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT((plan_dialog = properties->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-write-plan"))) != nullptr,
                             5'000);
    plan_table =
        plan_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-write-plan-table"));
    QVERIFY(plan_table != nullptr);
    QCOMPARE(plan_table->model()->rowCount(), 4);
    QCOMPARE(grid_model->patches().patch_count(), std::size_t{1U});
    plan_buttons = plan_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-write-plan-buttons"));
    QVERIFY(plan_buttons != nullptr);
    QTest::mouseClick(plan_buttons->button(QDialogButtonBox::Close), Qt::LeftButton);
    delete properties;
}

void BenchMainWindowTest::metadataOperationHistoryUndoesRetainedBackup() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto source_path = media.filePath(QStringLiteral("history.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), source_path));
    QFile original_file{source_path};
    QVERIFY(original_file.open(QIODevice::ReadOnly));
    const auto original_bytes = original_file.readAll();
    const auto encoded = QFile::encodeName(source_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    const auto initial = metadata::read_local_metadata(raw_path);
    QVERIFY(initial.has_value());
    auto selection = metadata::StagedMetadataSelection::create({
        metadata::StagedMetadataSource{
            .raw_path = raw_path,
            .source_revision = initial->source_revision,
            .baseline = initial->document,
        },
    });
    QVERIFY(selection.has_value());
    const auto title = selection->field_index("TITLE");
    QVERIFY(title.has_value());
    metadata::StagedMetadataPatchSet patches;
    QVERIFY(patches.replace_values(*selection, 0U, *title, {"History published title"}));
    auto plan = metadata::revalidate_metadata_write_plan(*selection, patches);
    QVERIFY(plan.has_value());
    QVERIFY(plan->ready());
    QCOMPARE(plan->sources.size(), 1U);

    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto repository = persistence::ListRepository::open(database_path);
        QVERIFY(repository.has_value());
        const std::vector documents{persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "History",
            .pinned = false,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = raw_path,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = initial->source_revision,
                .fields = {},
            }},
        }};
        QVERIFY(repository->replace_all(documents));
        auto journal = persistence::SqliteMetadataOperationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto committed = operations::commit_flac_metadata_source(
            plan->sources.front(), *journal,
            [&repository](const operations::MetadataCommitResult& result) -> core::Result<void> {
                auto refreshed =
                    repository->refresh_local_metadata(persistence::LocalMetadataRefresh{
                        .operation_id = result.journal_id,
                        .source_reference = result.source_raw_path,
                        .previous_revision = result.previous_revision,
                        .published_revision = result.published_revision,
                        .document = result.document,
                    });
                return refreshed ? core::Result<void>{}
                                 : std::unexpected(std::move(refreshed.error()));
            });
        QVERIFY2(committed.has_value(), committed ? "" : committed.error().message.c_str());
    }

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* history = window.findChild<QAction*>(QStringLiteral("action-metadata-operation-history"));
    QVERIFY(tabs != nullptr);
    QVERIFY(history != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    QTRY_VERIFY(!window.property("trackbench-metadata-operation-running").toBool());
    QTRY_COMPARE(window.property("trackbench-metadata-retained-backups").toULongLong(), 1ULL);
    QVERIFY(history->isEnabled());
    auto* list_model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE(list_model->rowCount(), 1);
    QCOMPARE(list_model->rows().front().title, std::string{"History published title"});

    history->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("bench-metadata-operation-history"));
    QVERIFY(dialog != nullptr);
    auto* table = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-operation-table"));
    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-operation-undo"));
    auto* release =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-operation-release"));
    auto* policy = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-retention-policy"));
    QVERIFY(table != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(release != nullptr);
    QVERIFY(policy != nullptr);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Undo available"));
    QVERIFY(policy->text().contains(QStringLiteral("7 days")));
    QVERIFY(policy->text().contains(QStringLiteral("256 operations")));
    QVERIFY(undo->isEnabled());
    QVERIFY(release->isEnabled());

    QTimer::singleShot(0, [] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(confirmation != nullptr);
        auto* yes = confirmation->button(QMessageBox::Yes);
        QVERIFY(yes != nullptr);
        QTest::mouseClick(yes, Qt::LeftButton);
    });
    QTest::mouseClick(undo, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!window.property("trackbench-metadata-operation-running").toBool(),
                             5'000);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rows().front().title, std::string{"Metadata Fixture"},
                              5'000);
    QFile restored_file{source_path};
    QVERIFY(restored_file.open(QIODevice::ReadOnly));
    QCOMPARE(restored_file.readAll(), original_bytes);
    QTRY_COMPARE(window.property("trackbench-metadata-retained-backups").toULongLong(), 0ULL);
    QDialog* refreshed_dialog = nullptr;
    QTRY_VERIFY((refreshed_dialog = window.findChild<QDialog*>(
                     QStringLiteral("bench-metadata-operation-history"))) != nullptr);
    auto* refreshed_table =
        refreshed_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-operation-table"));
    QVERIFY(refreshed_table != nullptr);
    QCOMPARE(refreshed_table->model()->rowCount(), 1);
    QCOMPARE(refreshed_table->model()->index(0, 0).data().toString(), QStringLiteral("Undone"));
}

void BenchMainWindowTest::metadataStartupPresentsReconciliation() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base));
    const auto database_path = std::filesystem::path{
        QFile::encodeName(base + QStringLiteral("/lists.sqlite")).toStdString()};
    {
        auto journal = persistence::SqliteMetadataOperationJournal::open(database_path);
        QVERIFY(journal.has_value());
        const auto id = core::StableId::random();
        const core::LocalSourceRevision revision{.device = 1,
                                                 .inode = 2,
                                                 .size = 3,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
        const operations::MetadataOperationJournalRecord record{
            .id = id,
            .state = operations::MetadataOperationJournalState::planned,
            .source_raw_path = "/music/needs-attention.flac",
            .prepared_raw_path = "/music/.trackknife-prepared",
            .backup_raw_path = "/music/.trackknife-backup",
            .expected_revision = revision,
            .prepared_revision = std::nullopt,
            .published_revision = std::nullopt,
            .occurrence_indexes = {0U},
            .changes = {operations::MetadataOperationJournalChange{
                .field_index = 0U,
                .canonical_name = "title",
                .property_name = "TITLE",
                .original_present = true,
                .original_values = {"Old"},
                .kind = metadata::StagedMetadataPatchKind::replace_values,
                .planned_values = {"New"},
                .item_indexes = {0U},
            }},
            .failure = std::nullopt,
        };
        QVERIFY(journal->create(record));
        QVERIFY(journal->transition(
            id, operations::MetadataOperationJournalTransition{
                    .expected_state = operations::MetadataOperationJournalState::planned,
                    .state = operations::MetadataOperationJournalState::needs_reconciliation,
                    .prepared_revision = std::nullopt,
                    .published_revision = std::nullopt,
                    .failure = core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Ambiguous source identity",
                                           .context = {}},
                }));
    }

    BenchMainWindow window;
    window.show();
    QTRY_VERIFY(!window.property("trackbench-metadata-operation-running").toBool());
    QTRY_COMPARE(window.property("trackbench-metadata-reconciliation-count").toULongLong(), 1ULL);
    QDialog* dialog = nullptr;
    QTRY_VERIFY((dialog = window.findChild<QDialog*>(
                     QStringLiteral("bench-metadata-operation-history"))) != nullptr);
    auto* table = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-operation-table"));
    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-operation-undo"));
    auto* release =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-operation-release"));
    QVERIFY(table != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(release != nullptr);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Needs attention"));
    QVERIFY(table->model()->index(0, 3).data().toString().contains(
        QStringLiteral("Ambiguous source identity")));
    QVERIFY(!undo->isEnabled());
    QVERIFY(!release->isEnabled());
}

void BenchMainWindowTest::folderDiscoveryAdmitsWave64() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave64_path = media.filePath(QStringLiteral("accepted.W64"));
    const auto ignored_path = media.filePath(QStringLiteral("ignored.txt"));
    for (const auto& path : {wave64_path, ignored_path}) {
        QFile file{path};
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("fixture"), 7);
    }
    const auto directory_encoded = QFile::encodeName(media.path());
    const std::string directory_raw{directory_encoded.constData(),
                                    static_cast<std::size_t>(directory_encoded.size())};
    const auto wave64_encoded = QFile::encodeName(wave64_path);
    const std::string wave64_raw{wave64_encoded.constData(),
                                 static_cast<std::size_t>(wave64_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({directory_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->rawPath(0), wave64_raw);
}

void BenchMainWindowTest::contextMenusTargetSelectionsListsAndFolders() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    std::vector<std::string> raw_paths;
    for (int index = 0; index < 3; ++index) {
        const auto path = media.filePath(QStringLiteral("context-%1.wav").arg(index));
        write_wave(path, wave_sample_rate / 10U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
    auto* track_menu = window.findChild<QMenu*>(QStringLiteral("bench-track-context-menu"));
    auto* folder_menu = window.findChild<QMenu*>(QStringLiteral("bench-folder-context-menu"));
    auto* folder_view = window.findChild<QTreeView*>(QStringLiteral("bench-folder-tree"));
    auto* folder_model = window.findChild<ui::LocalFolderTreeModel*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(duplicate != nullptr);
    QVERIFY(track_menu != nullptr);
    QVERIFY(folder_menu != nullptr);
    QVERIFY(folder_view != nullptr);
    QVERIFY(folder_model != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    window.openLocalPaths(raw_paths);
    auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(source != nullptr);
    QTRY_COMPARE(source->model()->rowCount(), 3);

    duplicate->trigger();
    QCOMPARE(tabs->count(), 3);
    auto* destination = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(destination != nullptr);
    QCOMPARE(destination->model()->rowCount(), 3);
    tabs->setCurrentWidget(source);

    source->selectionModel()->select(source->model()->index(0, 0),
                                     QItemSelectionModel::ClearAndSelect |
                                         QItemSelectionModel::Rows);
    source->selectionModel()->select(source->model()->index(1, 0),
                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const auto selected_position = source->visualRect(source->model()->index(1, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, selected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 2);
    auto* copy_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-copy-menu"));
    auto* move_menu = track_menu->findChild<QMenu*>(QStringLiteral("bench-track-move-menu"));
    auto* play = window.findChild<QAction*>(QStringLiteral("action-play-selected-track"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove-selected-tracks"));
    QVERIFY(copy_menu != nullptr);
    QVERIFY(move_menu != nullptr);
    QVERIFY(play != nullptr);
    QVERIFY(properties != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(play->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(track_menu->actions().contains(properties));
    QVERIFY(remove->isEnabled());
    QCOMPARE(copy_menu->actions().size(), 1);
    QCOMPARE(move_menu->actions().size(), 1);
    copy_menu->actions().front()->trigger();
    QCOMPARE(destination->model()->rowCount(), 5);

    const auto unselected_position = source->visualRect(source->model()->index(2, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, unselected_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(source->selectionModel()->selectedRows().front().row(), 2);

    const auto album_header_position =
        QPoint{source->visualRect(source->model()->index(0, 0)).center().x(),
               source->visualRect(source->model()->index(0, 0)).top() + 5};
    QVERIFY(QMetaObject::invokeMethod(source, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, album_header_position)));
    QCOMPARE(source->selectionModel()->selectedRows().size(), 3);
    track_menu->close();

    const auto directory_encoded = QFile::encodeName(media.path());
    const std::string directory_raw{directory_encoded.constData(),
                                    static_cast<std::size_t>(directory_encoded.size())};
    folder_model->addRoot(directory_raw);
    const auto root = folder_model->index(0, 0);
    QVERIFY(root.isValid());
    folder_view->scrollTo(root);
    const auto root_position = folder_view->visualRect(root).center();
    QVERIFY(QMetaObject::invokeMethod(folder_view, "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, root_position)));
    auto* add_folder = window.findChild<QAction*>(QStringLiteral("action-folder-add-to-list"));
    auto* toggle_folder =
        window.findChild<QAction*>(QStringLiteral("action-folder-toggle-expanded"));
    QVERIFY(add_folder != nullptr);
    QVERIFY(toggle_folder != nullptr);
    QCOMPARE(add_folder->text(), QStringLiteral("Add folder to current list"));
    QCOMPARE(toggle_folder->text(), QStringLiteral("Expand"));
    toggle_folder->trigger();
    QVERIFY(folder_view->isExpanded(root));
    add_folder->trigger();
    QTRY_COMPARE(source->model()->rowCount(), 6);
}

void BenchMainWindowTest::panelLayoutPersistsAndPreservesFutureState() {
    constexpr auto layout_key = "workspace/panel-layout-v1";
    const auto root_widget = [](BenchMainWindow& window) {
        auto* host = window.findChild<QWidget*>(QStringLiteral("bench-panel-layout-host"));
        return host != nullptr && host->layout() != nullptr && host->layout()->count() == 1
                   ? host->layout()->itemAt(0)->widget()
                   : nullptr;
    };

    {
        BenchMainWindow window;
        window.show();
        auto* edit = window.findChild<QAction*>(QStringLiteral("action-edit-panel-layout"));
        auto* vertical = window.findChild<QAction*>(QStringLiteral("action-layout-top-bottom"));
        auto* tabbed = window.findChild<QAction*>(QStringLiteral("action-layout-tabbed"));
        auto* swap = window.findChild<QAction*>(QStringLiteral("action-layout-swap-panels"));
        auto* track_tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(edit != nullptr);
        QVERIFY(vertical != nullptr);
        QVERIFY(tabbed != nullptr);
        QVERIFY(swap != nullptr);
        QVERIFY(track_tabs != nullptr);
        QTRY_COMPARE(track_tabs->count(), 2);
        QCOMPARE(edit->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+L")));

        auto* initial_split = qobject_cast<QSplitter*>(root_widget(window));
        QVERIFY(initial_split != nullptr);
        QCOMPARE(initial_split->orientation(), Qt::Horizontal);
        QCOMPARE(initial_split->widget(0)->objectName(), QStringLiteral("bench-panel-folders"));
        QCOMPARE(initial_split->widget(1)->objectName(), QStringLiteral("bench-tabs"));
        QTRY_VERIFY(initial_split->sizes().at(0) < initial_split->sizes().at(1));
        QVERIFY(!vertical->isEnabled());

        edit->trigger();
        QVERIFY(edit->isChecked());
        QVERIFY(vertical->isEnabled());
        vertical->trigger();
        auto* vertical_split = qobject_cast<QSplitter*>(root_widget(window));
        QVERIFY(vertical_split != nullptr);
        QCOMPARE(vertical_split->orientation(), Qt::Vertical);

        tabbed->trigger();
        auto* panel_tabs = qobject_cast<QTabWidget*>(root_widget(window));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->objectName(), QStringLiteral("bench-panel-layout-tabs"));
        QCOMPARE(panel_tabs->count(), 2);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->currentIndex(), 1);

        swap->trigger();
        panel_tabs = qobject_cast<QTabWidget*>(root_widget(window));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->currentIndex(), 0);
        QVERIFY(window.close());
    }

    {
        BenchMainWindow restored;
        restored.show();
        auto* panel_tabs = qobject_cast<QTabWidget*>(root_widget(restored));
        QVERIFY(panel_tabs != nullptr);
        QCOMPARE(panel_tabs->tabText(0), QStringLiteral("Track Lists"));
        QCOMPARE(panel_tabs->tabText(1), QStringLiteral("Sources"));
        QCOMPARE(panel_tabs->currentIndex(), 0);

        QSettings settings;
        QString error;
        const auto decoded = ui::deserializePanelLayout(
            settings.value(QString::fromLatin1(layout_key)).toByteArray(),
            {QStringLiteral("folders"), QStringLiteral("track-lists")}, &error);
        QVERIFY2(decoded.has_value(), qPrintable(error));
        QCOMPARE(decoded->root.kind, ui::PanelLayoutNodeKind::tabs);

        auto* reset = restored.findChild<QAction*>(QStringLiteral("action-reset-panel-layout"));
        QVERIFY(reset != nullptr);
        reset->trigger();
        auto* reset_split = qobject_cast<QSplitter*>(root_widget(restored));
        QVERIFY(reset_split != nullptr);
        QCOMPARE(reset_split->orientation(), Qt::Horizontal);
        QCOMPARE(reset_split->widget(0)->objectName(), QStringLiteral("bench-panel-folders"));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"root":{"kind":"panel","panel":"future"},"future":{"keep":true}})"};
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(layout_key), future_layout);
        settings.sync();
    }
    {
        BenchMainWindow fallback;
        fallback.show();
        auto* fallback_split = qobject_cast<QSplitter*>(root_widget(fallback));
        QVERIFY(fallback_split != nullptr);
        QCOMPARE(fallback_split->orientation(), Qt::Horizontal);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    QSettings settings;
    QCOMPARE(settings.value(QString::fromLatin1(layout_key)).toByteArray(), future_layout);
}

void BenchMainWindowTest::localReorderPreservesVisibleRowGeometry() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        result.artist = "Artist";
        result.album = "Album";
        return result;
    };
    model.replaceRows(
        {make_row("/music/one.flac"), make_row("/music/two.flac"), make_row("/music/three.flac")});
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};
    QSignalSpy moved{&model, &QAbstractItemModel::rowsMoved};
    ui::QueueTableView view{nullptr};
    view.setModel(&model);
    view.setItemDelegate(new ui::QueueItemDelegate(&view));
    view.verticalHeader()->setDefaultSectionSize(22);
    view.verticalHeader()->setMinimumSectionSize(18);
    view.setAlbumGroupingEnabled(true);
    view.resize(640, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    model.reorderRows({0}, 3);
    QCoreApplication::processEvents();

    QCOMPARE(reset.count(), 0);
    QCOMPARE(moved.count(), 1);
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/three.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(view.verticalHeader()->count(), model.rowCount());
    for (int row_index = 0; row_index < model.rowCount(); ++row_index) {
        QVERIFY(!view.isRowHidden(row_index));
        QVERIFY(view.rowHeight(row_index) >= 18);
        QVERIFY(!view.visualRect(model.index(row_index, local_title_column)).isEmpty());
    }
}

void BenchMainWindowTest::noncontiguousLocalReorderKeepsTheModelResetBoundaryIntact() {
    LocalListModel model;
    const auto make_row = [](std::string path) {
        LocalTrackRow result;
        result.raw_path = std::move(path);
        result.title = result.raw_path;
        return result;
    };
    model.replaceRows({make_row("/music/one.flac"), make_row("/music/two.flac"),
                       make_row("/music/three.flac"), make_row("/music/four.flac")});
    std::vector<std::string> rows_seen_before_reset;
    connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&] {
        for (const auto& row : model.rows()) {
            rows_seen_before_reset.push_back(row.raw_path);
        }
    });
    QSignalSpy reset{&model, &QAbstractItemModel::modelReset};

    model.reorderRows({0, 2}, 4);

    QCOMPARE(reset.count(), 1);
    QCOMPARE(rows_seen_before_reset,
             (std::vector<std::string>{"/music/one.flac", "/music/two.flac", "/music/three.flac",
                                       "/music/four.flac"}));
    QCOMPARE(model.rows()[0].raw_path, std::string{"/music/two.flac"});
    QCOMPARE(model.rows()[1].raw_path, std::string{"/music/four.flac"});
    QCOMPARE(model.rows()[2].raw_path, std::string{"/music/one.flac"});
    QCOMPARE(model.rows()[3].raw_path, std::string{"/music/three.flac"});
}

void BenchMainWindowTest::trackViewLayoutMatchesGroupedQueueAndPersists() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto first_path = media.filePath(QStringLiteral("first.wav"));
    const auto second_path = media.filePath(QStringLiteral("second.wav"));
    write_wave(first_path, wave_sample_rate / 10U);
    write_wave(second_path, wave_sample_rate / 10U);
    const auto first_encoded = QFile::encodeName(first_path);
    const auto second_encoded = QFile::encodeName(second_path);
    const std::string first_raw{first_encoded.constData(),
                                static_cast<std::size_t>(first_encoded.size())};
    const std::string second_raw{second_encoded.constData(),
                                 static_cast<std::size_t>(second_encoded.size())};
    QString binding;
    int persisted_title_width = 0;
    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* side = window.findChild<QAction*>(QStringLiteral("action-track-layout-albums-side"));
        auto* plain = window.findChild<QAction*>(QStringLiteral("action-track-layout-plain"));
        auto* date = window.findChild<QAction*>(QStringLiteral("action-track-column-date"));
        QVERIFY(tabs != nullptr);
        QVERIFY(side != nullptr);
        QVERIFY(plain != nullptr);
        QVERIFY(date != nullptr);
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        QCOMPARE(view->model()->columnCount(), local_column_count);
        QVERIFY(side->isChecked());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(view->albumGroupingEnabled());
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) != nullptr);
        const auto viewport_children =
            view->viewport()->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly);
        for (const auto* child : viewport_children) {
            QVERIFY2(!child->isVisible() || !child->geometry().contains(view->viewport()->rect()),
                     "A child widget must not cover Trackbench's delegate-painted track rows");
        }
        QVERIFY(!view->wordWrap());
        QCOMPARE(view->textElideMode(), Qt::ElideRight);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        window.openLocalPaths({first_raw, second_raw});
        QTRY_COMPARE(view->model()->rowCount(), 2);
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        QCOMPARE(view->model()->headerData(local_artist_column, Qt::Horizontal).toString(),
                 QStringLiteral("Artist"));
        QCOMPARE(view->model()->headerData(local_track_number_column, Qt::Horizontal).toString(),
                 QStringLiteral("#"));

        LocalTrackRow singleton{
            .raw_path = "/standalone.wav",
            .logical_reference = std::nullopt,
            .selection = {},
            .segment = std::nullopt,
            .title = "Standalone",
            .artist = "Solo artist",
            .album = "One-off",
            .album_artist = {},
            .date = "2026",
            .track_number = {},
            .duration_ms = 1'000,
            .metadata = {},
            .source_revision = std::nullopt,
        };
        auto* local_model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(local_model != nullptr);
        local_model->appendRows({std::move(singleton)});
        QTRY_COMPARE(view->rowHeight(2), view->rowHeight(1));
        QImage singleton_cover{12, 12, QImage::Format_RGB32};
        singleton_cover.fill(Qt::red);
        local_model->setArtwork(local_model->groupKey(2), singleton_cover);
        QCoreApplication::processEvents();
        const auto artwork_rect = view->visualRect(local_model->index(2, local_artwork_column));
        const auto artwork_render = view->viewport()->grab(artwork_rect).toImage();
        bool found_inline_cover = false;
        auto leftmost_cover_pixel = artwork_render.width();
        for (int y = 0; y < artwork_render.height(); ++y) {
            for (int x = 0; x < artwork_render.width(); ++x) {
                if (artwork_render.pixelColor(x, y) == QColor(Qt::red)) {
                    found_inline_cover = true;
                    leftmost_cover_pixel = std::min(leftmost_cover_pixel, x);
                }
            }
        }
        QVERIFY(found_inline_cover);
        QVERIFY(leftmost_cover_pixel >= artwork_render.width() / 2);

        view->selectionModel()->select(local_model->index(2, 0),
                                       QItemSelectionModel::ClearAndSelect |
                                           QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
        const auto selected_artwork_render = view->viewport()->grab(artwork_rect).toImage();
        QCOMPARE(selected_artwork_render, artwork_render);
        view->clearSelection();
        local_model->setCurrentSource(local_model->source(2), 2);
        QCoreApplication::processEvents();
        const auto active_artwork_render = view->viewport()->grab(artwork_rect).toImage();
        QCOMPARE(active_artwork_render, artwork_render);

        plain->trigger();
        QVERIFY(plain->isChecked());
        QCOMPARE(view->albumArtworkColumn(), -1);
        QVERIFY(qobject_cast<ui::QueueItemDelegate*>(view->itemDelegate()) == nullptr);
        QVERIFY(view->isColumnHidden(local_artwork_column));
        QCOMPARE(view->rowHeight(0), view->rowHeight(1));
        QCOMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize());
        QVERIFY(!view->albumGroupingEnabled());
        QCOMPARE(view->verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);

        side->trigger();
        QVERIFY(view->albumGroupingEnabled());
        QTRY_VERIFY(view->rowHeight(0) > view->rowHeight(1));
        auto* header = view->horizontalHeader();
        header->moveSection(header->visualIndex(local_title_column), 1);
        view->setColumnWidth(local_title_column, 333);
        date->trigger();
        QVERIFY(view->isColumnHidden(local_date_column));
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        window.resize(1'350, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QVERIFY(view->columnWidth(local_title_column) > persisted_title_width);
        window.resize(1'100, 720);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        persisted_title_width = view->columnWidth(local_title_column);
        binding = QStringLiteral("local:%1").arg(view->property("bench-document-id").toString());
        QVERIFY(window.close());
    }

    {
        BenchMainWindow restored;
        restored.show();
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QCOMPARE(view->horizontalHeader()->logicalIndex(1), local_title_column);
        QTRY_COMPARE(view->horizontalHeader()->length(), view->viewport()->width());
        QCOMPARE(view->columnWidth(local_title_column), persisted_title_width);
        QVERIFY(view->isColumnHidden(local_date_column));
        QVERIFY(restored.close());
    }

    const QByteArray future_layout{
        R"({"schema":99,"presentation":"future","columns":[],"keep":true})"};
    const auto database_path = std::filesystem::path{
        QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          QStringLiteral("/lists.sqlite"))
            .toStdString()};
    {
        auto repository = persistence::ListRepository::open(database_path);
        QVERIFY(repository.has_value());
        const std::vector presets{persistence::TrackViewPreset{
            .binding = binding.toStdString(),
            .header_state = std::string{future_layout.constData(),
                                        static_cast<std::size_t>(future_layout.size())},
        }};
        QVERIFY(repository->replace_view_presets(presets).has_value());
    }
    {
        BenchMainWindow fallback;
        fallback.show();
        auto* tabs = fallback.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = static_cast<ui::QueueTableView*>(tabs->currentWidget());
        QCOMPARE(view->albumArtworkColumn(), local_artwork_column);
        QVERIFY(fallback.statusBar()->currentMessage().contains(QStringLiteral("preserved")));
        QVERIFY(fallback.close());
    }
    auto repository = persistence::ListRepository::open(database_path);
    QVERIFY(repository.has_value());
    const auto presets = repository->load_view_presets();
    QVERIFY(presets.has_value());
    QCOMPARE(presets->size(), 2U);
    const auto stored =
        std::ranges::find(*presets, binding.toStdString(), &persistence::TrackViewPreset::binding);
    QVERIFY(stored != presets->end());
    const QByteArray stored_layout{stored->header_state.data(),
                                   static_cast<qsizetype>(stored->header_state.size())};
    QCOMPARE(stored_layout, future_layout);
}

void BenchMainWindowTest::persistsPinnedDuplicatedAndDirtyTabs() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto path = media.filePath(QStringLiteral("tab-state.wav"));
    write_wave(path, wave_sample_rate / 10U);
    const auto encoded = QFile::encodeName(path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
        auto* pin = window.findChild<QAction*>(QStringLiteral("action-pin-tab"));
        auto* save = window.findChild<QAction*>(QStringLiteral("action-save-list"));
        auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QVERIFY(duplicate != nullptr);
        QVERIFY(pin != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(close != nullptr);
        QCOMPARE(duplicate->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+D")));
        QCOMPARE(pin->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(save->shortcut(), QKeySequence::Save);
        QCOMPARE(close->shortcut(), QKeySequence(QStringLiteral("Ctrl+W")));
        QTRY_COMPARE(tabs->count(), 2);

        window.openLocalPaths({raw_path});
        auto* source = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(source != nullptr);
        QTRY_COMPARE(source->model()->rowCount(), 1);
        const auto source_index = tabs->indexOf(source);
        QVERIFY(source_index >= 0);
        QTRY_VERIFY(tabs->tabText(source_index).endsWith(QStringLiteral(" *")));
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("modified")));

        pin->trigger();
        QVERIFY(pin->isChecked());
        QVERIFY(tabs->tabToolTip(source_index).contains(QStringLiteral("pinned")));
        auto* source_close = tabs->tabBar()->tabButton(source_index, QTabBar::RightSide);
        QVERIFY(source_close != nullptr);
        QVERIFY(!source_close->isVisible());
        QVERIFY(!close->isEnabled());

        duplicate->trigger();
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->currentIndex(), 2);
        auto* copied = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(copied != nullptr);
        QCOMPARE(copied->model()->rowCount(), 1);
        const auto copied_index = tabs->indexOf(copied);
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Local Queue copy *"));
        QVERIFY(tabs->tabBar()->tabButton(copied_index, QTabBar::RightSide)->isVisible());

        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QStringLiteral("Saved copy"));
                dialog->accept();
            }
        });
        save->trigger();
        QCOMPARE(tabs->tabText(copied_index), QStringLiteral("Saved copy"));
        QVERIFY(!tabs->tabToolTip(copied_index).contains(QStringLiteral("modified")));

        // A later edit makes the explicitly saved list dirty again. Closing
        // must offer discard and honor both answers.
        window.openLocalPaths({raw_path});
        QTRY_COMPARE(copied->model()->rowCount(), 2);
        QTRY_VERIFY(tabs->tabText(copied_index).endsWith(QStringLiteral(" *")));
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::No);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 3);
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                box->done(QMessageBox::Yes);
            }
        });
        close->trigger();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentWidget(), source);
        QVERIFY(pin->isChecked());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    const auto local_index = tabs->currentIndex();
    QCOMPARE(tabs->tabText(local_index), QStringLiteral("Local Queue *"));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("pinned")));
    QVERIFY(tabs->tabToolTip(local_index).contains(QStringLiteral("modified")));
    QVERIFY(!tabs->tabBar()->tabButton(local_index, QTabBar::RightSide)->isVisible());
    auto* view = qobject_cast<QTableView*>(tabs->widget(local_index));
    QVERIFY(view != nullptr);
    QCOMPARE(view->model()->rowCount(), 1);
}

void BenchMainWindowTest::richMetadataValuesAndIdentitiesSurviveListRestart() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto fixture_path = media.filePath(QStringLiteral("rich-metadata.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), fixture_path));
    const auto encoded = QFile::encodeName(fixture_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 2);
        auto* model = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
        QTRY_VERIFY_WITH_TIMEOUT(model->rows().front().probed, 5'000);
        const auto& row = model->rows().front();
        QCOMPARE(row.title, std::string{"Metadata Fixture"});
        QCOMPARE(row.artist, std::string{"First Artist"});
        QCOMPARE(row.album, std::string{"Rich Metadata"});
        QCOMPARE(row.album_artist, std::string{"Album Credit"});
        QCOMPARE(row.metadata.effective_values("artist"),
                 (std::vector<std::string>{"First Artist", "Second Artist"}));
        QCOMPARE(row.metadata.effective_values("custom-field"),
                 (std::vector<std::string>{"first custom value", "second custom value"}));
        const auto identity = metadata::project_musicbrainz(row.metadata);
        QCOMPARE(identity.recording_ids,
                 (std::vector<std::string>{"11111111-1111-1111-1111-111111111111"}));
        QCOMPARE(identity.artist_ids,
                 (std::vector<std::string>{"55555555-5555-5555-5555-555555555555",
                                           "66666666-6666-6666-6666-666666666666"}));
        QVERIFY(row.source_revision.has_value());
        QCOMPARE(row.source_revision->size, 2'308U);
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 1);
    const auto& row = model->rows().front();
    QCOMPARE(row.artist, std::string{"First Artist"});
    QCOMPARE(row.metadata.effective_values("artist"),
             (std::vector<std::string>{"First Artist", "Second Artist"}));
    QCOMPARE(row.metadata.effective_values("custom_field"),
             (std::vector<std::string>{"first custom value", "second custom value"}));
    QCOMPARE(metadata::project_musicbrainz(row.metadata).release_ids,
             (std::vector<std::string>{"33333333-3333-3333-3333-333333333333"}));
    // The snapshot and revision are explicitly stale evidence. The write-plan
    // boundary observes the file again and compares this value before commit;
    // retaining it lets dependent-state replay distinguish an older queued
    // list save from a later external refresh.
    QVERIFY(row.source_revision.has_value());
    QCOMPARE(row.source_revision->size, 2'308U);
}

void BenchMainWindowTest::metadataPropertiesFileSelectionDrivesIndividualAndBulkEdits() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto rich_path = media.filePath(QStringLiteral("rich.flac"));
    const auto ordinary_path = media.filePath(QStringLiteral("ordinary.flac"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("rich-metadata-flac.b64"), rich_path));
    QVERIFY(materialize_audio_fixture(QStringLiteral("tagged-tone-flac.b64"), ordinary_path));
    const auto rich_encoded = QFile::encodeName(rich_path);
    const auto ordinary_encoded = QFile::encodeName(ordinary_path);
    const std::string rich_raw{rich_encoded.constData(),
                               static_cast<std::size_t>(rich_encoded.size())};
    const std::string ordinary_raw{ordinary_encoded.constData(),
                                   static_cast<std::size_t>(ordinary_encoded.size())};

    BenchMainWindow window;
    window.show();
    window.openLocalPaths({rich_raw, ordinary_raw});
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    auto* properties = window.findChild<QAction*>(QStringLiteral("action-track-properties"));
    QVERIFY(tabs != nullptr);
    QVERIFY(properties != nullptr);
    QCOMPARE(properties->shortcut(), QKeySequence(QStringLiteral("Alt+Return")));
    QTRY_COMPARE(tabs->count(), 2);
    auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(view != nullptr);
    auto* list_model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(list_model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(list_model->rowCount(), 2, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(list_model->rows()[0].probed && list_model->rows()[1].probed, 5'000);
    view->selectionModel()->select(list_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->selectionModel()->select(list_model->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_VERIFY(properties->isEnabled());
    properties->trigger();

    auto* dialog = window.findChild<QDialog*>(QStringLiteral("bench-metadata-properties"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->isModal());
    QVERIFY(!dialog->isWindow());
    QTRY_COMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget*>(dialog));
    QVERIFY(tabs->tabText(tabs->currentIndex()).startsWith(QStringLiteral("Tags · 2 tracks")));
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-loading")) != nullptr);

    QTableView* files = nullptr;
    QTRY_VERIFY((files = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-files"))) !=
                nullptr);
    auto* fields = dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-fields"));
    auto* summary = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-summary"));
    auto* read_only = dialog->findChild<QLabel*>(QStringLiteral("bench-metadata-read-only"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("bench-metadata-buttons"));
    QVERIFY(fields != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(read_only != nullptr);
    QVERIFY(buttons != nullptr);
    QVERIFY(dialog->findChild<QTabWidget*>(QStringLiteral("bench-metadata-pages")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-grid")) == nullptr);
    QVERIFY(dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-values")) == nullptr);
    QVERIFY2(summary->text().contains(QStringLiteral("2 of 2 files selected")),
             qPrintable(QStringLiteral("unexpected summary: %1").arg(summary->text())));
    QVERIFY(summary->text().contains(QStringLiteral("2 sources")));
    QVERIFY(read_only->text().startsWith(QStringLiteral("In-memory draft only")));
    QVERIFY(read_only->text().contains(QStringLiteral("file writing is not enabled")));
    QCOMPARE(buttons->standardButtons(), QDialogButtonBox::Close);
    QVERIFY(buttons->button(QDialogButtonBox::Apply) == nullptr);
    QCOMPARE(files->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(files->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(files->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(!files->wordWrap());
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::EditKeyPressed));
    QVERIFY(fields->editTriggers().testFlag(QAbstractItemView::AnyKeyPressed));
    QVERIFY(!fields->wordWrap());

    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-undo"));
    auto* redo = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-redo"));
    auto* discard = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-discard"));
    auto* add_field = dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-add-field"));
    auto* remove_field =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-remove-field"));
    auto* edit_values =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-edit-values"));
    auto* preview_write_plan =
        dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-preview-write-plan"));
    QVERIFY(undo != nullptr);
    QVERIFY(redo != nullptr);
    QVERIFY(discard != nullptr);
    QVERIFY(add_field != nullptr);
    QVERIFY(remove_field != nullptr);
    QVERIFY(edit_values != nullptr);
    QVERIFY(preview_write_plan != nullptr);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
    QVERIFY(!discard->isEnabled());
    QVERIFY(!preview_write_plan->isEnabled());

    auto* grid_model = qobject_cast<MetadataGridModel*>(files->model());
    auto* aggregate_model = qobject_cast<MetadataAggregateModel*>(fields->model());
    QVERIFY(grid_model != nullptr);
    QVERIFY(aggregate_model != nullptr);
    QCOMPARE(grid_model->rowCount(), 2);
    QCOMPARE(files->selectionModel()->selectedRows(0).size(), 2);
    QCOMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QCOMPARE(aggregate_model->columnCount(), 3);
    QCOMPARE(aggregate_model->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Field"));
    QCOMPARE(aggregate_model->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Original"));
    QCOMPARE(aggregate_model->headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(),
             QStringLiteral("Draft"));
    for (auto column = 1; column < grid_model->columnCount(); ++column) {
        QVERIFY(files->isColumnHidden(column));
    }

    const auto title_column = grid_model->fieldColumn(QStringLiteral("title"));
    const auto artist_column = grid_model->fieldColumn(QStringLiteral("artist"));
    const auto date_column = grid_model->fieldColumn(QStringLiteral("date"));
    const auto genre_column = grid_model->fieldColumn(QStringLiteral("genre"));
    const auto custom_column = grid_model->fieldColumn(QStringLiteral("custom_field"));
    QVERIFY(title_column.has_value());
    QVERIFY(artist_column.has_value());
    QVERIFY(date_column.has_value());
    QVERIFY(genre_column.has_value());
    QVERIFY(custom_column.has_value());
    const auto title_row = aggregate_model->fieldRow(QStringLiteral("title"));
    const auto artist_row = aggregate_model->fieldRow(QStringLiteral("artist"));
    const auto date_row = aggregate_model->fieldRow(QStringLiteral("date"));
    const auto genre_row = aggregate_model->fieldRow(QStringLiteral("genre"));
    const auto custom_row = aggregate_model->fieldRow(QStringLiteral("custom_field"));
    QVERIFY(title_row.has_value());
    QVERIFY(artist_row.has_value());
    QVERIFY(date_row.has_value());
    QVERIFY(genre_row.has_value());
    QVERIFY(custom_row.has_value());
    QVERIFY(aggregate_model->index(*title_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("various")));
    QVERIFY(aggregate_model->index(*date_row, 1)
                .data(Qt::DisplayRole)
                .toString()
                .contains(QStringLiteral("present on 1 of 2")));
    QCOMPARE(aggregate_model->index(*genre_row, 1).data(Qt::DisplayRole).toString(),
             QStringLiteral("—"));

    // Arbitrary fields join the same Original/Draft table. The field starts
    // missing, its inline Draft edit applies to all selected files, and
    // removing it again cancels those additions because no baseline existed.
    const auto initial_field_count = aggregate_model->rowCount();
    QTRY_VERIFY(add_field->isEnabled());
    QTest::mouseClick(add_field, Qt::LeftButton);
    QInputDialog* field_prompt = nullptr;
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    QVERIFY(!add_field->isEnabled());
    auto* field_name =
        field_prompt->findChild<QLineEdit*>(QStringLiteral("bench-metadata-add-field-name"));
    auto* field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_name != nullptr);
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("alb art"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("Album Artist"));
    field_prompt->setTextValue(QStringLiteral("mb track"));
    QTRY_VERIFY(field_completer->model()->rowCount() >= 2);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(),
             QStringLiteral("MUSICBRAINZ_TRACKID"));
    field_prompt->setTextValue(QStringLiteral("Mood"));
    field_prompt->accept();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);
    QTRY_COMPARE(aggregate_model->rowCount(), initial_field_count + 1);
    const auto mood_row = aggregate_model->fieldRow(QStringLiteral("mood"));
    const auto mood_column = grid_model->fieldColumn(QStringLiteral("mood"));
    QVERIFY(mood_row.has_value());
    QVERIFY(mood_column.has_value());
    QVERIFY(files->isColumnHidden(*mood_column));
    const auto mood_draft = aggregate_model->index(*mood_row, 2);
    QTRY_VERIFY(mood_draft.flags().testFlag(Qt::ItemIsEditable));
    QTRY_COMPARE(fields->currentIndex(), mood_draft);
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* field_editor = nullptr;
    QTRY_VERIFY((field_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(field_editor, QStringLiteral("Energetic"));
    QTest::keyClick(field_editor, Qt::Key_Return);
    QTRY_VERIFY(fields->findChild<QLineEdit*>() == nullptr);
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Energetic"));
    QCOMPARE(grid_model->index(0, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QCOMPARE(grid_model->index(1, *mood_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Energetic")}));
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(!grid_model->index(0, *mood_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *mood_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(mood_draft.data(Qt::DisplayRole).toString(), QStringLiteral("—"));

    // Successful arbitrary names become session-recent completion candidates.
    QTest::mouseClick(add_field, Qt::LeftButton);
    QTRY_VERIFY((field_prompt = dialog->findChild<QInputDialog*>(
                     QStringLiteral("bench-metadata-add-field-dialog"))) != nullptr);
    field_completer =
        field_prompt->findChild<QCompleter*>(QStringLiteral("bench-metadata-field-completer"));
    QVERIFY(field_completer != nullptr);
    field_prompt->setTextValue(QStringLiteral("moo"));
    QTRY_VERIFY(field_completer->model()->rowCount() > 0);
    QCOMPARE(field_completer->model()->index(0, 0).data().toString(), QStringLiteral("Mood"));
    field_prompt->reject();
    QTRY_VERIFY(dialog->findChild<QInputDialog*>(
                    QStringLiteral("bench-metadata-add-field-dialog")) == nullptr);

    // One selected file exposes exact values in the same lower table and limits
    // edits to that source row.
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(aggregate_model->index(*title_row, 1).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Metadata Fixture"));
    QVERIFY(summary->text().contains(QStringLiteral("1 of 2 files selected")));
    const auto custom_draft = aggregate_model->index(*custom_row, 2);
    fields->setCurrentIndex(custom_draft);
    QTRY_VERIFY(edit_values->isEnabled());
    QTest::mouseClick(edit_values, Qt::LeftButton);

    QDialog* exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    auto* exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    auto* exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    auto* exact_up =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-up"));
    auto* exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_up != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 2);
    QCOMPARE(exact_table->model()->index(0, 0).data(Qt::EditRole).toString(),
             QStringLiteral("first custom value"));
    QVERIFY(
        exact_table->model()->setData(exact_table->model()->index(0, 0), QString{}, Qt::EditRole));
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QLineEdit* exact_editor = nullptr;
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("third; value"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    exact_table->setCurrentIndex(exact_table->model()->index(2, 0));
    QTest::mouseClick(exact_up, Qt::LeftButton);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);

    const auto first_custom = grid_model->index(0, *custom_column);
    const auto second_custom = grid_model->index(1, *custom_column);
    const QStringList custom_draft_values{QString{}, QStringLiteral("third; value"),
                                          QStringLiteral("second custom value")};
    QCOMPARE(first_custom.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QCOMPARE(second_custom.data(metadata_cell_values_role).toStringList(), QStringList{});
    QCOMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("1 staged change")));

    // Planning is an explicit fresh-read operation. Native FLAC now has a
    // preservation-proven text writer, but this exact draft deliberately
    // contains an empty value that TagLib would reinterpret as deletion. The
    // format mapping therefore remains visibly blocked without adding Apply.
    QTRY_VERIFY(preview_write_plan->isEnabled());
    QTest::mouseClick(preview_write_plan, Qt::LeftButton);
    QVERIFY(!preview_write_plan->isEnabled());
    QDialog* write_plan = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((write_plan = dialog->findChild<QDialog*>(
                                  QStringLiteral("bench-metadata-write-plan"))) != nullptr,
                             5'000);
    auto* write_plan_summary =
        write_plan->findChild<QLabel*>(QStringLiteral("bench-metadata-write-plan-summary"));
    auto* write_plan_table =
        write_plan->findChild<QTableView*>(QStringLiteral("bench-metadata-write-plan-table"));
    auto* write_plan_buttons = write_plan->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-write-plan-buttons"));
    QVERIFY(write_plan_summary != nullptr);
    QVERIFY(write_plan_table != nullptr);
    QVERIFY(write_plan_buttons != nullptr);
    QVERIFY(write_plan_summary->text().contains(QStringLiteral("1 staged change")));
    QVERIFY(write_plan_summary->text().contains(QStringLiteral("1 physical source")));
    QVERIFY(write_plan_summary->text().contains(QStringLiteral("blocking")));
    QCOMPARE(write_plan_buttons->standardButtons(), QDialogButtonBox::Close);
    QVERIFY(write_plan_buttons->button(QDialogButtonBox::Apply) == nullptr);
    QCOMPARE(write_plan_table->model()->rowCount(), 1);
    QCOMPARE(write_plan_table->model()->headerData(3, Qt::Horizontal).toString(),
             QStringLiteral("Fresh original"));
    QCOMPARE(write_plan_table->model()->headerData(4, Qt::Horizontal).toString(),
             QStringLiteral("Planned result"));
    QCOMPARE(write_plan_table->model()->index(0, 0).data().toString(), QStringLiteral("Blocked"));
    QCOMPARE(write_plan_table->model()->index(0, 2).data().toString(),
             QStringLiteral("CUSTOM_FIELD"));
    QCOMPARE(write_plan_table->model()->index(0, 4).data().toString(),
             QStringLiteral("(empty value)  ·  third; value  ·  second custom value"));
    QVERIFY(write_plan_table->model()
                ->index(0, 0)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("unsupported field mapping")));
    write_plan->close();
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-write-plan")) ==
                nullptr);
    QTRY_VERIFY(preview_write_plan->isEnabled());

    // Switching the file scope away and back reconstructs the selected file's
    // exact draft. With both files selected, the background result preview
    // reports the real partial state instead of a staged-count placeholder.
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(custom_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::partial));
    QCOMPARE(custom_draft.data(Qt::DisplayRole).toString(),
             QStringLiteral("(various · present on 1 of 2 files)"));
    QVERIFY(
        custom_draft.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Complete draft")));
    files->selectionModel()->select(grid_model->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                 QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    QTRY_COMPARE(custom_draft.data(metadata_cell_values_role).toStringList(), custom_draft_values);
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!first_custom.data(metadata_cell_staged_role).toBool());
    QCOMPARE(
        first_custom.data(metadata_cell_values_role).toStringList(),
        (QStringList{QStringLiteral("first custom value"), QStringLiteral("second custom value")}));

    // A per-file exception can also make previously mixed values converge.
    // The complete preview exposes the resulting exact common value.
    const auto second_title_values =
        grid_model->index(1, *title_column).data(metadata_cell_values_role).toStringList();
    QCOMPARE(second_title_values.size(), 1);
    const auto title_draft = aggregate_model->index(*title_row, 2);
    QVERIFY(aggregate_model->setData(title_draft, second_title_values.front(), Qt::EditRole));
    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::common));
    QCOMPARE(title_draft.data(metadata_cell_values_role).toStringList(), second_title_values);
    QCOMPARE(title_draft.data(Qt::DisplayRole).toString(), second_title_values.front());
    fields->setCurrentIndex(title_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!grid_model->index(0, *title_column).data(metadata_cell_staged_role).toBool());
    QTRY_VERIFY(aggregate_model->draftPreviewReady());
    QTRY_COMPARE(title_draft.data(metadata_field_state_role).toInt(),
                 static_cast<int>(metadata::MetadataSelectionFieldState::mixed));

    // Selecting both rows makes the same editor a bulk editor.
    QTRY_VERIFY(aggregate_model->index(*title_row, 1)
                    .data(Qt::DisplayRole)
                    .toString()
                    .contains(QStringLiteral("various")));
    const auto artist_draft = aggregate_model->index(*artist_row, 2);
    fields->setCurrentIndex(artist_draft);
    QTest::mouseClick(edit_values, Qt::LeftButton);
    exact_dialog = nullptr;
    QTRY_VERIFY((exact_dialog = dialog->findChild<QDialog*>(
                     QStringLiteral("bench-metadata-exact-values"))) != nullptr);
    exact_table =
        exact_dialog->findChild<QTableView*>(QStringLiteral("bench-metadata-exact-values-table"));
    exact_add =
        exact_dialog->findChild<QPushButton*>(QStringLiteral("bench-metadata-exact-values-add"));
    exact_buttons = exact_dialog->findChild<QDialogButtonBox*>(
        QStringLiteral("bench-metadata-exact-values-buttons"));
    QVERIFY(exact_table != nullptr);
    QVERIFY(exact_add != nullptr);
    QVERIFY(exact_buttons != nullptr);
    QCOMPARE(exact_table->model()->rowCount(), 0);
    QVERIFY(!exact_buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Lead Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_add, Qt::LeftButton);
    QTRY_VERIFY((exact_editor = exact_table->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(exact_editor, QStringLiteral("Guest Artist"));
    QTest::keyClick(exact_editor, Qt::Key_Return);
    QTRY_VERIFY(exact_table->findChild<QLineEdit*>() == nullptr);
    QTest::mouseClick(exact_buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(dialog->findChild<QDialog*>(QStringLiteral("bench-metadata-exact-values")) ==
                nullptr);
    const QStringList artist_draft_values{QStringLiteral("Lead Artist"),
                                          QStringLiteral("Guest Artist")};
    QCOMPARE(grid_model->index(0, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(grid_model->index(1, *artist_column).data(metadata_cell_values_role).toStringList(),
             artist_draft_values);
    QCOMPARE(artist_draft.data(metadata_cell_values_role).toStringList(), artist_draft_values);
    QVERIFY(summary->text().contains(QStringLiteral("2 staged changes")));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!grid_model->index(0, *artist_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *artist_column).data(metadata_cell_staged_role).toBool());

    const auto genre_draft = aggregate_model->index(*genre_row, 2);
    fields->setCurrentIndex(genre_draft);
    fields->setFocus();
    QTest::keyClick(fields, Qt::Key_F2);
    QLineEdit* aggregate_editor = nullptr;
    QTRY_VERIFY((aggregate_editor = fields->findChild<QLineEdit*>()) != nullptr);
    QTest::keyClicks(aggregate_editor, QStringLiteral("Rock"));
    QTest::keyClick(aggregate_editor, Qt::Key_Return);
    QTRY_COMPARE(genre_draft.data(Qt::DisplayRole).toString(), QStringLiteral("Rock"));
    QCOMPARE(grid_model->index(0, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QCOMPARE(grid_model->index(1, *genre_column).data(metadata_cell_values_role).toStringList(),
             (QStringList{QStringLiteral("Rock")}));
    QTest::keyClick(fields, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!grid_model->index(0, *genre_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(1, *genre_column).data(metadata_cell_staged_role).toBool());

    // Delete and revert are also scoped by the file list.
    const auto date_file = grid_model->index(0, *date_column)
                                   .data(metadata_cell_baseline_values_role)
                                   .toStringList()
                                   .isEmpty()
                               ? 1
                               : 0;
    const auto other_file = 1 - date_file;
    files->selectionModel()->select(grid_model->index(date_file, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{1U});
    const auto date_draft = aggregate_model->index(*date_row, 2);
    fields->setCurrentIndex(date_draft);
    fields->selectionModel()->select(date_draft, QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    QTRY_VERIFY(remove_field->isEnabled());
    QTest::mouseClick(remove_field, Qt::LeftButton);
    QTRY_VERIFY(
        grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());
    QVERIFY(!grid_model->index(other_file, *date_column).data(metadata_cell_staged_role).toBool());
    QCOMPARE(date_draft.data(Qt::DisplayRole).toString(), QStringLiteral("(remove)"));
    QTest::keyClick(fields, Qt::Key_Backspace, Qt::ControlModifier);
    QTRY_VERIFY(
        !grid_model->index(date_file, *date_column).data(metadata_cell_staged_role).toBool());

    files->clearSelection();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{0U});
    QTRY_VERIFY(!add_field->isEnabled());
    QVERIFY(!remove_field->isEnabled());
    QTRY_VERIFY(!edit_values->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("0 of 2 files selected")));

    files->selectAll();
    QTRY_COMPARE(aggregate_model->selectedItemCount(), std::size_t{2U});
    QTRY_VERIFY(add_field->isEnabled());
    QVERIFY(edit_values->isEnabled());
    QVERIFY(!summary->text().contains(QStringLiteral("staged change")));
    QVERIFY(!discard->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection,
                                      Q_ARG(int, tabs->currentIndex())));
    QTRY_COMPARE(tabs->count(), 2);
}

void BenchMainWindowTest::cueSheetsExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto wave_path = media.filePath(QStringLiteral("disc.wav"));
    const auto cue_path = media.filePath(QStringLiteral("album.cue"));
    write_wave(wave_path, wave_sample_rate * 2U);
    {
        std::ofstream cue{QFile::encodeName(cue_path).toStdString(), std::ios::binary};
        cue << "REM DATE 2026\n"
               "PERFORMER \"Cue Artist\"\n"
               "TITLE \"Cue Album\"\n"
               "FILE \"disc.wav\" WAVE\n"
               "TRACK 01 AUDIO\n"
               "TITLE \"First Cue Track\"\n"
               "INDEX 01 00:00:00\n"
               "TRACK 02 AUDIO\n"
               "TITLE \"Second Cue Track\"\n"
               "INDEX 01 00:01:00\n";
    }
    const auto folder_encoded = QFile::encodeName(media.path());
    const std::string raw_folder{folder_encoded.constData(),
                                 static_cast<std::size_t>(folder_encoded.size())};
    const auto canonical_wave =
        std::filesystem::canonical(
            std::filesystem::path{QFile::encodeName(wave_path).toStdString()})
            .native();

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        // The referenced disc.wav is represented by its two cue tracks, not
        // by an additional whole-file row from the same folder scan.
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        QTRY_COMPARE(view->rowHeight(0), view->verticalHeader()->defaultSectionSize() +
                                             ui::QueueItemDelegate::album_header_height);
        QCOMPARE(view->rowHeight(1), view->verticalHeader()->defaultSectionSize());
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, canonical_wave);
        QCOMPARE(rows[1].raw_path, canonical_wave);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{44'100});
        QCOMPARE(rows[1].segment->start_sample, 44'100);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{88'200});
        QCOMPARE(rows[0].title, std::string{"First Cue Track"});
        QCOMPARE(rows[1].title, std::string{"Second Cue Track"});
        QCOMPARE(rows[0].artist, std::string{"Cue Artist"});
        QCOMPARE(rows[0].album, std::string{"Cue Album"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{1'000});
        QCOMPARE(rows[0].metadata.first_effective_value("title"),
                 std::optional<std::string>{"First Cue Track"});
        QCOMPARE(rows[0].metadata.first_effective_value("album_artist"),
                 std::optional<std::string>{"Cue Artist"});
        QVERIFY(rows[0].source_revision.has_value());
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    auto* restored_view = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(restored_view != nullptr);
    QTRY_COMPARE(restored_view->rowHeight(0),
                 restored_view->verticalHeader()->defaultSectionSize() +
                     ui::QueueItemDelegate::album_header_height);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{44'100});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First Cue Track"});
    QCOMPARE(model->rows()[1].title, std::string{"Second Cue Track"});
}

void BenchMainWindowTest::containerChaptersExpandIntoPersistentSegmentRows() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto container_path = media.filePath(QStringLiteral("chaptered.mka"));
    QVERIFY(
        materialize_audio_fixture(QStringLiteral("container-chapters-mka.b64"), container_path));
    const auto encoded = QFile::encodeName(container_path);
    const std::string raw_path{encoded.constData(), static_cast<std::size_t>(encoded.size())};

    {
        BenchMainWindow window;
        window.show();
        window.openLocalPaths({raw_path});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        QCOMPARE(rows[0].raw_path, raw_path);
        QCOMPARE(rows[1].raw_path, raw_path);
        QVERIFY(rows[0].logical_reference.has_value());
        QVERIFY(rows[1].logical_reference.has_value());
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(rows[0].segment.has_value());
        QVERIFY(rows[1].segment.has_value());
        QCOMPARE(rows[0].segment->start_sample, 0);
        QCOMPARE(rows[0].segment->end_sample, std::optional<std::int64_t>{4'800});
        QCOMPARE(rows[1].segment->start_sample, 4'800);
        QCOMPARE(rows[1].segment->end_sample, std::optional<std::int64_t>{9'600});
        QCOMPARE(rows[0].title, std::string{"First chapter"});
        QCOMPARE(rows[1].title, std::string{"Second chapter"});
        QCOMPARE(rows[0].artist, std::string{"First Artist"});
        QCOMPARE(rows[1].artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].album, std::string{"Chapter Album"});
        QCOMPARE(rows[0].album_artist, std::string{"Album Artist"});
        QCOMPARE(rows[0].date, std::string{"2026"});
        QCOMPARE(rows[0].track_number, std::string{"1"});
        QCOMPARE(rows[1].track_number, std::string{"2"});
        QCOMPARE(rows[0].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[1].duration_ms, std::optional<std::int64_t>{100});
        QCOMPARE(rows[0].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"First Artist"});
        QCOMPARE(rows[1].metadata.first_effective_value("artist"),
                 std::optional<std::string>{"Album Artist"});
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QVERIFY(model->rows()[0].segment.has_value());
    QCOMPARE(model->rows()[0].segment->start_sample, 0);
    QCOMPARE(model->rows()[0].segment->end_sample, std::optional<std::int64_t>{4'800});
    QVERIFY(model->rows()[1].logical_reference.has_value());
    QCOMPARE(model->rows()[0].metadata.first_effective_value("title"),
             std::optional<std::string>{"First chapter"});
}

void BenchMainWindowTest::codecNativeSubsongsExpandAndPersistDecoderSelections() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const auto module_path = media.filePath(QStringLiteral("two-songs.mod"));
    QVERIFY(materialize_audio_fixture(QStringLiteral("two-subsongs-mod.b64"), module_path));
    const auto encoded_folder = QFile::encodeName(media.path());
    const std::string raw_folder{encoded_folder.constData(),
                                 static_cast<std::size_t>(encoded_folder.size())};
    const auto encoded_path = QFile::encodeName(module_path);
    const std::string raw_path{encoded_path.constData(),
                               static_cast<std::size_t>(encoded_path.size())};

    {
        BenchMainWindow window;
        window.show();
        // Folder intake proves tracker extensions participate in bounded
        // discovery rather than working only through explicit file opens.
        window.openLocalPaths({raw_folder});
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        QVERIFY(tabs != nullptr);
        QTRY_COMPARE(tabs->count(), 2);
        auto* view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(view != nullptr);
        auto* model = qobject_cast<LocalListModel*>(view->model());
        QVERIFY(model != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, 5'000);
        const auto& rows = model->rows();
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            QCOMPARE(rows[index].raw_path, raw_path);
            QVERIFY(rows[index].logical_reference.has_value());
            QCOMPARE(rows[index].selection.stream_index, std::optional<int>{0});
            QCOMPARE(rows[index].selection.subsong_index,
                     std::optional<int>{static_cast<int>(index)});
            QVERIFY(rows[index].segment.has_value());
            QCOMPARE(rows[index].segment->start_sample, 0);
            QCOMPARE(rows[index].segment->end_sample, std::optional<std::int64_t>{28'800});
            QCOMPARE(rows[index].title, "Subsong " + std::to_string(index + 1U));
            QCOMPARE(rows[index].album, std::string{"Trackknife subsongs"});
            QCOMPARE(rows[index].duration_ms, std::optional<std::int64_t>{600});
            QCOMPARE(rows[index].metadata.first_effective_value("title"),
                     std::optional<std::string>{"Subsong " + std::to_string(index + 1U)});
        }
        QVERIFY(rows[0].logical_reference != rows[1].logical_reference);
        QVERIFY(window.close());
    }

    BenchMainWindow restored;
    restored.show();
    auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QVERIFY(tabs != nullptr);
    QTRY_COMPARE(tabs->count(), 2);
    auto* model =
        qobject_cast<LocalListModel*>(qobject_cast<QTableView*>(tabs->currentWidget())->model());
    QVERIFY(model != nullptr);
    QTRY_COMPARE(model->rowCount(), 2);
    QCOMPARE(model->rows()[0].selection.subsong_index, std::optional<int>{0});
    QCOMPARE(model->rows()[1].selection.subsong_index, std::optional<int>{1});
    QVERIFY(model->rows()[1].segment.has_value());
    QCOMPARE(model->rows()[1].segment->end_sample, std::optional<std::int64_t>{28'800});
    QCOMPARE(model->rows()[1].metadata.first_effective_value("title"),
             std::optional<std::string>{"Subsong 2"});
}

// Regression for the runaway auto-advance: the player's "ended" state names a
// finished *file*, not a finished list, and persists for several transport
// ticks while the next source loads. Each finished track must advance the
// list exactly one row, and the last row must stay ended without wrapping
// (ADR-0023).
void BenchMainWindowTest::autoAdvancesOncePerFinishedTrack() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    const std::array names{QStringLiteral("a.wav"), QStringLiteral("b.wav"),
                           QStringLiteral("c.wav")};
    std::vector<std::string> raw_paths;
    for (const auto& name : names) {
        const auto path = media.filePath(name);
        write_wave(path, wave_sample_rate * 2U);
        const auto encoded = QFile::encodeName(path);
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }

    BenchMainWindow window;
    window.show();
    window.openLocalPaths(raw_paths);

    QTableView* view = nullptr;
    const QDeadlineTimer view_deadline{5'000};
    while (view == nullptr && !view_deadline.hasExpired()) {
        const auto views = window.findChildren<QTableView*>();
        for (auto* candidate : views) {
            if (qobject_cast<LocalListModel*>(candidate->model()) != nullptr &&
                candidate->model()->rowCount() == 3) {
                view = candidate;
                break;
            }
        }
        if (view == nullptr) {
            QTest::qWait(50);
        }
    }
    QVERIFY(view != nullptr);
    if (view == nullptr) {
        return;
    }
    auto* model = qobject_cast<LocalListModel*>(view->model());
    QVERIFY(model != nullptr);
    const auto current = [model](const int row) {
        return model->index(row, 0).data(ui::track_current_role).toBool();
    };

    view->selectionModel()->setCurrentIndex(
        model->index(0, 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);

    // Without a live PipeWire server the load fails and no row ever becomes
    // current; that environment cannot exercise progression.
    const auto deadline = QDeadlineTimer{3'000};
    while (!current(0) && !current(1) && !deadline.hasExpired()) {
        QTest::qWait(50);
    }
    if (!current(0) && !current(1)) {
        QSKIP("live PipeWire playback unavailable");
    }

    // Each two-second track advances exactly one row, and transitions are
    // gapless: the player never reports "ended" (7) between tracks — that
    // state may only appear once the final track finishes.
    bool ended_between_tracks = false;
    {
        const QDeadlineTimer advance_deadline{5'000};
        int last_state = -1;
        int ticks = 0;
        while (!current(1) && !advance_deadline.hasExpired()) {
            const auto state = window.property("trackbench-player-state").toInt();
            ended_between_tracks = ended_between_tracks || state == 7;
            if (state != last_state || ++ticks % 20 == 0) {
                qInfo() << "player state" << state << "position"
                        << window.property("trackbench-player-position").toLongLong() << "buffered"
                        << window.property("trackbench-player-buffered").toLongLong() << "callbacks"
                        << window.property("trackbench-player-callbacks").toLongLong() << "output"
                        << window.property("trackbench-player-outputstate").toInt();
                last_state = state;
            }
            QTest::qWait(50);
        }
    }
    QVERIFY(current(1));
    QTest::qWait(300);
    QVERIFY(current(1));
    QVERIFY(!current(2));
    {
        const QDeadlineTimer second_deadline{5'000};
        while (!current(2) && !second_deadline.hasExpired()) {
            ended_between_tracks =
                ended_between_tracks || window.property("trackbench-player-state").toInt() == 7;
            QTest::qWait(50);
        }
    }
    QVERIFY(current(2));
    QVERIFY(!ended_between_tracks);

    // The end of the list stays ended: no wrap-around back to the first row.
    QTest::qWait(2'700);
    QVERIFY(current(2));
    QVERIFY(!current(0));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::BenchMainWindowTest)

#include "bench_main_window_test.moc"
