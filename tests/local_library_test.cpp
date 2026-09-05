// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_library_panel.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/local_artwork.hpp"
#include "uicommon/local_files_mime_data.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QBuffer>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <sqlite3.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/tpropertymap.h>

#include <filesystem>
#include <fstream>

namespace trackknife::bench {
namespace {

std::string fixture(const std::filesystem::path& root, const std::string& name,
                    const std::string& title = "First song",
                    const std::string& album = "Test album",
                    const std::string& track_number = "3") {
    std::filesystem::create_directories(root);
    QFile encoded{QStringLiteral(TRACKKNIFE_AUDIO_FIXTURE_DIR "/tagged-tone-flac.b64")};
    if (!encoded.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto data = QByteArray::fromBase64(encoded.readAll());
    const auto path = (root / name).native();
    {
        std::ofstream output{std::filesystem::path{path}, std::ios::binary};
        output.write(data.data(), data.size());
    }
    TagLib::FLAC::File file{path.c_str()};
    auto properties = file.properties();
    properties.replace("TITLE", TagLib::String{title, TagLib::String::UTF8});
    properties.replace("ALBUM", TagLib::String{album, TagLib::String::UTF8});
    properties.replace("ARTIST", TagLib::String{"Björk", TagLib::String::UTF8});
    properties.replace("ALBUMARTIST", TagLib::String{"Björk", TagLib::String::UTF8});
    if (track_number.empty()) {
        properties.erase("TRACKNUMBER");
    } else {
        properties.replace("TRACKNUMBER", TagLib::String{track_number, TagLib::String::UTF8});
    }
    file.setProperties(properties);
    if (!file.save()) {
        return {};
    }
    return path;
}

persistence::LibraryQuery tracks(std::string text = {}) {
    persistence::LibraryQuery query;
    query.kind = persistence::LibraryEntryKind::track;
    query.text = std::move(text);
    return query;
}

QMenu* libraryMenu(LocalLibraryPanel* panel, const QModelIndex& index) {
    auto* tree = panel->findChild<QTreeView*>();
    tree->scrollTo(index);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualRect(index).center()));
    return panel->findChild<QMenu*>(QStringLiteral("local-library-context-menu"));
}

bool triggerLibraryAction(LocalLibraryPanel* panel, const QModelIndex& index, int action) {
    auto* menu = libraryMenu(panel, index);
    if (!menu) {
        return false;
    }
    auto* command =
        menu->findChild<QAction*>(QStringLiteral("action-local-library-%1").arg(action));
    if (!command || !command->isEnabled()) {
        menu->close();
        return false;
    }
    command->trigger();
    menu->close();
    return true;
}

bool dropFiles(QTableView* view, const QMimeData* mime, const QPoint& position) {
    QDragEnterEvent enter{position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view->viewport(), &enter);
    if (!enter.isAccepted()) {
        return false;
    }
    QDropEvent drop{QPointF{position}, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(view->viewport(), &drop);
    return drop.isAccepted();
}

} // namespace

class LocalLibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void rootsRetainOfflineMusicAndRawPaths();
    void incrementalScanSearchAndPaging();
    void albumIdentityKeepsEditionsSeparate();
    void metadataAndMovesFollowTheListTransaction();
    void migrationRoundTrip();
    void scansOnlyOnRefresh_data();
    void scansOnlyOnRefresh();
    void localViewBrowsesSearchesAndOpensFiles();
    void dragResolvesUnloadedPagesAndRawPaths();
    void trackNumbersAppearInTreeAndSearch();
    void albumCoversLoadAndRefresh();
};

