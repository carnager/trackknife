// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

namespace trackknife::bench {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    setObjectName(QStringLiteral("bench-settings-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(520, 0);

    const QSettings settings;
    auto* form = new QFormLayout(this);

    startup_ = new QComboBox(this);
    startup_->setObjectName(QStringLiteral("bench-settings-startup"));
    startup_->addItem(QStringLiteral("Local queue"), QStringLiteral("local"));
    startup_->addItem(QStringLiteral("MPD queue"), QStringLiteral("mpd"));
    const auto saved_context =
        settings.value(QLatin1String(startup_context_key), QStringLiteral("local")).toString();
    if (const auto position = startup_->findData(saved_context); position >= 0) {
        startup_->setCurrentIndex(position);
    }
    form->addRow(QStringLiteral("Start in:"), startup_);

    auto* root_row = new QHBoxLayout;
    music_root_ = new QLineEdit(this);
    music_root_->setObjectName(QStringLiteral("bench-settings-music-root"));
    music_root_->setText(settings.value(QLatin1String(music_root_key)).toString());
    music_root_->setPlaceholderText(QStringLiteral("MPD music folder, e.g. /mnt/nas/Music"));
    music_root_->setToolTip(
        QStringLiteral("The folder MPD serves its library from, as this machine sees it. "
                       "With it set, MPD selections can load as local files."));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("bench-settings-music-root-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the MPD music folder"), music_root_->text());
        if (!chosen.isEmpty()) {
            music_root_->setText(chosen);
        }
    });
    root_row->addWidget(music_root_, 1);
    root_row->addWidget(browse);
    form->addRow(QStringLiteral("MPD music folder:"), root_row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("bench-settings-buttons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

void SettingsDialog::save() {
    QSettings settings;
    settings.setValue(QLatin1String(startup_context_key), startup_->currentData().toString());
    settings.setValue(QLatin1String(music_root_key), music_root_->text().trimmed());
}

} // namespace trackknife::bench
