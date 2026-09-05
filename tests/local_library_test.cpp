// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_library_panel.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <sqlite3.h>
#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <filesystem>
#include <fstream>

namespace trackknife::bench {
namespace {

std::string fixture(const std::filesystem::path& root, const std::string& name,
                    const std::string& title = "First song",
                    const std::string& album = "Test album") {
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

} // namespace

class LocalLibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void rootsRetainOfflineMusicAndRawPaths();
    void incrementalScanSearchAndPaging();
    void albumIdentityKeepsEditionsSeparate();
    void metadataAndMovesFollowTheListTransaction();
    void migrationRoundTrip();
    void localViewBrowsesSearchesAndOpensFiles();
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
        QTest::keyClick(tree, Qt::Key_Return);
        QTRY_VERIFY(qobject_cast<QTableView*>(tabs->currentWidget()) != nullptr &&
                    qobject_cast<QTableView*>(tabs->currentWidget())->model()->rowCount() == 1);
        auto* local = qobject_cast<LocalListModel*>(
            qobject_cast<QTableView*>(tabs->currentWidget())->model());
        QVERIFY(local);
        QCOMPARE(local->rows().front().raw_path, path);
        if (const auto screenshot = qgetenv("TRACKKNIFE_LIBRARY_SCREENSHOT");
            !screenshot.isEmpty()) {
            QVERIFY(window.grab().save(QString::fromUtf8(screenshot)));
        }
        auto* mpd = window.findChild<QTableView*>(QStringLiteral("bench-mpd-queue"));
        QVERIFY(mpd);
        tabs->setCurrentWidget(mpd);
        QVERIFY(!panel->isVisible());
        QCOMPARE(window.property("trackknife-active-authority").toString(), QStringLiteral("mpd"));
        QVERIFY(sources->currentWidget() != panel);
    }
    qputenv("XDG_DATA_HOME", old_data);
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::LocalLibraryTest)
#include "local_library_test.moc"
