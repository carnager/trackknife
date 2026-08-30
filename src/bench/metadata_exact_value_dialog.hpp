// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

[[nodiscard]] QDialog* createMetadataExactValueDialog(const QString& heading,
                                                      const QString& context, QStringList values,
                                                      QWidget* parent);
[[nodiscard]] std::vector<std::string> metadataExactValueDialogValues(const QDialog* dialog);

} // namespace trackknife::bench