void LocalLibraryTest::rootsRetainOfflineMusicAndRawPaths() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto path = fixture(root, "raw-\xff.flac");
    QVERIFY(!path.empty());
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library);
    QVERIFY(library->roots()->empty());
    QVERIFY(library->add_root(root.native()));
    QVERIFY(!library->add_root(root.native()));
    QVERIFY(!library->add_root(base.native()));
    QVERIFY(!library->add_root("relative"));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    QCOMPARE(progress.indexed.load(), 1U);
    QCOMPARE(library->paths(tracks())->front(), path);
    QCOMPARE(library->query(tracks())->entries.front().available, 1U);
    std::filesystem::rename(root, base / "unplugged");
    persistence::LibraryScanProgress offline;
    QVERIFY(library->scan({}, offline));
    QVERIFY(!library->roots()->front().available);
    QCOMPARE(library->query(tracks())->entries.size(), 1U);
    QCOMPARE(library->query(tracks())->entries.front().available, 0U);
    QVERIFY(library->paths(tracks())->empty());
    std::filesystem::rename(base / "unplugged", root);
    persistence::LibraryScanProgress online;
    QVERIFY(library->scan({}, online));
    QCOMPARE(online.indexed.load(), 0U);
    QCOMPARE(library->query(tracks())->entries.front().available, 1U);
    QVERIFY(library->remove_root(root.native()));
    QVERIFY(library->query(tracks())->entries.empty());
    QVERIFY(std::filesystem::exists(std::filesystem::path{path}));
    // Qt's temporary-directory cleanup cannot round-trip a non-UTF-8 name.
    QVERIFY(std::filesystem::remove(std::filesystem::path{path}));
}

void LocalLibraryTest::incrementalScanSearchAndPaging() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root, "01.flac", "Needle in a song");
    const auto second = fixture(root, "02.flac", "Other song");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress first_scan;
    QVERIFY(library->scan({}, first_scan));
    QCOMPARE(first_scan.indexed.load(), 2U);
    persistence::LibraryScanProgress repeat;
    QVERIFY(library->scan({}, repeat));
    QCOMPARE(repeat.indexed.load(), 0U);
    QCOMPARE(library->query(tracks("BJÖRK needle"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("needle absent"))->entries.empty());
    QVERIFY(library->query(tracks("%' OR 1=1 --"))->entries.empty());
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    albums.text = "needle";
    QVERIFY(library->query(albums)->entries.empty());
    albums.text = "test björk";
    QCOMPARE(library->query(albums)->entries.size(), 1U);
    QCOMPARE(library->query(albums)->entries.front().tracks, 2U);
    auto page = tracks();
    page.limit = 1;
    const auto one = library->query(page);
    QVERIFY(one && one->more);
    page.offset = 1;
    const auto two = library->query(page);
    QVERIFY(two && !two->more);
    QVERIFY(one->entries.front().key != two->entries.front().key);
    std::filesystem::remove(std::filesystem::path{second});
    core::CancellationSource cancel;
    cancel.request_cancellation();
    persistence::LibraryScanProgress stopped;
    QVERIFY(library->scan(cancel.token(), stopped)->cancelled);
    QCOMPARE(library->paths(tracks())->size(), 2U);
    persistence::LibraryScanProgress missing;
    QVERIFY(library->scan({}, missing));
    QCOMPARE(library->paths(tracks())->size(), 1U);
    QCOMPARE(library->query(tracks())->entries.size(), 2U);
    QCOMPARE(fixture(root, "01.flac", "Changed title"), first);
    persistence::LibraryScanProgress changed;
    QVERIFY(library->scan({}, changed));
    QCOMPARE(changed.indexed.load(), 1U);
    QCOMPARE(library->query(tracks("Changed"))->entries.size(), 1U);
    std::filesystem::create_symlink(std::filesystem::path{first}, root / "alias.flac");
    persistence::LibraryScanProgress symlink;
    QVERIFY(library->scan({}, symlink));
    QCOMPARE(library->paths(tracks())->size(), 1U);
}

void LocalLibraryTest::albumIdentityKeepsEditionsSeparate() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root / "edition-a", "01.flac");
    const auto second = fixture(root / "edition-b", "01.flac");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    QCOMPARE(library->query(albums)->entries.size(), 2U);
    for (const auto& path : {first, second}) {
        TagLib::FLAC::File file{path.c_str()};
        auto properties = file.properties();
        properties.replace("MUSICBRAINZ_ALBUMID",
                           TagLib::String{"9e181c7e-6df1-4cb0-82ac-77d2be9a3c70"});
        file.setProperties(properties);
        QVERIFY(file.save());
    }
    persistence::LibraryScanProgress refresh;
    QVERIFY(library->scan({}, refresh));
    const auto grouped = library->query(albums);
    QVERIFY(grouped);
    QCOMPARE(grouped->entries.size(), 1U);
    QCOMPARE(grouped->entries.front().tracks, 2U);
}

