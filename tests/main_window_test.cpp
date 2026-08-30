// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "quick/mpd_search_result_model.hpp"
#include "ui/format_sandbox.hpp"
#include "ui/main_window.hpp"
#include "ui/mpd_connection_dialog.hpp"
#include "ui/server_library_tree_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QBuffer>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFile>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSizePolicy>
#include <QSlider>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace trackknife::ui {
namespace {

[[nodiscard]] mpd::Track albumTrack(const std::uint32_t id, std::string album, std::string title,
                                    std::string track_number) {
    return mpd::Track{
        .uri = title + ".flac",
        .metadata = mpd::Metadata{{
            {"Artist", "Artist"},
            {"AlbumArtist", "Artist"},
            {"Album", std::move(album)},
            {"Date", "2000"},
            {"Track", std::move(track_number)},
            {"Title", std::move(title)},
        }},
        .musicbrainz = {},
        .queue_id = id,
        .queue_position = id - 1U,
        .duration = std::nullopt,
        .last_modified = std::nullopt,
        .audio_format = std::nullopt,
        .priority = std::nullopt,
        .unknown_structural_pairs = {},
    };
}

} // namespace

class MainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void shipsExpectedNativeWorkspace();
    void persistsMovedDockLayout();
    void persistsLibraryDockWidth();
    void resetsMovedDockLayout();
    void recoversFromCorruptState();
    void opensWorkingFormatSandbox();
    void opensMpdConnectionDialog();
    void connectionDialogAcceptsEnter();
    void autoConnectsLastEndpoint();
    void searchUsesTransientSurface();
    void submittedSearchesCreateIndependentTabs();
    void storedPlaylistsOpenInReusableTabs();
    void browsesStoredPlaylistsWithMouseAndKeyboard();
    void scratchTabsPersistLifecycle();
    void editsAndDuplicatesPersistentWorkingLists();
    void commandPaletteDiscoversAndPersistsShortcuts();
    void decodesArtworkOffTheUiPath();
    void persistsTrackViewPresetsTransactionally();
    void usesGroupedCantataStyleQueue();
    void mapsDropPositionsToInsertionRows();
    void rendersNonzeroQueuePriority();
    void albumHeaderSelectsAlbumTracks();
    void usesCapabilityAwareContextMenus();
    void usesNativePlaybackMenus();
    void showsTransientFailuresAsToasts();

  private:
    QTemporaryDir settings_directory_;
};

void MainWindowTest::initTestCase() {
    QVERIFY(settings_directory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WorkspaceTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
}

void MainWindowTest::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
    qApp->setProperty("trackknife-state-database-path", QVariant{});
}

void MainWindowTest::shipsExpectedNativeWorkspace() {
    MainWindow window(1'000'000);
    auto* library = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    auto* selection = window.findChild<QDockWidget*>(QStringLiteral("panel-selection"));
    auto* jobs = window.findChild<QDockWidget*>(QStringLiteral("panel-jobs"));
    auto* transport = window.findChild<QToolBar*>(QStringLiteral("toolbar-transport"));
    auto* progress = window.findChild<QToolBar*>(QStringLiteral("toolbar-progress"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("global-search"));
    auto* search_spacer = window.findChild<QWidget*>(QStringLiteral("toolbar-search-spacer"));
    auto* seek = window.findChild<QSlider*>(QStringLiteral("seek-slider"));
    auto* volume = window.findChild<QSlider*>(QStringLiteral("volume-slider"));
    auto* library_sources = window.findChild<QTabWidget*>(QStringLiteral("library-source-tabs"));
    QVERIFY(library != nullptr);
    QVERIFY(selection != nullptr);
    QVERIFY(transport != nullptr);
    QVERIFY(progress != nullptr);
    QVERIFY(search != nullptr);
    QVERIFY(search_spacer != nullptr);
    QVERIFY(seek != nullptr);
    QVERIFY(volume != nullptr);
    QVERIFY(library_sources != nullptr);
    QCOMPARE(library_sources->count(), 1);
    QCOMPARE(library_sources->tabText(0), QStringLiteral("Server"));
    QCOMPARE(library->windowTitle(), QStringLiteral("Library"));
    QCOMPARE(seek->accessibleName(), QStringLiteral("Playback position"));
    QCOMPARE(volume->accessibleName(), QStringLiteral("Volume"));
    QVERIFY(jobs != nullptr);
    QCOMPARE(window.dockWidgetArea(library), Qt::LeftDockWidgetArea);
    QCOMPARE(window.toolBarArea(transport), Qt::TopToolBarArea);
    QCOMPARE(window.toolBarArea(progress), Qt::TopToolBarArea);
    QVERIFY(window.toolBarBreak(progress));
    QVERIFY(!transport->isMovable());
    QVERIFY(!progress->isMovable());
    QCOMPARE(search_spacer->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
    window.show();
    QTRY_VERIFY(search->mapTo(transport, QPoint{}).x() > transport->width() / 2);
    auto* now_playing = window.findChild<QLabel*>(QStringLiteral("now-playing"));
    auto* queue_summary = window.findChild<QLabel*>(QStringLiteral("queue-summary"));
    QVERIFY(now_playing != nullptr);
    QVERIFY(queue_summary != nullptr);
    QVERIFY(now_playing->parentWidget() != window.statusBar());
    QCOMPARE(queue_summary->parentWidget(), window.statusBar());
}

void MainWindowTest::persistsMovedDockLayout() {
    {
        MainWindow window(100);
        auto* library = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
        window.addDockWidget(Qt::RightDockWidgetArea, library);
        window.show();
        QVERIFY(window.close());
    }

    MainWindow restored(100);
    auto* library = restored.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    QCOMPARE(restored.dockWidgetArea(library), Qt::RightDockWidgetArea);
}

void MainWindowTest::persistsLibraryDockWidth() {
    int saved_width = 0;
    {
        MainWindow window(100);
        auto* library = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
        window.show();
        window.resizeDocks({library}, {410}, Qt::Horizontal);
        QTRY_VERIFY(library->width() >= 400);
        saved_width = library->width();
        QVERIFY(window.close());
    }

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("workspace/library-dock-width")).toInt(), saved_width);

    MainWindow restored(100);
    auto* library = restored.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    restored.show();
    QTRY_VERIFY(std::abs(library->width() - saved_width) <= 2);
}

