// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include <QApplication>
#include <QFile>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("trackknife"));
    QApplication::setApplicationName(QStringLiteral("trackbench"));
    QApplication::setApplicationDisplayName(QStringLiteral("Trackbench"));

    trackknife::bench::BenchMainWindow window;
    window.show();

    std::vector<std::string> raw_paths;
    const auto arguments = QApplication::arguments();
    raw_paths.reserve(static_cast<std::size_t>(arguments.size()));
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto encoded = QFile::encodeName(arguments.at(index));
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (!raw_paths.empty()) {
        window.openLocalPaths(std::move(raw_paths));
    }

    return QApplication::exec();
}
