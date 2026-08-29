// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/local_folder_tree_model.hpp"

#include <QByteArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <string>

namespace trackknife::ui {

class LocalFolderTreeModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void lazilyEnumeratesRawPathsWithoutFollowingDirectorySymlinks();
};

void LocalFolderTreeModelTest::lazilyEnumeratesRawPathsWithoutFollowingDirectorySymlinks() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path{QFile::encodeName(temporary.path()).toStdString()};
    const auto child_directory = root / "album";
    std::filesystem::create_directory(child_directory);
    const std::string raw_filename{"track_\xFF.wav", 11U};
    const auto raw_file = root / std::filesystem::path{raw_filename};
    std::ofstream{raw_file}.put('\0');
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(child_directory, root / "album-link", symlink_error);
    QVERIFY(!symlink_error);

    LocalFolderTreeModel model;
    QSignalSpy errors(&model, &LocalFolderTreeModel::directoryError);
    model.addRoot(root.native());
    QCOMPARE(model.rowCount(), 1);
    const auto root_index = model.index(0, 0);
    QVERIFY(root_index.isValid());
    QVERIFY(model.isDirectory(root_index));
    QCOMPARE(model.rawPath(root_index), root.native());
    QVERIFY(model.canFetchMore(root_index));

    model.fetchMore(root_index);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(root_index), 2, 2'000);
    QCOMPARE(errors.count(), 0);
    QVERIFY(!model.canFetchMore(root_index));

    bool saw_directory = false;
    bool saw_raw_file = false;
    bool saw_symlink = false;
    for (int row = 0; row < model.rowCount(root_index); ++row) {
        const auto child = model.index(row, 0, root_index);
        const auto raw_path = model.rawPath(child);
        saw_directory = saw_directory || raw_path == child_directory.native();
        saw_raw_file = saw_raw_file || raw_path == raw_file.native();
        saw_symlink = saw_symlink || raw_path == (root / "album-link").native();
        if (raw_path == raw_file.native()) {
            QVERIFY(child.data(Qt::DisplayRole).toString().contains(QStringLiteral("\\xFF")));
            const QByteArray expected{raw_path.data(), static_cast<qsizetype>(raw_path.size())};
            QCOMPARE(child.data(Qt::UserRole).toByteArray(), expected);
        }
    }
    QVERIFY(saw_directory);
    QVERIFY(saw_raw_file);
    QVERIFY(!saw_symlink);
}

} // namespace trackknife::ui

QTEST_MAIN(trackknife::ui::LocalFolderTreeModelTest)

#include "local_folder_tree_model_test.moc"
