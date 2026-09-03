// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;

namespace trackknife::bench {

// Application settings (ADR-0112): which queue context activates on start,
// and the MPD music folder that lets MPD-mode selections resolve to local
// files. Values persist through QSettings on accept.
class SettingsDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // QSettings keys shared with the consumers.
    static constexpr auto startup_context_key = "startup/context";
    static constexpr auto music_root_key = "mpd/music-root";

  private:
    void save();

    QComboBox* startup_{nullptr};
    QLineEdit* music_root_{nullptr};
};

} // namespace trackknife::bench
