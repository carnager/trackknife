// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <cstddef>
#include <string>
#include <vector>

namespace {

QtMessageHandler default_message_handler = nullptr;

// Qt's Wayland backend logs a mouse-grab complaint on every ordinary
// menu-bar interaction while a menu is open (upstream QTBUG-87303 family);
// the navigation itself works, so the known-noise line is dropped and
// everything else reaches the default handler untouched.
void filtered_message_handler(const QtMsgType type, const QMessageLogContext& context,
                              const QString& message) {
    if (message ==
        QLatin1String("This plugin supports grabbing the mouse only for popup windows")) {
        return;
    }
    if (default_message_handler != nullptr) {
        default_message_handler(type, context, message);
    }
}

} // namespace

// QA soak hook: TRACKBENCH_SOAK_LOG=<path> appends one line per minute —
// timestamp, resident memory, live QObject/widget counts, and event-loop
// lateness — so gradual degradation over long sessions becomes measurable
// instead of anecdotal.
namespace {
void startSoakLog(QObject* parent) {
    const auto path = qEnvironmentVariable("TRACKBENCH_SOAK_LOG");
    if (path.isEmpty()) {
        return;
    }
    auto* timer = new QTimer(parent);
    timer->setInterval(60'000);
    auto* lateness = new qint64{0};
    QObject::connect(timer, &QTimer::destroyed, parent, [lateness] { delete lateness; });
    auto* expected = new QElapsedTimer{};
    QObject::connect(timer, &QTimer::destroyed, parent, [expected] { delete expected; });
    expected->start();
    QObject::connect(timer, &QTimer::timeout, parent, [path, timer, lateness, expected] {
        // How late the timer fired is a direct sample of event-loop
        // congestion — the thing a user feels as sluggish tab switches.
        *lateness = expected->elapsed() - timer->interval();
        expected->restart();
        long long resident_pages = 0;
        if (QFile statm{QStringLiteral("/proc/self/statm")}; statm.open(QIODevice::ReadOnly)) {
            const auto fields = QString::fromLatin1(statm.readAll()).split(QLatin1Char(' '));
            if (fields.size() > 1) {
                resident_pages = fields[1].toLongLong();
            }
        }
        std::size_t object_count = 0;
        const auto widgets = QApplication::allWidgets();
        for (const auto* widget : widgets) {
            object_count += static_cast<std::size_t>(widget->children().size());
        }
        QFile log{path};
        if (log.open(QIODevice::Append | QIODevice::Text)) {
            log.write(QStringLiteral("%1 rss_kb=%2 widgets=%3 child_objects=%4 late_ms=%5\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                          .arg(resident_pages * 4)
                          .arg(widgets.size())
                          .arg(object_count)
                          .arg(*lateness)
                          .toUtf8());
        }
    });
    timer->start();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    default_message_handler = qInstallMessageHandler(filtered_message_handler);
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

    startSoakLog(&application);
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
