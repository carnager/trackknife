// SPDX-License-Identifier: GPL-3.0-only

#include "ui/mpd_connection_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace trackknife::ui {
namespace {

[[nodiscard]] QString displayText(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

MpdConnectionDialog::MpdConnectionDialog(QWidget* parent,
                                         std::vector<persistence::ConnectionProfile> profiles,
                                         const QString& active_profile_id)
    : QDialog(parent), profiles_(std::move(profiles)) {
    setObjectName(QStringLiteral("mpd-connection-dialog"));
    setWindowTitle(QStringLiteral("Connect to MPD or Melody"));
    setModal(false);
    setMinimumWidth(480);

    profile_ = new QComboBox(this);
    profile_->setObjectName(QStringLiteral("mpd-profile"));
    profile_->setEditable(true);
    profile_->setInsertPolicy(QComboBox::NoInsert);
    for (const auto& saved : profiles_) {
        profile_->addItem(displayText(saved.name), QString::fromStdString(saved.id.to_string()));
    }
    if (profile_->count() == 0) {
        profile_->addItem(QStringLiteral("Default"));
    }
    auto* new_profile = new QPushButton(QStringLiteral("New…"), this);
    new_profile->setObjectName(QStringLiteral("mpd-new-profile"));
    new_profile->setToolTip(QStringLiteral("Create another saved connection profile"));
    auto* profile_row = new QWidget(this);
    auto* profile_layout = new QHBoxLayout(profile_row);
    profile_layout->setContentsMargins(0, 0, 0, 0);
    profile_layout->addWidget(profile_, 1);
    profile_layout->addWidget(new_profile);

    host_ = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    host_->setObjectName(QStringLiteral("mpd-host"));
    host_->setPlaceholderText(QStringLiteral("Host name, address, or Unix socket"));

    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("mpd-port"));
    port_->setRange(1, 65'535);
    port_->setValue(6600);

    password_ = new QLineEdit(this);
    password_->setObjectName(QStringLiteral("mpd-password"));
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(QStringLiteral("Optional; not stored"));

    music_root_ = new QLineEdit(this);
    music_root_->setObjectName(QStringLiteral("mpd-music-root"));
    music_root_->setPlaceholderText(QStringLiteral("Optional local copy of the server music root"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("mpd-music-root-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose local music root"), music_root_->text());
        if (!chosen.isEmpty()) {
            music_root_->setText(chosen);
        }
    });
    auto* root_row = new QWidget(this);
    auto* root_layout = new QHBoxLayout(root_row);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->addWidget(music_root_, 1);
    root_layout->addWidget(browse);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Profile"), profile_row);
    form->addRow(QStringLiteral("Host or socket"), host_);
    form->addRow(QStringLiteral("Port"), port_);
    form->addRow(QStringLiteral("Password"), password_);
    form->addRow(QStringLiteral("Local music root"), root_row);
    auto_connect_ = new QCheckBox(QStringLiteral("Connect this profile on startup"), this);
    auto_connect_->setChecked(true);
    form->addRow(QString{}, auto_connect_);

    const auto select_profile = [this](const int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= profiles_.size()) {
            return;
        }
        const auto& saved = profiles_[static_cast<std::size_t>(index)];
        host_->setText(displayText(saved.host));
        port_->setValue(static_cast<int>(saved.port));
        music_root_->setText(saved.local_music_root ? displayText(*saved.local_music_root)
                                                    : QString{});
        auto_connect_->setChecked(saved.auto_connect);
    };
    connect(profile_, &QComboBox::currentIndexChanged, this, select_profile);
    connect(new_profile, &QPushButton::clicked, this, [this] {
        profile_->setCurrentIndex(-1);
        profile_->setEditText(QStringLiteral("New profile"));
        host_->setText(QStringLiteral("127.0.0.1"));
        port_->setValue(6600);
        password_->clear();
        music_root_->clear();
        auto_connect_->setChecked(true);
        profile_->setFocus();
        if (profile_->lineEdit() != nullptr) {
            profile_->lineEdit()->selectAll();
        }
    });
    int active_index = profile_->findData(active_profile_id);
    if (active_index < 0) {
        active_index = 0;
    }
    profile_->setCurrentIndex(active_index);
    select_profile(active_index);

    auto* note = new QLabel(
        QStringLiteral("Enter connects. The password remains in memory for this session only."),
        this);
    note->setObjectName(QStringLiteral("mpd-connection-status"));
    note->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* connect_button =
        buttons->addButton(QStringLiteral("Connect"), QDialogButtonBox::AcceptRole);
    connect_button->setObjectName(QStringLiteral("mpd-connect"));
    connect_button->setDefault(true);
    connect(connect_button, &QPushButton::clicked, this, &MpdConnectionDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    host_->selectAll();
    host_->setFocus();
}

MpdConnectionDialog::~MpdConnectionDialog() = default;

void MpdConnectionDialog::submit() {
    const auto host = host_->text().trimmed();
    if (host.isEmpty()) {
        host_->setFocus();
        return;
    }

    auto profile_id = profile_->currentData().toString();
    if (profile_id.isEmpty()) {
        profile_id = QString::fromStdString(core::StableId::random().to_string());
    }
    auto profile_name = profile_->currentText().trimmed();
    if (profile_name.isEmpty()) {
        profile_name = host;
    }
    emit connectionRequested(profile_id, profile_name, host, port_->value(), password_->text(),
                             music_root_->text(), auto_connect_->isChecked());
    accept();
}

} // namespace trackknife::ui
