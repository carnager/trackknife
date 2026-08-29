// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_probe_controller.hpp"
#include "trackknife/qtmodels/paged_track_model.hpp"
#include "ui/main_window.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Summary {
    double p50_ms{0.0};
    double p95_ms{0.0};
    double worst_ms{0.0};
};

[[nodiscard]] Summary summarize(std::vector<double> samples) {
    std::ranges::sort(samples);
    const auto percentile = [&samples](const double fraction) {
        const auto position =
            static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1U));
        return samples.at(position);
    };
    return {.p50_ms = percentile(0.50), .p95_ms = percentile(0.95), .worst_ms = samples.back()};
}

template <typename Operation> [[nodiscard]] double measure(Operation&& operation) {
    QElapsedTimer timer;
    timer.start();
    operation();
    QApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents);
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

template <typename Operation, typename Predicate>
[[nodiscard]] double measureUntil(Operation&& operation, Predicate&& predicate,
                                  const int timeout_ms = 1'000) {
    QElapsedTimer timer;
    timer.start();
    operation();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

void report(const std::string_view metric, const double budget_ms, const Summary& summary) {
    std::cout << metric << ',' << budget_ms << ',' << summary.p50_ms << ',' << summary.p95_ms << ','
              << summary.worst_ms << ',' << (summary.p95_ms <= budget_ms ? "within" : "over")
              << '\n';
}

[[nodiscard]] qint64 residentMemoryKiB() {
    std::ifstream status{"/proc/self/status"};
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            qint64 value = -1;
            status >> value;
            return value;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return -1;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TrackknifeBenchmarks"));
    QApplication::setApplicationName(QStringLiteral("UiInteractionBenchmark"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings{}.clear();

    const bool quick = application.arguments().contains(QStringLiteral("--quick"));
    const auto sample_count = quick ? 12 : 240;
    trackknife::ui::MainWindow window(1'000'000);
    window.show();
    static_cast<void>(QTest::qWaitForWindowExposed(&window, 2'000));
    QTest::qWait(50);

    auto* view = window.findChild<QTableView*>(QStringLiteral("track-view-library"));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("track-tabs"));
    auto* library_dock = window.findChild<QDockWidget*>(QStringLiteral("panel-library"));
    auto* model = view == nullptr
                      ? nullptr
                      : qobject_cast<trackknife::qtmodels::PagedTrackModel*>(view->model());
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("global-search"));
    auto* center = window.findChild<QStackedWidget*>(QStringLiteral("center-stack"));
    auto* search_page = window.findChild<QWidget*>(QStringLiteral("live-search-surface"));
    auto* controller = window.findChild<trackknife::quick::MpdProbeController*>();
    auto* cover = window.findChild<QLabel*>(QStringLiteral("now-playing-cover"));
    if (view == nullptr || tabs == nullptr || library_dock == nullptr || model == nullptr) {
        std::cerr << "benchmark could not locate the instrumented workspace objects\n";
        return EXIT_FAILURE;
    }
    const auto initial_memory_kib = residentMemoryKiB();

    std::vector<double> scroll_samples;
    std::vector<double> selection_samples;
    std::vector<double> tab_samples;
    std::vector<double> dock_samples;
    std::vector<double> restore_samples;
    std::vector<double> page_load_samples;
    std::vector<double> search_ack_samples;
    std::vector<double> artwork_samples;
    scroll_samples.reserve(static_cast<std::size_t>(sample_count));
    selection_samples.reserve(static_cast<std::size_t>(sample_count));
    tab_samples.reserve(static_cast<std::size_t>(sample_count));
    dock_samples.reserve(static_cast<std::size_t>(sample_count));
    restore_samples.reserve(static_cast<std::size_t>(sample_count));
    search_ack_samples.reserve(static_cast<std::size_t>(sample_count));
    artwork_samples.reserve(static_cast<std::size_t>(sample_count));

    QImage artwork_source(32, 32, QImage::Format_RGB32);
    artwork_source.fill(Qt::red);
    QByteArray artwork_bytes;
    QBuffer artwork_buffer(&artwork_bytes);
    static_cast<void>(artwork_buffer.open(QIODevice::WriteOnly));
    static_cast<void>(artwork_source.save(&artwork_buffer, "PNG"));

    QObject::connect(model, &trackknife::qtmodels::PagedTrackModel::pageLoaded, &application,
                     [&page_load_samples](const qint64, const qint64, const qint64 microseconds) {
                         page_load_samples.push_back(static_cast<double>(microseconds) / 1'000.0);
                     });

    const auto initial_state = window.saveState(1);
    for (int sample = 0; sample < sample_count; ++sample) {
        const auto row = static_cast<int>((static_cast<qint64>(sample) * 104'729) % 1'000'000);
        const auto target = model->index(row, 0);

        scroll_samples.push_back(measure([&view, &target]() {
            view->scrollTo(target, QAbstractItemView::PositionAtCenter);
            view->viewport()->repaint();
        }));
        selection_samples.push_back(measure([&view, &target]() {
            view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
            view->viewport()->repaint();
        }));
        tab_samples.push_back(measure([&tabs]() {
            tabs->setCurrentIndex(1 - tabs->currentIndex());
            tabs->currentWidget()->repaint();
        }));
        dock_samples.push_back(measure([&window, library_dock, sample]() {
            const auto area = sample % 2 == 0 ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea;
            window.addDockWidget(area, library_dock);
            window.repaint();
        }));
        restore_samples.push_back(measure([&window, &initial_state]() {
            static_cast<void>(window.restoreState(initial_state, 1));
            window.repaint();
        }));
        if (search != nullptr && center != nullptr && search_page != nullptr) {
            search_ack_samples.push_back(measure([search, center, sample]() {
                search->setText(QStringLiteral("query-%1").arg(sample));
                static_cast<void>(QMetaObject::invokeMethod(
                    search, "textEdited", Qt::DirectConnection, Q_ARG(QString, search->text())));
                center->repaint();
            }));
            QTest::keyClick(search, Qt::Key_Escape);
        }
        if (controller != nullptr && cover != nullptr) {
            const auto uri = QStringLiteral("benchmark/%1.flac").arg(sample);
            artwork_samples.push_back(measureUntil(
                [controller, &artwork_bytes, &uri] {
                    static_cast<void>(QMetaObject::invokeMethod(
                        controller, "artworkLoaded", Qt::DirectConnection, Q_ARG(QString, uri),
                        Q_ARG(QByteArray, artwork_bytes)));
                },
                [cover] {
                    const auto image = cover->pixmap().toImage();
                    return !image.isNull() &&
                           image.pixelColor(image.width() / 2, image.height() / 2) ==
                               QColor(Qt::red);
                }));
        }

        if (sample % 8 == 0) {
            model->refreshPageContaining(row);
        }
    }

    QTest::qWait(100);
    const auto scroll = summarize(std::move(scroll_samples));
    const auto selection = summarize(std::move(selection_samples));
    const auto tab = summarize(std::move(tab_samples));
    const auto dock = summarize(std::move(dock_samples));
    const auto restore = summarize(std::move(restore_samples));

    std::cout << "platform," << QApplication::platformName().toStdString() << '\n';
    std::cout << "logical_rows," << model->logicalRowCount() << '\n';
    std::cout << "resident_pages," << model->residentPageCount() << ','
              << trackknife::qtmodels::PagedTrackModel::residentPageLimit() << '\n';
    const auto final_memory_kib = residentMemoryKiB();
    std::cout << "initial_memory_kib," << initial_memory_kib << '\n';
    std::cout << "final_memory_kib," << final_memory_kib << '\n';
    std::cout << "memory_growth_kib,"
              << (initial_memory_kib >= 0 && final_memory_kib >= 0
                      ? final_memory_kib - initial_memory_kib
                      : -1)
              << '\n';
    std::cout << "metric,budget_ms,p50_ms,p95_ms,worst_ms,result\n";
    report("scroll_and_paint", 16.7, scroll);
    report("selection_and_paint", 50.0, selection);
    report("tab_switch_and_paint", 50.0, tab);
    report("dock_move_and_paint", 50.0, dock);
    report("saved_layout_restore_and_paint", 75.0, restore);
    if (!search_ack_samples.empty()) {
        report("search_loading_acknowledgement", 50.0, summarize(std::move(search_ack_samples)));
    }
    if (!artwork_samples.empty()) {
        report("artwork_decode_and_present", 50.0, summarize(std::move(artwork_samples)));
    }
    if (!page_load_samples.empty()) {
        report("background_page_load", 50.0, summarize(std::move(page_load_samples)));
    }

    const bool bounded =
        model->residentPageCount() <= trackknife::qtmodels::PagedTrackModel::residentPageLimit() &&
        model->activeLoadCount() <= trackknife::qtmodels::PagedTrackModel::activeLoadLimit() &&
        model->pendingLoadCount() <= trackknife::qtmodels::PagedTrackModel::pendingLoadLimit();
    QSettings{}.clear();
    return bounded ? EXIT_SUCCESS : EXIT_FAILURE;
}