void MainWindowTest::resetsMovedDockLayout() {
    MainWindow window(100);
    auto* library = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    auto* reset = window.findChild<QAction*>(QStringLiteral("action-reset-workspace"));
    QVERIFY(reset != nullptr);
    window.addDockWidget(Qt::RightDockWidgetArea, library);
    QCOMPARE(window.dockWidgetArea(library), Qt::RightDockWidgetArea);
    reset->trigger();
    QCOMPARE(window.dockWidgetArea(library), Qt::LeftDockWidgetArea);
}

void MainWindowTest::recoversFromCorruptState() {
    QSettings settings;
    settings.setValue(QStringLiteral("workspace/state"), QByteArrayLiteral("not a Qt layout"));
    settings.sync();

    MainWindow window(100);
    auto* library = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    QCOMPARE(window.dockWidgetArea(library), Qt::LeftDockWidgetArea);
    QVERIFY(!settings.contains(QStringLiteral("workspace/state")));
}

void MainWindowTest::opensWorkingFormatSandbox() {
    MainWindow window(100);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-format-sandbox"));
    QVERIFY(action != nullptr);
    action->trigger();

    auto* sandbox = window.findChild<FormatSandboxDialog*>(QStringLiteral("format-sandbox"));
    QVERIFY(sandbox != nullptr);
    auto* source = sandbox->findChild<QPlainTextEdit*>(QStringLiteral("format-source"));
    auto* host = sandbox->findChild<QComboBox*>(QStringLiteral("format-host"));
    auto* preview = sandbox->findChild<QPlainTextEdit*>(QStringLiteral("format-preview"));
    auto* status = sandbox->findChild<QLabel*>(QStringLiteral("format-status"));
    QVERIFY(source != nullptr);
    QVERIFY(host != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(
        preview->toPlainText().contains(QStringLiteral("Talk Talk/Spirit of Eden/02 - Eden.flac")));

    host->setCurrentIndex(2);
    source->setPlainText(QStringLiteral("$each(genre)"));
    QVERIFY(status->text().contains(QStringLiteral("2 result")));
    QVERIFY(preview->toPlainText().contains(QStringLiteral("[0] Art Rock")));
    QVERIFY(preview->toPlainText().contains(QStringLiteral("[1] Post-Rock")));

    source->setPlainText(QStringLiteral("$set(title,bad)"));
    QCOMPARE(status->text(), QStringLiteral("Cannot evaluate"));
    QVERIFY(preview->toPlainText().contains(QStringLiteral("unknown format function")));
    sandbox->close();
}

void MainWindowTest::opensMpdConnectionDialog() {
    MainWindow window(100);
    auto* action = window.findChild<QAction*>(QStringLiteral("action-connect-mpd"));
    QVERIFY(action != nullptr);
    action->trigger();

    auto* dialog = window.findChild<MpdConnectionDialog*>(QStringLiteral("mpd-connection-dialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("mpd-host")) != nullptr);
    QVERIFY(dialog->findChild<QPushButton*>(QStringLiteral("mpd-connect")) != nullptr);
    QVERIFY(dialog->findChild<QPushButton*>(QStringLiteral("mpd-new-profile")) != nullptr);
    auto* status = dialog->findChild<QLabel*>(QStringLiteral("mpd-connection-status"));
    QVERIFY(status != nullptr);
    QVERIFY(status->text().contains(QStringLiteral("Enter connects")));
    dialog->close();
}

void MainWindowTest::connectionDialogAcceptsEnter() {
    MpdConnectionDialog dialog;
    auto* host = dialog.findChild<QLineEdit*>(QStringLiteral("mpd-host"));
    QVERIFY(host != nullptr);
    host->setText(QStringLiteral("example.test"));
    QSignalSpy requested(&dialog, &MpdConnectionDialog::connectionRequested);

    dialog.show();
    host->setFocus();
    QTest::keyClick(host, Qt::Key_Return);

    QTRY_COMPARE(requested.count(), 1);
    QCOMPARE(dialog.result(), QDialog::Accepted);
    QVERIFY(core::StableId::parse(requested.constFirst().at(0).toString().toStdString()));
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("Default"));
    QCOMPARE(requested.constFirst().at(2).toString(), QStringLiteral("example.test"));
    QVERIFY(requested.constFirst().at(6).toBool());
}

void MainWindowTest::autoConnectsLastEndpoint() {
    const auto database_path = settings_directory_.filePath(QStringLiteral("profiles.sqlite3"));
    QFile::remove(database_path);
    auto repository = persistence::ListRepository::open(
        std::filesystem::path{QFile::encodeName(database_path).toStdString()});
    QVERIFY(repository);
    const auto profile_id = core::StableId::random();
    const std::array profiles{persistence::ConnectionProfile{
        .id = profile_id,
        .name = "Local",
        .host = "127.0.0.1",
        .port = 1U,
        .local_music_root = std::string{"/music"},
        .auto_connect = true,
    }};
    QVERIFY(repository->replace_profiles(profiles));
    qApp->setProperty("trackknife-state-database-path", database_path);

    MainWindow window(0);
    auto* controller = window.findChild<quick::MpdProbeController*>();
    QVERIFY(controller != nullptr);
    QSignalSpy changed(controller, &quick::MpdProbeController::stateChanged);

    window.show();
    QTRY_VERIFY(changed.count() > 0);
    QVERIFY(controller->status() != QStringLiteral("Disconnected"));
    QCOMPARE(controller->profileId(), QString::fromStdString(profile_id.to_string()));
}

void MainWindowTest::searchUsesTransientSurface() {
    MainWindow window(100);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
    auto* center = window.findChild<QStackedWidget*>(QStringLiteral("center-stack"));
    auto* search_page = window.findChild<QWidget*>(QStringLiteral("live-search-surface"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("global-search"));
    QVERIFY(tabs != nullptr);
    QVERIFY(center != nullptr);
    QVERIFY(search_page != nullptr);
    QVERIFY(search != nullptr);
    QCOMPARE(tabs->indexOf(search_page), -1);
    QCOMPARE(tabs->count(), 2);

    window.show();
    search->setFocus();
    QTest::keyClicks(search, QStringLiteral("Slayer"));

    QTRY_VERIFY(search_page->isVisible());
    QCOMPARE(center->currentWidget(), tabs);
    auto* queue_view = window.findChild<QTableView*>(QStringLiteral("track-view-queue"));
    QVERIFY(queue_view != nullptr);
    auto* search_view = window.findChild<QTableView*>(QStringLiteral("track-view-live-search"));
    QVERIFY(search_view != nullptr);
    QVERIFY(!search_view->showGrid());
    QVERIFY(search_view->verticalHeader()->isHidden());
    QCOMPARE(search_view->iconSize(), QSize(30, 30));
    QVERIFY(!search_view->horizontalHeader()->stretchLastSection());
    QCOMPARE(search_view->horizontalHeader()->sectionResizeMode(1), QHeaderView::Stretch);
    QCOMPARE(search_view->columnWidth(4), 24);
    QCOMPARE(search_view->columnWidth(5), 24);
    QCOMPARE(search_view->columnWidth(6), 24);

    auto* model = window.findChild<quick::MpdSearchResultModel*>();
    QVERIFY(model != nullptr);
    model->replaceTracks(
        {albumTrack(1U, "First album", "One", "1"), albumTrack(2U, "First album", "Two", "2")});

    const auto action_index = model->index(1, 4);
    QStyleOptionViewItem option;
    option.rect = QRect{0, 0, 30, 22};
    option.widget = search_view;
    QImage action_image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    action_image.fill(Qt::transparent);
    {
        QPainter painter(&action_image);
        search_view->itemDelegateForColumn(4)->paint(&painter, option, action_index);
    }
    QImage empty_image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    empty_image.fill(Qt::transparent);
    {
        QPainter painter(&empty_image);
        QStyledItemDelegate empty_delegate;
        empty_delegate.paint(&painter, option, action_index);
    }
    QVERIFY(action_image != empty_image);

    QTest::keyClick(search, Qt::Key_Down);
    QTRY_COMPARE(QApplication::focusWidget(), search_view);
    QCOMPARE(search_view->currentIndex().row(), 1);
    QCOMPARE(search_view->currentIndex().column(), 1);
    QCOMPARE(search_view->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(search_view->selectionModel()->selectedRows().constFirst().row(), 1);
    QImage active_action_image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    active_action_image.fill(Qt::transparent);
    {
        QPainter painter(&active_action_image);
        search_view->itemDelegateForColumn(4)->paint(&painter, option, action_index);
    }
    QVERIFY(active_action_image != action_image);
    QTest::keyClick(search_view, Qt::Key_Right);
    QCOMPARE(search_view->currentIndex().column(), 5);
    QTest::keyClick(search_view, Qt::Key_Right);
    QCOMPARE(search_view->currentIndex().column(), 6);
    QTest::keyClick(search_view, Qt::Key_Left);
    QCOMPARE(search_view->currentIndex().column(), 5);
    QTest::keyClick(search_view, Qt::Key_Left);
    QCOMPARE(search_view->currentIndex().column(), 1);
    QTest::keyClick(search_view, Qt::Key_X);
    QTRY_COMPARE(QApplication::focusWidget(), search);
    QCOMPARE(search->text(), QStringLiteral("Slayerx"));
    search_view->setFocus();
    QTest::keyClick(search_view, Qt::Key_Backspace);
    QTRY_COMPARE(QApplication::focusWidget(), search);
    QCOMPARE(search->text(), QStringLiteral("Slayer"));
    QTest::keyClick(search_view, Qt::Key_Escape);
    QTRY_COMPARE(center->currentWidget(), tabs);
    QTRY_VERIFY(!search_page->isVisible());
    QTRY_COMPARE(QApplication::focusWidget(), queue_view);

    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(search_page->isVisible());
    QCOMPARE(center->currentWidget(), tabs);
    QTRY_COMPARE(QApplication::focusWidget(), search_view);
    QCOMPARE(search_view->currentIndex().row(), 1);
    QTest::keyClick(search_view, Qt::Key_Down);
    QCOMPARE(search_view->currentIndex().row(), 3);
    QTest::keyClick(search_view, Qt::Key_Escape);
    QTRY_COMPARE(center->currentWidget(), tabs);
    QTRY_VERIFY(!search_page->isVisible());

    auto* scratch_view = window.findChild<QTableView*>(QStringLiteral("track-view-library"));
    QVERIFY(scratch_view != nullptr);
    tabs->setCurrentWidget(scratch_view);
    const auto hovered = scratch_view->model()->index(4, 1);
    QVERIFY(hovered.isValid());
    const auto hover_position = scratch_view->visualRect(hovered).center();
    QMouseEvent hover_event{QEvent::MouseMove,
                            QPointF{hover_position},
                            QPointF{scratch_view->viewport()->mapToGlobal(hover_position)},
                            Qt::NoButton,
                            Qt::NoButton,
                            Qt::NoModifier};
    QApplication::sendEvent(scratch_view->viewport(), &hover_event);
    QTRY_COMPARE(scratch_view->property("trackknife-hover-row").toInt(), 4);
}

void MainWindowTest::submittedSearchesCreateIndependentTabs() {
    MainWindow window(100);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
    auto* preview = window.findChild<QWidget*>(QStringLiteral("live-search-surface"));
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("global-search"));
    QVERIFY(tabs != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(search != nullptr);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->indexOf(preview), -1);
    QCOMPARE(preview->layout()->contentsMargins(), QMargins());

    window.show();
    search->setText(QStringLiteral("Slayer"));
    QTest::keyClick(search, Qt::Key_Return);
    QCOMPARE(tabs->count(), 2);
    QTest::keyClick(search, Qt::Key_Return, Qt::ShiftModifier);

    QCOMPARE(tabs->count(), 3);
    auto* first_result = tabs->currentWidget();
    QVERIFY(first_result != preview);
    QVERIFY(first_result->property("trackknife-search-tab").toBool());
    QVERIFY(tabs->tabText(tabs->indexOf(first_result)).contains(QStringLiteral("Slayer")));
    auto* first_view =
        first_result->findChild<QTableView*>(QStringLiteral("track-view-search-result"));
    QVERIFY(first_view != nullptr);
    QVERIFY(!first_view->showGrid());
    QVERIFY(first_view->verticalHeader()->isHidden());
    QCOMPARE(first_view->horizontalHeader()->sectionResizeMode(1), QHeaderView::Stretch);
    QCOMPARE(first_result->layout()->contentsMargins(), QMargins());
    QVERIFY(tabs->tabBar()->tabButton(tabs->indexOf(first_result), QTabBar::RightSide) != nullptr);

    search->setText(QStringLiteral("Morbid Angel"));
    QTest::keyClick(search, Qt::Key_Return, Qt::ShiftModifier);

    QCOMPARE(tabs->count(), 4);
    QVERIFY(tabs->currentWidget() != first_result);
    QVERIFY(tabs->tabText(tabs->currentIndex()).contains(QStringLiteral("Morbid Angel")));
    QVERIFY(tabs->indexOf(first_result) >= 0);
}

void MainWindowTest::storedPlaylistsOpenInReusableTabs() {
    MainWindow window(100);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    QVERIFY(tabs != nullptr);
    QVERIFY(controller != nullptr);
    QCOMPARE(tabs->count(), 2);

    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistLoaded", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Road mix"))));
    QCOMPARE(tabs->count(), 3);
    auto* road_mix = tabs->currentWidget();
    QVERIFY(road_mix->property("trackknife-stored-playlist-tab").toBool());
    QCOMPARE(road_mix->property("trackknife-stored-playlist-name").toString(),
             QStringLiteral("Road mix"));
    QVERIFY(road_mix->findChild<QTableView*>(QStringLiteral("track-view-stored-playlist")) !=
            nullptr);
    QVERIFY(tabs->tabBar()->tabButton(tabs->indexOf(road_mix), QTabBar::RightSide) != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistLoaded", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Road mix"))));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), road_mix);

    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistLoaded", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Quiet mix"))));
    QCOMPARE(tabs->count(), 4);
    auto* quiet_mix = tabs->currentWidget();
    QVERIFY(quiet_mix != road_mix);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-playlist-load")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-playlist-clear")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-playlist-rename")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-playlist-delete")) != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("action-queue-priority")) != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistRenamed", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Quiet mix")),
                                      Q_ARG(QString, QStringLiteral("Silent mix"))));
    QCOMPARE(quiet_mix->property("trackknife-stored-playlist-name").toString(),
             QStringLiteral("Silent mix"));
    QCOMPARE(tabs->tabText(tabs->indexOf(quiet_mix)), QStringLiteral("Silent mix"));

    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistDeleted", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Road mix"))));
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->indexOf(road_mix), -1);
}