void LocalLibraryTest::metadataAndMovesFollowTheListTransaction() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root, "01.flac");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    auto repository = persistence::ListRepository::open(base / "state.sqlite");
    QVERIFY(repository);
    auto previous = core::observe_local_source_revision(source);
    QVERIFY(previous);
    persistence::ListDocument list{.id = core::StableId::random(),
                                   .kind = persistence::ListKind::scratch,
                                   .name = "Local Queue",
                                   .pinned = false,
                                   .dirty = false,
                                   .items = {}};
    persistence::ListItem item;
    item.source = persistence::ListSource::local;
    item.source_reference = source;
    item.source_revision = *previous;
    list.items.push_back(item);
    QVERIFY(repository->replace_all(std::vector{list}));
    QCOMPARE(fixture(root, "01.flac", "New title", "New album"), source);
    const auto read = metadata::read_local_metadata(source);
    QVERIFY(read);
    const persistence::LocalMetadataRefresh refresh{.operation_id = core::StableId::random(),
                                                    .source_reference = source,
                                                    .previous_revision = *previous,
                                                    .published_revision = read->source_revision,
                                                    .document = read->document};
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open((base / "state.sqlite").c_str(), &db), SQLITE_OK);
    QCOMPARE(
        sqlite3_exec(db,
                     "CREATE TRIGGER reject_library_refresh BEFORE UPDATE ON local_library_tracks "
                     "BEGIN SELECT RAISE(ABORT,'injected index failure'); END",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    QVERIFY(!repository->refresh_local_metadata(refresh));
    QCOMPARE(repository->load_all()->front().items.front().source_revision,
             std::optional{*previous});
    QCOMPARE(library->query(tracks("First song"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("New title"))->entries.empty());
    QCOMPARE(sqlite3_exec(db, "DROP TRIGGER reject_library_refresh", nullptr, nullptr, nullptr),
             SQLITE_OK);
    sqlite3_close(db);
    QVERIFY(repository->refresh_local_metadata(refresh));
    QCOMPARE(library->query(tracks("New title"))->entries.size(), 1U);
    QVERIFY(library->query(tracks("First song"))->entries.empty());
    QVERIFY(repository->refresh_local_metadata(refresh)->already_applied);
    const auto target = (root / "renamed.flac").native();
    std::filesystem::rename(std::filesystem::path{source}, std::filesystem::path{target});
    persistence::LocalSourceRelocation relocation{.operation_id = core::StableId::random(),
                                                  .source_reference = source,
                                                  .target_reference = target,
                                                  .previous_revision = read->source_revision,
                                                  .published_revision =
                                                      *core::observe_local_source_revision(target),
                                                  .published_document = std::nullopt};
    QVERIFY(repository->relocate_local_source(relocation));
    QCOMPARE(library->paths(tracks())->front(), target);
    QCOMPARE(repository->load_all()->front().items.front().source_reference, target);
    QVERIFY(repository->relocate_local_source(relocation)->already_applied);
    const auto outside = (base / "outside.flac").native();
    std::filesystem::rename(std::filesystem::path{target}, std::filesystem::path{outside});
    relocation.operation_id = core::StableId::random();
    relocation.source_reference = target;
    relocation.target_reference = outside;
    QVERIFY(repository->relocate_local_source(relocation));
    QVERIFY(library->query(tracks())->entries.empty());
    QCOMPARE(repository->load_all()->front().items.front().source_reference, outside);
}

void LocalLibraryTest::migrationRoundTrip() {
    QTemporaryDir temporary;
    const auto database = (std::filesystem::path{temporary.path().toStdString()} / "state.sqlite");
    {
        auto repository = persistence::ListRepository::open(database);
        QVERIFY(repository);
        QCOMPARE(*repository->schema_version(), 28U);
    }
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    for (const auto* direction : {"down", "up"}) {
        QFile migration{QStringLiteral(TRACKKNIFE_MIGRATION_DIR "/0028_local_library.%1.sql")
                            .arg(QString::fromLatin1(direction))};
        QVERIFY(migration.open(QIODevice::ReadOnly));
        QCOMPARE(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_exec(db, migration.readAll().constData(), nullptr, nullptr, nullptr),
                 SQLITE_OK);
        QCOMPARE(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    sqlite3_close(db);
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library);
    QVERIFY(library->roots()->empty());
    QVERIFY(library->query(tracks())->entries.empty());
}

void LocalLibraryTest::scansOnlyOnRefresh_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("completed") << false;
    QTest::newRow("cancelled-with-pending-changes") << true;
}

void LocalLibraryTest::scansOnlyOnRefresh() {
    QFETCH(bool, cancel);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    QVERIFY(!fixture(root, "01.flac").empty());
    const auto database = base / "state.sqlite";
    auto library = persistence::LocalLibrary::open(database);
    QVERIFY(library && library->add_root(root.native()));

    LocalLibraryPanel panel{database};
    auto* button = panel.findChild<QToolButton*>(QStringLiteral("local-library-scan"));
    QVERIFY(button);
    QVERIFY(!panel.property("scanning").toBool());
    // Accelerate any timers so an accidental periodic scanner is exercised.
    for (auto* timer : panel.findChildren<QTimer*>()) {
        timer->setInterval(10);
    }
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(!panel.property("scanning").toBool());
    QVERIFY(library->paths(tracks())->empty());

    // Hold a manual scan at the database boundary to exercise cancellation
    // and view refreshes arriving during a slow scan.
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(database.c_str(), &db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> lock{db, sqlite3_close};
    QCOMPARE(sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    button->click();
    QVERIFY(panel.property("scanning").toBool());
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(panel.property("scanning").toBool());
    if (cancel) {
        panel.refreshLibrary();
        button->click();
    }
    QCOMPARE(sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    QTRY_VERIFY(!panel.property("scanning").toBool());
    if (cancel) {
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("local-library-status"))
                    ->text()
                    .contains(QStringLiteral("Scan stopped")));
    } else {
        QCOMPARE(library->paths(tracks())->size(), 1U);
    }

    const auto indexed = library->paths(tracks())->size();
    QVERIFY(!fixture(root, "02.flac").empty());
    panel.refreshLibrary();
    QTest::qWait(100);
    QVERIFY(!panel.property("scanning").toBool());
    QCOMPARE(library->paths(tracks())->size(), indexed);

    button->click();
    QVERIFY(panel.property("scanning").toBool());
    QTRY_VERIFY(!panel.property("scanning").toBool());
    QCOMPARE(library->paths(tracks())->size(), 2U);
    panel.stop();
}

void LocalLibraryTest::localViewBrowsesSearchesAndOpensFiles() {
    QTemporaryDir temporary;
    const auto old_data = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName(QStringLiteral("TrackknifeLibraryTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LocalLibrary"));
    QSettings{}.clear();
    const auto root = std::filesystem::path{temporary.path().toStdString()} / "music";
    const auto path = fixture(root, "01.flac");
    {
        BenchMainWindow window;
        window.show();
        QTRY_VERIFY(window.findChild<LocalLibraryPanel*>() != nullptr);
        auto* panel = window.findChild<LocalLibraryPanel*>();
        auto* selector =
            window.findChild<QComboBox*>(QStringLiteral("bench-local-source-selector"));
        auto* tree = panel->findChild<QTreeView*>();
        auto* search = panel->findChild<QLineEdit*>();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
        auto* sources = window.findChild<QStackedWidget*>(QStringLiteral("bench-source-stack"));
        QVERIFY(selector && tree && search && tabs && sources);
        QCOMPARE(selector->currentIndex(), 0);
        selector->setCurrentIndex(1);
        QCOMPARE(sources->currentWidget(), panel);
        panel->addRoot(root.native());
        QTRY_COMPARE(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))->text(),
                     QStringLiteral("Folder added. Press Refresh to scan for music."));
        QVERIFY(!panel->property("scanning").toBool());
        panel->findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
        QTRY_VERIFY(tree->model()->index(0, 0).data().toString().contains(QStringLiteral("Björk")));
        const auto artist = tree->model()->index(0, 0);
        tree->expand(artist);
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("Test album")));
        search->setText(QStringLiteral("Test album"));
        QCOMPARE(panel->findChild<QLabel*>(QStringLiteral("local-library-status"))->text(),
                 QStringLiteral("Searching…"));
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("Test album")));
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(1, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("First song")));
        const auto album = tree->model()->index(0, 0, tree->model()->index(0, 0));
        tree->setCurrentIndex(album);
        QVERIFY(triggerLibraryAction(panel, album, 0));
        QTRY_VERIFY(qobject_cast<QTableView*>(tabs->currentWidget()) != nullptr &&
                    qobject_cast<QTableView*>(tabs->currentWidget())->model()->rowCount() == 1);
        auto* local = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(local);
        QCOMPARE(local->rows().front().raw_path, path);
        auto* local_view = qobject_cast<QTableView*>(tabs->currentWidget());
        auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
        QVERIFY(local_view && mpd);
        const auto second = fixture(root, "02.flac", "Second song");
        panel->findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
        QTRY_VERIFY(tree->model()
                        ->index(0, 0, tree->model()->index(0, 0))
                        .data()
                        .toString()
                        .contains(QStringLiteral("(2)")));
        const auto album_index = [&] {
            return tree->model()->index(0, 0, tree->model()->index(0, 0));
        };
        // An album drag captures the selection before a search reset and resolves
        // asynchronously into the exact target tab and insertion row.
        std::unique_ptr<QMimeData> mime{tree->model()->mimeData({album_index()})};
        QVERIFY(dynamic_cast<ui::LocalFilesMimeData*>(mime.get()));
        const auto first_rect = local_view->visualRect(local->index(0, 0));
        QVERIFY(dropFiles(local_view, mime.get(), first_rect.topLeft() + QPoint{5, 2}));
        search->setText(QStringLiteral("no match"));
        tabs->setCurrentWidget(mpd);
        QTRY_COMPARE(local->rowCount(), 3);
        QCOMPARE(local->rows()[0].raw_path, path);
        QCOMPARE(local->rows()[1].raw_path, second);
        QCOMPARE(local->rows()[2].raw_path, path);
        QCOMPARE(tabs->currentWidget(), mpd);
        QVERIFY(!dropFiles(mpd, mime.get(), QPoint{20, 20}));
        QCOMPARE(mpd->model()->rowCount(), 0);
        tabs->setCurrentWidget(local_view);
        search->setText(QStringLiteral("Test album"));
        QTRY_VERIFY(album_index().data().toString().contains(QStringLiteral("(2)")));
        // Append an overlapping album + track selection once, preserving the
        // multi-selection when opening the menu on an already selected entry.
        QTRY_COMPARE(tree->model()->rowCount(tree->model()->index(1, 0)), 2);
        tree->selectionModel()->select(album_index(), QItemSelectionModel::ClearAndSelect |
                                                          QItemSelectionModel::Rows);
        const auto first_track = tree->model()->index(0, 0, tree->model()->index(1, 0));
        tree->selectionModel()->select(first_track,
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(triggerLibraryAction(panel, first_track, 0));
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 2);
        QTRY_COMPARE(local->rowCount(), 5);
        QCOMPARE(local->rows()[3].raw_path, path);
        QCOMPARE(local->rows()[4].raw_path, second);
        tree->setCurrentIndex(album_index());
        local_view->setCurrentIndex(local->index(0, 0));
        QVERIFY(triggerLibraryAction(panel, album_index(), 1));
        QTRY_COMPARE(local->rowCount(), 7);
        QCOMPARE(local->rows()[1].raw_path, path);
        QCOMPARE(local->rows()[2].raw_path, second);
        QVERIFY(triggerLibraryAction(panel, album_index(), 2));
        QTRY_COMPARE(local->rowCount(), 2);
        QCOMPARE(local->rows()[0].raw_path, path);
        QCOMPARE(local->rows()[1].raw_path, second);
        const auto original_tabs = tabs->count();
        QVERIFY(triggerLibraryAction(panel, album_index(), 3));
        QTRY_COMPARE(tabs->count(), original_tabs + 1);
        auto* new_view = qobject_cast<QTableView*>(tabs->currentWidget());
        QVERIFY(new_view && new_view != local_view);
        QTRY_COMPARE(new_view->model()->rowCount(), 2);
        // Closing a captured destination must never redirect its pending drop.
        QVERIFY(dropFiles(new_view, mime.get(), QPoint{20, 20}));
        const auto closed_index = tabs->currentIndex();
        QTimer::singleShot(0, [] {
            if (auto* confirmation =
                    qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                confirmation->done(QMessageBox::Yes);
            }
        });
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Qt::DirectConnection,
                                          Q_ARG(int, closed_index)));
        tabs->setCurrentWidget(local_view);
        QTest::qWait(300);
        QCOMPARE(local->rowCount(), 2);
        QVERIFY(!panel->property("scanning").toBool());
        if (const auto screenshot = qgetenv("TRACKKNIFE_LIBRARY_SCREENSHOT");
            !screenshot.isEmpty()) {
            tree->setFocus();
            tree->setCurrentIndex(album_index());
            QVERIFY(window.grab().save(QString::fromUtf8(screenshot)));
        }
        tabs->setCurrentWidget(mpd);
        QVERIFY(!panel->isVisible());
        QCOMPARE(window.property("trackknife-active-authority").toString(), QStringLiteral("mpd"));
        QVERIFY(sources->currentWidget() != panel);
    }
    qputenv("XDG_DATA_HOME", old_data);
}

