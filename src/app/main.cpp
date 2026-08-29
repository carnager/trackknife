// SPDX-License-Identifier: GPL-3.0-only

#include "ui/main_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QTimer>

#include <array>
#include <cstdio>
#include <limits>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Trackknife"));
    QApplication::setApplicationName(QStringLiteral("Trackknife"));
    QApplication::setApplicationVersion(QStringLiteral("0.0.1-m2"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Trackknife MPD and Melody client"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption rows_option(
        {QStringLiteral("r"), QStringLiteral("rows")},
        QStringLiteral("Logical rows in the synthetic scratch-list performance model."),
        QStringLiteral("count"), QStringLiteral("0"));
    const QCommandLineOption smoke_option(QStringLiteral("smoke-test"),
                                          QStringLiteral("Exit after the UI event loop starts."));
    parser.addOption(rows_option);
    parser.addOption(smoke_option);
    parser.process(application);

    bool rows_ok = false;
    const auto logical_rows = parser.value(rows_option).toLongLong(&rows_ok);
    if (!rows_ok || logical_rows < 0 || logical_rows > std::numeric_limits<int>::max()) {
        parser.showHelp(2);
    }
    if (parser.isSet(smoke_option)) {
        QStandardPaths::setTestModeEnabled(true);
    }

    trackknife::ui::MainWindow window(logical_rows);
    window.show();
    if (parser.isSet(smoke_option)) {
        if (window.objectName() != QStringLiteral("trackknife-main-window")) {
            std::fputs("Widgets shell has the wrong root object\n", stderr);
            return 1;
        }
        constexpr std::array required_objects{
            "track-tabs",        "track-view-queue", "live-search-surface", "panel-library",
            "toolbar-transport", "toolbar-progress", "replaygain-popup",    "output-popup"};
        for (const auto* name : required_objects) {
            if (window.findChild<QObject*>(QString::fromLatin1(name)) == nullptr) {
                std::fprintf(stderr, "Widgets shell is missing required object: %s\n", name);
                return 1;
            }
        }
        QTimer::singleShot(250, &application, &QApplication::quit);
    }
    return QApplication::exec();
}