void MainWindowTest::browsesStoredPlaylistsWithMouseAndKeyboard() {
    MainWindow window(100);
    auto* tree = window.findChild<QTreeView*>(QStringLiteral("server-library-tree"));
    auto* tree_model = window.findChild<ServerLibraryTreeModel*>();
    auto* filter = window.findChild<QLineEdit*>(QStringLiteral("server-library-filter"));
    auto* playlists = window.findChild<QPushButton*>(QStringLiteral("server-library-playlists"));
    auto* configure = window.findChild<QPushButton*>(QStringLiteral("server-library-configure"));
    auto* library = window.findChild<QTableView*>(QStringLiteral("track-view-server-library"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* toast = window.findChild<QLabel*>(QStringLiteral("notification-toast"));
    QVERIFY(tree != nullptr);
    QVERIFY(tree_model != nullptr);
    QVERIFY(filter != nullptr);
    QVERIFY(playlists != nullptr);
    QVERIFY(configure != nullptr);
    QVERIFY(library != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(toast != nullptr);
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
    QVERIFY(proxy != nullptr);
    QCOMPARE(proxy->sourceModel(), tree_model);
    QCOMPARE(tree_model->definition(), defaultLibraryTreeDefinition());
    window.show();

    QSignalSpy root_request{tree_model, &ServerLibraryTreeModel::rootRequested};
    tree_model->reload();
    QCOMPARE(root_request.size(), 1);
    tree_model->acceptRoot(root_request.front().front().toULongLong(),
                           QStringLiteral("AlbumArtist"),
                           {QStringLiteral("Artist B"), QStringLiteral("Artist A")}, {});
    QCOMPARE(tree_model->rowCount(), 2);
    QCOMPARE(tree_model->index(0, 0).data().toString(), QStringLiteral("Artist A"));
    QVERIFY(!tree_model->index(0, 0).data(Qt::DecorationRole).value<QIcon>().isNull());
    QCOMPARE(tree_model->columnCount(), 1);
    QVERIFY(tree->itemDelegate() != nullptr);
    QVERIFY(filter->isVisible());
    filter->setText(QStringLiteral("Artist B"));
    QCOMPARE(proxy->rowCount(), 1);
    filter->clear();
    auto* center = window.findChild<QStackedWidget*>(QStringLiteral("center-stack"));
    QVERIFY(center != nullptr);
    auto* prior_center_page = center->currentWidget();
    tree->setCurrentIndex(proxy->index(0, 0));
    QCOMPARE(center->currentWidget(), prior_center_page);

    QVERIFY(QObject::disconnect(tree_model, &ServerLibraryTreeModel::branchRequested, controller,
                                &quick::MpdProbeController::loadServerLibraryBranch));
    QSignalSpy branch_request{tree_model, &ServerLibraryTreeModel::branchRequested};
    const auto artist_row = proxy->index(0, 0);
    tree->setEnabled(true);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(artist_row).center());
    QVERIFY(!tree->isExpanded(artist_row));
    QCOMPARE(branch_request.size(), 1);
    tree_model->acceptBranch(branch_request.front().front().toULongLong(),
                             {albumTrack(1U, "Album", "Track", "1")}, {});
    QTRY_VERIFY(tree->isExpanded(artist_row));
    QStyleOptionViewItem tree_option;
    tree_option.widget = tree;
    QCOMPARE(tree->itemDelegate()->sizeHint(tree_option, artist_row).height(), 46);
    const auto album_row = proxy->mapFromSource(tree_model->index(0, 0, tree_model->index(0, 0)));
    QCOMPARE(tree->itemDelegate()->sizeHint(tree_option, album_row).height(), 42);
    const auto track_row = proxy->mapFromSource(
        tree_model->index(0, 0, tree_model->index(0, 0, tree_model->index(0, 0))));
    QCOMPARE(tree->itemDelegate()->sizeHint(tree_option, track_row).height(), 34);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(artist_row).center());
    QVERIFY(!tree->isExpanded(artist_row));

    playlists->setEnabled(true);
    QTest::mouseClick(playlists, Qt::LeftButton);
    const QStringList names{QStringLiteral("Road mix"), QStringLiteral("Quiet mix")};
    QVERIFY(QMetaObject::invokeMethod(controller, "storedPlaylistListLoaded", Qt::DirectConnection,
                                      Q_ARG(QStringList, names)));
    QCOMPARE(library->model()->rowCount(), 2);
    QCOMPARE(library->model()->index(0, 0).data().toString(), QStringLiteral("Road mix"));

    library->setCurrentIndex(library->model()->index(0, 0));
    library->setFocus();
    QTest::keyClick(library, Qt::Key_Return);
    QTRY_VERIFY(toast->text().contains(QStringLiteral("cannot read stored playlists")));
}

void MainWindowTest::scratchTabsPersistLifecycle() {
    const auto database_path = settings_directory_.filePath(QStringLiteral("scratch-tabs.sqlite3"));
    const auto remove_database = [&database_path] {
        QFile::remove(database_path);
        QFile::remove(database_path + QStringLiteral("-wal"));
        QFile::remove(database_path + QStringLiteral("-shm"));
    };
    remove_database();
    qApp->setProperty("trackknife-state-database-path", database_path);

    {
        MainWindow window(0);
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
        auto* create = window.findChild<QAction*>(QStringLiteral("action-new-scratch"));
        auto* rename = window.findChild<QAction*>(QStringLiteral("action-rename-tab"));
        auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QVERIFY(create != nullptr);
        QVERIFY(rename != nullptr);
        QVERIFY(close != nullptr);
        QTRY_VERIFY(window.property("trackknife-persistence-ready").toBool());
        QCOMPARE(create->shortcut(), QKeySequence(QStringLiteral("Ctrl+T")));
        QCOMPARE(rename->shortcut(), QKeySequence(Qt::Key_F2));
        QCOMPARE(close->shortcut(), QKeySequence(QStringLiteral("Ctrl+W")));
        QCOMPARE(tabs->count(), 2);
        QVERIFY(tabs->widget(1)->property("trackknife-local-list-tab").toBool());

        create->trigger();
        QCOMPARE(tabs->count(), 3);
        QVERIFY(tabs->currentWidget()->property("trackknife-local-list-tab").toBool());
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Scratch 2"));
        tabs->tabBar()->moveTab(2, 1);
    }

    {
        MainWindow restored(0);
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
        auto* close = restored.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QTRY_VERIFY(restored.property("trackknife-persistence-ready").toBool());
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Scratch 2"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Scratch 1"));
        tabs->setCurrentIndex(1);
        QTRY_VERIFY(close->isEnabled());
        close->trigger();
        QCOMPARE(tabs->count(), 2);
    }

    {
        MainWindow restored(0);
        auto* tabs = restored.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
        QTRY_VERIFY(restored.property("trackknife-persistence-ready").toBool());
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Scratch 1"));
    }

    qApp->setProperty("trackknife-state-database-path", QVariant{});
    remove_database();
}