void LocalLibraryTest::dragResolvesUnloadedPagesAndRawPaths() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto raw = fixture(root, "raw-\xff.flac");
    QVERIFY(!raw.empty());
    for (int index = 0; index < 204; ++index) {
        std::filesystem::copy_file(std::filesystem::path{raw},
                                   root / (std::to_string(index) + ".flac"));
    }
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    LocalLibraryPanel panel{base / "state.sqlite"};
    panel.resize(420, 400);
    panel.show();
    auto* tree = panel.findChild<QTreeView*>();
    QTRY_COMPARE(tree->model()->rowCount(), 1);
    const auto artist = tree->model()->index(0, 0);
    QVERIFY(!tree->isExpanded(artist));
    std::unique_ptr<QMimeData> mime{tree->model()->mimeData({artist})};
    const auto* files = dynamic_cast<ui::LocalFilesMimeData*>(mime.get());
    QVERIFY(files);
    std::vector<std::string> paths;
    files->resolve([&](std::vector<std::string> resolved) { paths = std::move(resolved); });
    QTRY_COMPARE(paths.size(), 205U);
    QVERIFY(std::ranges::find(paths, raw) != paths.end());
    QVERIFY(!tree->isExpanded(artist));
    QVERIFY(!panel.property("scanning").toBool());
    // Shared MPD-style branch interaction and inline actions work with local
    // presentation data, without enabling actions on placeholder rows.
    tree->setCurrentIndex(artist);
    QTest::keyClick(tree, Qt::Key_Return);
    QTRY_VERIFY(tree->isExpanded(artist));
    QTRY_VERIFY(tree->model()
                    ->index(0, 0, artist)
                    .data()
                    .toString()
                    .contains(QStringLiteral("Test album")));
    const auto album = tree->model()->index(0, 0, artist);
    std::vector<persistence::LibraryEntry> selected;
    connect(&panel, &LocalLibraryPanel::actionRequested, &panel,
            [&](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
                QCOMPARE(action, LocalLibraryAction::append);
                selected = std::move(entries);
            });
    const auto action_rect = ui::ServerLibraryTreeView::actionRect(tree->visualRect(album), 0);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, action_rect.center());
    QCOMPARE(selected.size(), 1U);
    QCOMPARE(selected.front().kind, persistence::LibraryEntryKind::album);
    QCOMPARE(selected.front().tracks, 205U);
    tree->selectionModel()->select(artist, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selected.clear();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, action_rect.center());
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 2);
    QCOMPARE(selected.size(), 1U);
    QCOMPARE(selected.front().kind, persistence::LibraryEntryKind::artist);
    std::filesystem::rename(root, base / "offline");
    persistence::LibraryScanProgress offline;
    QVERIFY(library->scan({}, offline));
    panel.refreshLibrary();
    QTRY_VERIFY(
        tree->model()->index(0, 0).data().toString().contains(QStringLiteral("unavailable")));
    auto* menu = libraryMenu(&panel, tree->model()->index(0, 0));
    QVERIFY(menu);
    for (int action = 0; action < 4; ++action) {
        auto* command =
            menu->findChild<QAction*>(QStringLiteral("action-local-library-%1").arg(action));
        QVERIFY(command && !command->isEnabled());
    }
    menu->close();
    std::filesystem::rename(base / "offline", root);
    std::filesystem::remove(std::filesystem::path{raw});
}

