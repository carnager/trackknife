// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QPlainTextEdit;

namespace trackknife::ui {

class FormatSandboxDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit FormatSandboxDialog(QWidget* parent = nullptr);

  private:
    void updatePreview();

    QComboBox* host_{nullptr};
    QPlainTextEdit* source_{nullptr};
    QPlainTextEdit* fields_{nullptr};
    QPlainTextEdit* information_{nullptr};
    QPlainTextEdit* preview_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace trackknife::ui