void MainWindowTest::editsAndDuplicatesPersistentWorkingLists() {
    const auto database_path =
        settings_directory_.filePath(QStringLiteral("working-list-items.sqlite3"));
    QFile::remove(database_path);
    const auto profile_id = core::StableId::random();
    const auto make_item = [&profile_id](std::string title) {
        return persistence::ListItem{
            .source = persistence::ListSource::mpd,
            .profile_id = profile_id,
            .source_reference = title + ".flac",
            .logical_reference = std::nullopt,
            .segment = std::nullopt,
            .source_selection = std::nullopt,
            .duration_ms = 60'000,
            .fields = {{.name = "Artist", .value = "Artist"},
                       {.name = "Title", .value = std::move(title)}},
        };
    };
    {
        auto repository = persistence::ListRepository::open(
            std::filesystem::path{QFile::encodeName(database_path).toStdString()});
        QVERIFY(repository);
        const std::array documents{persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = "Work",
            .pinned = false,
            .dirty = false,
            .items = {make_item("Duplicate"), make_item("Duplicate"), make_item("Last")},
        }};
        QVERIFY(repository->replace_all(documents));
    }
    qApp->setProperty("trackknife-state-database-path", database_path);

    {
        MainWindow window(0);
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
        auto* view = window.findChild<QTableView*>(QStringLiteral("track-view-library"));
        auto* move_down = window.findChild<QAction*>(QStringLiteral("action-move-down"));
        auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove"));
        auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
        auto* pin = window.findChild<QAction*>(QStringLiteral("action-pin-tab"));
        auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
        QVERIFY(tabs != nullptr);
        QVERIFY(view != nullptr);
        QVERIFY(move_down != nullptr);
        QVERIFY(remove != nullptr);
        QVERIFY(duplicate != nullptr);
        QVERIFY(pin != nullptr);
        QVERIFY(close != nullptr);
        QTRY_VERIFY(window.property("trackknife-persistence-ready").toBool());
        view = window.findChild<QTableView*>(QStringLiteral("track-view-library"));
        QVERIFY(view != nullptr);
        tabs->setCurrentWidget(view);
        view->selectionModel()->setCurrentIndex(view->model()->index(0, 0),
                                                QItemSelectionModel::ClearAndSelect |
                                                    QItemSelectionModel::Rows);
        move_down->trigger();
        QCOMPARE(view->model()->index(1, track_title_column).data().toString(),
                 QStringLiteral("Duplicate"));
        view->selectionModel()->setCurrentIndex(view->model()->index(2, 0),
                                                QItemSelectionModel::ClearAndSelect |
                                                    QItemSelectionModel::Rows);
        remove->trigger();
        QCOMPARE(view->model()->rowCount(), 2);
        duplicate->trigger();
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(qobject_cast<QTableView*>(tabs->currentWidget())->model()->rowCount(), 2);
        tabs->setCurrentWidget(view);
        pin->trigger();
        QVERIFY(view->property("trackknife-list-pinned").toBool());
        QVERIFY(!close->isEnabled());
        QVERIFY(window.close());
    }

    auto repository = persistence::ListRepository::open(
        std::filesystem::path{QFile::encodeName(database_path).toStdString()});
    QVERIFY(repository);
    const auto restored = repository->load_all();
    QVERIFY(restored);
    QCOMPARE(restored->size(), 2U);
    QVERIFY(restored->front().pinned);
    QCOMPARE(restored->front().items.size(), 2U);
    QCOMPARE(restored->back().items, restored->front().items);
    QVERIFY(restored->back().dirty);
}

