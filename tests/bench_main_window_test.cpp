// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_list_model.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace trackknife::bench {

namespace {

constexpr std::uint32_t wave_sample_rate = 44'100U;

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
        append_u16(bytes, static_cast<std::uint16_t>(
                              static_cast<std::int16_t>(((frame * 37U) % 20'001U) - 10'000)));
    }
    std::ofstream output{QFile::encodeName(path).toStdString(), std::ios::binary};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

class BenchMainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
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

    // Each two-second track advances exactly one row.
    {
        const QDeadlineTimer advance_deadline{5'000};
        int last_state = -1;
        int ticks = 0;
        while (!current(1) && !advance_deadline.hasExpired()) {
            const auto state = window.property("trackbench-player-state").toInt();
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
    QTRY_VERIFY_WITH_TIMEOUT(current(2), 5'000);

    // The end of the list stays ended: no wrap-around back to the first row.
    QTest::qWait(2'700);
    QVERIFY(current(2));
    QVERIFY(!current(0));
}

} // namespace trackknife::bench

QTEST_MAIN(trackknife::bench::BenchMainWindowTest)

#include "bench_main_window_test.moc"
