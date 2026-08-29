// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include <QApplication>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("trackknife"));
    QApplication::setApplicationName(QStringLiteral("trackbench"));
    QApplication::setApplicationDisplayName(QStringLiteral("Trackbench"));

    // QA hook: --screenshot <file.png> renders the workspace, grabs it once
    // background probing has had a moment, and exits. It switches to the
    // test-mode settings location so real user state stays untouched.
    QString screenshot_path;
    std::vector<std::string> raw_paths;
    const auto arguments = QApplication::arguments();
    raw_paths.reserve(static_cast<std::size_t>(arguments.size()));
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments.at(index) == QStringLiteral("--screenshot") && index + 1 < arguments.size()) {
            screenshot_path = arguments.at(++index);
            continue;
        }
        const auto encoded = QFile::encodeName(arguments.at(index));
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (!screenshot_path.isEmpty()) {
        QStandardPaths::setTestModeEnabled(true);
    }

    trackknife::bench::BenchMainWindow window;
    window.show();
    if (!raw_paths.empty()) {
        window.openLocalPaths(std::move(raw_paths));
    }
    if (!screenshot_path.isEmpty()) {
        QTimer::singleShot(3'000, &application, [&window, screenshot_path] {
            const auto image = window.grab().toImage();
            const auto saved = image.save(screenshot_path);
            QApplication::exit(saved ? 0 : 1);
        });
    }

    return QApplication::exec();
}