void LocalLibraryTest::trackNumbersAppearInTreeAndSearch() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto first = fixture(root, "first.flac", "First song", "Test album", "3/12");
    const auto last = fixture(root, "last.flac", "Last song", "Test album", "12");
    const auto unknown = fixture(root, "unknown.flac", "Unnumbered", "Test album", "");
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    auto query = tracks();
    query.raw_path = first;
    auto page = library->query(query);
    QVERIFY(page && page->entries.size() == 1U);
    QCOMPARE(page->entries.front().track_number, 3);
    QCOMPARE(page->entries.front().label, std::string{"03. First song"});
    query.raw_path = last;
    QCOMPARE(library->query(query)->entries.front().label, std::string{"12. Last song"});
    query.raw_path = unknown;
    QCOMPARE(library->query(query)->entries.front().label, std::string{"Unnumbered"});
    QCOMPARE(library->query(tracks("First"))->entries.front().label,
             std::string{"Björk — 03. First song"});
    QCOMPARE(library->query(tracks("Unnumbered"))->entries.front().label,
             std::string{"Björk — Unnumbered"});
}

void LocalLibraryTest::albumCoversLoadAndRefresh() {
    QTemporaryDir temporary;
    const std::filesystem::path base{temporary.path().toStdString()};
    const auto root = base / "music";
    const auto source = fixture(root, "raw-\xff.flac");
    QVERIFY(!source.empty());
    QImage cover{512, 256, QImage::Format_RGB32};
    cover.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer{&bytes};
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(cover.save(&buffer, "PNG"));
    {
        TagLib::FLAC::File file{source.c_str()};
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setWidth(cover.width());
        picture->setHeight(cover.height());
        picture->setColorDepth(24);
        picture->setData(
            TagLib::ByteVector{bytes.constData(), static_cast<unsigned int>(bytes.size())});
        file.addPicture(picture);
        QVERIFY(file.save());
    }
    const auto folder_cover = QString::fromStdString((root / "cover.png").native());
    cover.fill(Qt::red);
    QVERIFY(cover.save(folder_cover));
    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    QVERIFY(library && library->add_root(root.native()));
    persistence::LibraryScanProgress progress;
    QVERIFY(library->scan({}, progress));
    persistence::LibraryQuery albums;
    albums.kind = persistence::LibraryEntryKind::album;
    const auto album_key = library->query(albums)->entries.front().key;
    const auto representative = library->artwork_source(album_key);
    QVERIFY(representative && representative->has_value());
    QCOMPARE(**representative, source);
    LocalLibraryPanel panel{base / "state.sqlite"};
    panel.resize(420, 400);
    auto* tree = panel.findChild<QTreeView*>();
    auto* search = panel.findChild<QLineEdit*>();
    search->setText(QStringLiteral("Test album"));
    panel.show();
    const auto album = [&] { return tree->model()->index(0, 0, tree->model()->index(0, 0)); };
    const auto image = [&] {
        return album().data(Qt::DecorationRole).value<QIcon>().pixmap(128, 128).toImage();
    };
    const auto color = [&] {
        const auto loaded = image();
        return loaded.isNull() ? QColor{}
                               : loaded.pixelColor(loaded.width() / 2, loaded.height() / 2);
    };
    QTRY_COMPARE(color(), QColor{Qt::blue});
    QCOMPARE(image().size(), QSize(128, 64));
    QVERIFY(!panel.property("scanning").toBool());
    {
        TagLib::FLAC::File file{source.c_str()};
        file.removePictures();
        QVERIFY(file.save());
    }
    // Operation notifications invalidate thumbnails without a library scan.
    panel.refreshLibrary();
    QTRY_COMPARE(color(), QColor{Qt::red});
    QVERIFY(!panel.property("scanning").toBool());
    cover.fill(Qt::green);
    QVERIFY(cover.save(folder_cover));
    QCOMPARE(color(), QColor{Qt::red});
    panel.findChild<QToolButton*>(QStringLiteral("local-library-scan"))->click();
    QTRY_COMPARE(color(), QColor{Qt::green});
    QTRY_VERIFY(!panel.property("scanning").toBool());
    panel.hide();
    cover.fill(Qt::yellow);
    QVERIFY(cover.save(folder_cover));
    panel.refreshLibrary();
    QTest::qWait(300);
    QVERIFY(color() != QColor{Qt::yellow});
    panel.show();
    QTRY_COMPARE(color(), QColor{Qt::yellow});
    core::CancellationSource cancelled;
    cancelled.request_cancellation();
    QVERIFY(ui::loadLocalArtwork(source, cancelled.token()).isNull());
    // Corrupt and oversized fallback images leave the placeholder intact.
    {
        std::ofstream invalid{root / "cover.png", std::ios::binary | std::ios::trunc};
        invalid << "not an image";
    }
    QVERIFY(ui::loadLocalArtwork(source).isNull());
    std::filesystem::resize_file(root / "cover.png", 17U * 1024U * 1024U);
    QVERIFY(ui::loadLocalArtwork(source).isNull());
    panel.stop();
    QVERIFY(std::filesystem::remove(std::filesystem::path{source}));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LocalLibraryTest)
#include "local_library_test.moc"