void MainWindowTest::commandPaletteDiscoversAndPersistsShortcuts() {
    {
        MainWindow window(100);
        auto* open = window.findChild<QAction*>(QStringLiteral("action-command-palette"));
        QVERIFY(open != nullptr);
        QCOMPARE(open->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+P")));
        open->trigger();
        auto* palette = window.findChild<QDialog*>(QStringLiteral("command-palette"));
        auto* filter = palette != nullptr
                           ? palette->findChild<QLineEdit*>(QStringLiteral("command-filter"))
                           : nullptr;
        auto* results = palette != nullptr
                            ? palette->findChild<QListWidget*>(QStringLiteral("command-results"))
                            : nullptr;
        auto* shortcut =
            palette != nullptr
                ? palette->findChild<QKeySequenceEdit*>(QStringLiteral("command-shortcut"))
                : nullptr;
        auto* apply =
            palette != nullptr
                ? palette->findChild<QPushButton*>(QStringLiteral("command-apply-shortcut"))
                : nullptr;
        QVERIFY(palette != nullptr);
        QVERIFY(filter != nullptr);
        QVERIFY(results != nullptr);
        QVERIFY(shortcut != nullptr);
        QVERIFY(apply != nullptr);
        filter->setText(QStringLiteral("New scratch"));
        QCOMPARE(results->count(), 1);
        shortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+N")));
        apply->click();
        auto* scratch = window.findChild<QAction*>(QStringLiteral("action-new-scratch"));
        QVERIFY(scratch != nullptr);
        QCOMPARE(scratch->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+N")));
    }

    MainWindow restored(100);
    auto* scratch = restored.findChild<QAction*>(QStringLiteral("action-new-scratch"));
    QVERIFY(scratch != nullptr);
    QCOMPARE(scratch->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+N")));
}

void MainWindowTest::decodesArtworkOffTheUiPath() {
    MainWindow window(100);
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* cover = window.findChild<QLabel*>(QStringLiteral("now-playing-cover"));
    QVERIFY(controller != nullptr);
    QVERIFY(cover != nullptr);
    QImage source(8, 8, QImage::Format_RGB32);
    source.fill(Qt::red);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&buffer, "PNG"));
    QVERIFY(QMetaObject::invokeMethod(controller, "artworkLoaded", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("album/track.flac")),
                                      Q_ARG(QByteArray, encoded)));
    QTRY_VERIFY_WITH_TIMEOUT(cover->pixmap().toImage().pixelColor(cover->pixmap().width() / 2,
                                                                  cover->pixmap().height() / 2) ==
                                 QColor(Qt::red),
                             1'000);
}

void MainWindowTest::persistsTrackViewPresetsTransactionally() {
    const auto database_path =
        settings_directory_.filePath(QStringLiteral("track-view-presets.sqlite3"));
    QFile::remove(database_path);
    qApp->setProperty("trackknife-state-database-path", database_path);
    {
        MainWindow window(0);
        QTRY_VERIFY(window.property("trackknife-persistence-ready").toBool());
        auto* queue = window.findChild<QTableView*>(QStringLiteral("track-view-queue"));
        auto* scratch = window.findChild<QTableView*>(QStringLiteral("track-view-library"));
        QVERIFY(queue != nullptr);
        QVERIFY(scratch != nullptr);
        queue->setColumnWidth(track_length_column, 113);
        scratch->setColumnWidth(0, 247);
        QVERIFY(window.close());
    }
    MainWindow restored(0);
    QTRY_VERIFY(restored.property("trackknife-persistence-ready").toBool());
    auto* queue = restored.findChild<QTableView*>(QStringLiteral("track-view-queue"));
    auto* scratch = restored.findChild<QTableView*>(QStringLiteral("track-view-library"));
    QVERIFY(queue != nullptr);
    QVERIFY(scratch != nullptr);
    QCOMPARE(queue->columnWidth(track_length_column), 113);
    QCOMPARE(scratch->columnWidth(0), 247);
}

void MainWindowTest::usesGroupedCantataStyleQueue() {
    MainWindow window(100);
    auto* queue = window.findChild<QTableView*>(QStringLiteral("track-view-queue"));
    QVERIFY(queue != nullptr);
    QVERIFY(qobject_cast<QueueItemDelegate*>(queue->itemDelegate()) != nullptr);
    QVERIFY(!queue->showGrid());
    QVERIFY(queue->dragEnabled());
    QVERIFY(queue->acceptDrops());
    QVERIFY(queue->showDropIndicator());
    QCOMPARE(queue->dragDropMode(), QAbstractItemView::InternalMove);
    QVERIFY(!queue->dragDropOverwriteMode());
    QVERIFY(queue->model()->supportedDropActions().testFlag(Qt::MoveAction));
    QVERIFY(queue->horizontalHeader()->isHidden());
    QVERIFY(!queue->isColumnHidden(track_artwork_column));
    QVERIFY(!queue->isColumnHidden(track_artist_column));
    QVERIFY(queue->isColumnHidden(track_number_column));
    QVERIFY(!queue->isColumnHidden(track_title_column));
    QVERIFY(queue->isColumnHidden(track_album_column));
    QVERIFY(queue->isColumnHidden(track_date_column));
    QVERIFY(!queue->isColumnHidden(track_length_column));
    for (int visual = 0; visual < track_column_count; ++visual) {
        QCOMPARE(queue->horizontalHeader()->logicalIndex(visual), visual);
    }
    auto* list_actions = window.findChild<QToolButton*>(QStringLiteral("list-actions-button"));
    QVERIFY(list_actions != nullptr);
    QCOMPARE(list_actions->parentWidget(), window.statusBar());
    QVERIFY(window.findChild<QToolBar*>(QStringLiteral("toolbar-track-actions")) == nullptr);
}

void MainWindowTest::mapsDropPositionsToInsertionRows() {
    quick::MpdQueueModel model;
    model.replaceTracks({albumTrack(1U, "Album", "One", "1"), albumTrack(2U, "Album", "Two", "2"),
                         albumTrack(3U, "Album", "Three", "3")});

    QueueTableView view{nullptr};
    view.setModel(&model);
    view.setItemDelegate(new QueueItemDelegate(&view));
    view.setProperty("trackknife-hover-row", -1);
    view.verticalHeader()->hide();
    view.horizontalHeader()->hide();
    view.resize(400, 400);
    view.resizeRowsToContents();
    view.show();

    const auto row_center = [&view, &model](const int row) {
        return view.visualRect(model.index(row, track_title_column)).center();
    };
    // Above/on a row inserts before it; below it inserts after it.
    QCOMPARE(view.dropInsertionRow(row_center(1), QueueTableView::AboveItem), 1);
    QCOMPARE(view.dropInsertionRow(row_center(1), QueueTableView::OnItem), 1);
    QCOMPARE(view.dropInsertionRow(row_center(1), QueueTableView::BelowItem), 2);
    QCOMPARE(view.dropInsertionRow(row_center(2), QueueTableView::BelowItem), 3);
    // Empty space below the content appends regardless of the indicator.
    const QPoint below_content{10,
                               view.visualRect(model.index(2, track_title_column)).bottom() + 50};
    QCOMPARE(view.dropInsertionRow(below_content, QueueTableView::OnViewport), 3);
}

void MainWindowTest::rendersNonzeroQueuePriority() {
    quick::MpdQueueModel model;
    auto prioritized = albumTrack(1U, "Album", "One", "1");
    prioritized.priority = 192U;
    auto default_priority = albumTrack(2U, "Album", "Two", "2");
    default_priority.priority = 0U;
    model.replaceTracks({prioritized, default_priority, albumTrack(3U, "Album", "Three", "3")});

    QTableView view;
    view.setModel(&model);
    view.setProperty("trackknife-hover-row", -1);
    QueueItemDelegate delegate{&view};
    QCOMPARE(delegate.priorityLabel(model.index(0, track_title_column)), QStringLiteral("192"));
    QVERIFY(delegate.priorityLabel(model.index(1, track_title_column)).isEmpty());
    QVERIFY(delegate.priorityLabel(model.index(2, track_title_column)).isEmpty());

    const auto paint_title_cell = [&view, &delegate](const QModelIndex& index) {
        QImage canvas{400, 60, QImage::Format_ARGB32_Premultiplied};
        canvas.fill(Qt::white);
        QPainter painter{&canvas};
        QStyleOptionViewItem option;
        option.rect = QRect{0, 0, 400, 52};
        option.font = view.font();
        option.fontMetrics = QFontMetrics{option.font};
        option.palette = view.palette();
        option.widget = &view;
        delegate.paint(&painter, option, index);
        painter.end();
        return canvas;
    };
    const auto badge_band_has_paint = [](const QImage& canvas, const int top, const int bottom) {
        for (int y = top; y < bottom; ++y) {
            for (int x = 360; x < 398; ++x) {
                if (canvas.pixel(x, y) != qRgb(255, 255, 255)) {
                    return true;
                }
            }
        }
        return false;
    };
    // The prioritized row paints a badge at the right edge of its title cell's
    // track-row area (below the 30px album header it begins); rows without a
    // nonzero priority leave that band untouched.
    QVERIFY(badge_band_has_paint(paint_title_cell(model.index(0, track_title_column)), 31, 51));
    QVERIFY(!badge_band_has_paint(paint_title_cell(model.index(2, track_title_column)), 2, 51));
}

void MainWindowTest::albumHeaderSelectsAlbumTracks() {
    MainWindow window(100);
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* queue = window.findChild<QTableView*>(QStringLiteral("track-view-queue"));
    QVERIFY(controller != nullptr);
    QVERIFY(queue != nullptr);
    auto* model = qobject_cast<quick::MpdQueueModel*>(controller->queueModel());
    QVERIFY(model != nullptr);
    model->replaceTracks({albumTrack(1U, "First album", "One", "1"),
                          albumTrack(2U, "First album", "Two", "2"),
                          albumTrack(3U, "Second album", "Three", "1")});

    window.show();
    const auto first_album = model->index(0, 1);
    QTRY_VERIFY(queue->visualRect(first_album).height() > 30);
    const auto first_header = QPoint{queue->visualRect(first_album).center().x(),
                                     queue->visualRect(first_album).top() + 5};
    QTest::mouseClick(queue->viewport(), Qt::LeftButton, Qt::NoModifier, first_header);

    auto selected = queue->selectionModel()->selectedRows(0);
    QCOMPARE(selected.size(), 2);
    QCOMPARE(selected.at(0).row(), 0);
    QCOMPARE(selected.at(1).row(), 1);

    const auto second_album = model->index(2, 1);
    QCOMPARE(queue->visualRect(second_album).height(),
             queue->visualRect(model->index(1, 1)).height());
    const auto second_header = QPoint{queue->visualRect(second_album).center().x(),
                                      queue->visualRect(second_album).top() + 5};
    QTest::mouseClick(queue->viewport(), Qt::LeftButton, Qt::NoModifier, second_header);
    selected = queue->selectionModel()->selectedRows(0);
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected.at(0).row(), 2);
}

