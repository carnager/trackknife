// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QDialog>

#include <vector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace trackknife::ui {

class MpdConnectionDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit MpdConnectionDialog(QWidget* parent = nullptr,
                                 std::vector<persistence::ConnectionProfile> profiles = {},
                                 const QString& active_profile_id = {});
    ~MpdConnectionDialog() override;

    MpdConnectionDialog(const MpdConnectionDialog&) = delete;
    MpdConnectionDialog& operator=(const MpdConnectionDialog&) = delete;

  signals:
    void connectionRequested(const QString& profile_id, const QString& profile_name,
                             const QString& host, int port, const QString& password,
                             const QString& music_root, bool auto_connect);

  private slots:
    void submit();

  private:
    QLineEdit* host_{nullptr};
    QSpinBox* port_{nullptr};
    QLineEdit* password_{nullptr};
    QLineEdit* music_root_{nullptr};
    QCheckBox* auto_connect_{nullptr};
    QComboBox* profile_{nullptr};
    std::vector<persistence::ConnectionProfile> profiles_;
};

} // namespace trackknife::ui