void MainWindowTest::usesCapabilityAwareContextMenus() {
    MainWindow window(100);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
    auto* queue = window.findChild<QTableView*>(QStringLiteral("track-view-queue"));
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* row_menu = window.findChild<QMenu*>(QStringLiteral("track-row-context-menu"));
    auto* tab_menu = window.findChild<QMenu*>(QStringLiteral("track-tab-context-menu"));
    auto* activate = window.findChild<QAction*>(QStringLiteral("action-activate-selection"));
    auto* append = window.findChild<QAction*>(QStringLiteral("action-add-to-queue"));
    auto* add_next = window.findChild<QAction*>(QStringLiteral("action-add-next"));
    auto* remove = window.findChild<QAction*>(QStringLiteral("action-remove"));
    auto* crop = window.findChild<QAction*>(QStringLiteral("action-crop-selection"));
    auto* priority = window.findChild<QAction*>(QStringLiteral("action-queue-priority"));
    auto* rename = window.findChild<QAction*>(QStringLiteral("action-rename-tab"));
    auto* pin = window.findChild<QAction*>(QStringLiteral("action-pin-tab"));
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("action-duplicate-tab"));
    auto* move_left = window.findChild<QAction*>(QStringLiteral("action-move-tab-left"));
    auto* move_right = window.findChild<QAction*>(QStringLiteral("action-move-tab-right"));
    auto* close = window.findChild<QAction*>(QStringLiteral("action-close-tab"));
    QVERIFY(tabs != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(controller != nullptr);
    QVERIFY(row_menu != nullptr);
    QVERIFY(tab_menu != nullptr);
    QVERIFY(activate != nullptr);
    QVERIFY(append != nullptr);
    QVERIFY(add_next != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(crop != nullptr);
    QVERIFY(priority != nullptr);
    QVERIFY(rename != nullptr);
    QVERIFY(pin != nullptr);
    QVERIFY(duplicate != nullptr);
    QVERIFY(move_left != nullptr);
    QVERIFY(move_right != nullptr);
    QVERIFY(close != nullptr);

    auto* queue_model = qobject_cast<quick::MpdQueueModel*>(controller->queueModel());
    QVERIFY(queue_model != nullptr);
    queue_model->replaceTracks(
        {albumTrack(1U, "Album", "One", "1"), albumTrack(2U, "Album", "Two", "2")});
    window.show();
    QTRY_VERIFY(queue->visualRect(queue_model->index(1, 1)).isValid());
    const auto queue_position = queue->visualRect(queue_model->index(1, 1)).center();
    QContextMenuEvent queue_context{QContextMenuEvent::Mouse, queue_position,
                                    queue->viewport()->mapToGlobal(queue_position)};
    QApplication::sendEvent(queue->viewport(), &queue_context);
    QTRY_VERIFY(row_menu->isVisible());
    QCOMPARE(queue->currentIndex().row(), 1);
    QCOMPARE(activate->text(), QStringLiteral("Play"));
    QVERIFY(row_menu->actions().contains(activate));
    QVERIFY(row_menu->actions().contains(add_next));
    QVERIFY(row_menu->actions().contains(append));
    QVERIFY(row_menu->actions().contains(remove));
    QVERIFY(row_menu->actions().contains(crop));
    QVERIFY(row_menu->actions().contains(priority));
    row_menu->hide();

    auto* center = window.findChild<QStackedWidget*>(QStringLiteral("center-stack"));
    auto* search_page = window.findChild<QWidget*>(QStringLiteral("live-search-surface"));
    auto* search_view = window.findChild<QTableView*>(QStringLiteral("track-view-live-search"));
    auto* search_model = window.findChild<quick::MpdSearchResultModel*>();
    QVERIFY(center != nullptr);
    QVERIFY(search_page != nullptr);
    QVERIFY(search_view != nullptr);
    QVERIFY(search_model != nullptr);
    search_model->replaceTracks({albumTrack(3U, "Search album", "Result", "1")});
    search_page->show();
    search_page->raise();
    const auto search_row = search_model->firstResultRow();
    QTRY_VERIFY(search_view->visualRect(search_model->index(search_row, 1)).isValid());
    const auto search_position =
        search_view->visualRect(search_model->index(search_row, 1)).center();
    QContextMenuEvent search_context{QContextMenuEvent::Mouse, search_position,
                                     search_view->viewport()->mapToGlobal(search_position)};
    QApplication::sendEvent(search_view->viewport(), &search_context);
    QTRY_VERIFY(row_menu->isVisible());
    QCOMPARE(activate->text(), QStringLiteral("Replace queue and play"));
    QVERIFY(row_menu->actions().contains(activate));
    QVERIFY(row_menu->actions().contains(add_next));
    QVERIFY(row_menu->actions().contains(append));
    QVERIFY(!row_menu->actions().contains(remove));
    QVERIFY(!row_menu->actions().contains(crop));
    row_menu->hide();

    const auto scratch_index = 1;
    const auto scratch_position = tabs->tabBar()->tabRect(scratch_index).center();
    QContextMenuEvent scratch_context{QContextMenuEvent::Mouse, scratch_position,
                                      tabs->tabBar()->mapToGlobal(scratch_position)};
    QApplication::sendEvent(tabs->tabBar(), &scratch_context);
    QTRY_VERIFY(tab_menu->isVisible());
    QCOMPARE(tabs->currentIndex(), scratch_index);
    QVERIFY(tab_menu->actions().contains(rename));
    QVERIFY(tab_menu->actions().contains(pin));
    QVERIFY(tab_menu->actions().contains(duplicate));
    QVERIFY(tab_menu->actions().contains(move_left));
    QVERIFY(tab_menu->actions().contains(move_right));
    QVERIFY(tab_menu->actions().contains(close));
    tab_menu->hide();

    auto* scratch = qobject_cast<QTableView*>(tabs->currentWidget());
    QVERIFY(scratch != nullptr);
    const auto scratch_row_position = scratch->visualRect(scratch->model()->index(0, 1)).center();
    QContextMenuEvent scratch_row_context{QContextMenuEvent::Mouse, scratch_row_position,
                                          scratch->viewport()->mapToGlobal(scratch_row_position)};
    QApplication::sendEvent(scratch->viewport(), &scratch_row_context);
    QTRY_VERIFY(row_menu->isVisible());
    QVERIFY(!activate->isEnabled());
    QVERIFY(!append->isEnabled());
    QVERIFY(!add_next->isEnabled());
    QVERIFY(row_menu->actions().contains(remove));
    QVERIFY(row_menu->actions().contains(crop));
    row_menu->hide();

    const auto queue_tab_position = tabs->tabBar()->tabRect(0).center();
    QContextMenuEvent queue_tab_context{QContextMenuEvent::Mouse, queue_tab_position,
                                        tabs->tabBar()->mapToGlobal(queue_tab_position)};
    QApplication::sendEvent(tabs->tabBar(), &queue_tab_context);
    QTRY_VERIFY(tab_menu->isVisible());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(tab_menu->actions().contains(move_right));
    QVERIFY(!tab_menu->actions().contains(close));
    tab_menu->hide();
}

void MainWindowTest::usesNativePlaybackMenus() {
    MainWindow window(100);
    auto* replay_gain = window.findChild<QToolButton*>(QStringLiteral("replaygain-button"));
    auto* outputs = window.findChild<QToolButton*>(QStringLiteral("output-button"));
    auto* repeat = window.findChild<QAction*>(QStringLiteral("repeat-button"));
    QVERIFY(replay_gain != nullptr);
    QVERIFY(outputs != nullptr);
    QVERIFY(repeat != nullptr);
    QVERIFY(replay_gain->menu() != nullptr);
    QVERIFY(outputs->menu() != nullptr);
    QCOMPARE(replay_gain->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(outputs->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(replay_gain->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QCOMPARE(outputs->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(!replay_gain->icon().isNull());
    QVERIFY(!outputs->icon().isNull());
    QVERIFY(outputs->text().isEmpty());
    QCOMPARE(outputs->parentWidget(), window.statusBar());
    QVERIFY(repeat->isCheckable());
}

void MainWindowTest::showsTransientFailuresAsToasts() {
    MainWindow window(100);
    auto* controller = window.findChild<quick::MpdProbeController*>();
    auto* toast = window.findChild<QLabel*>(QStringLiteral("notification-toast"));
    QVERIFY(controller != nullptr);
    QVERIFY(toast != nullptr);
    QVERIFY(!toast->isVisible());

    window.show();
    emit controller->notificationRequested(QStringLiteral("Test failure"));

    QTRY_VERIFY(toast->isVisible());
    QCOMPARE(toast->text(), QStringLiteral("Test failure"));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("connection-status")) == nullptr);
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::MainWindowTest)

#include "main_window_test.moc"
