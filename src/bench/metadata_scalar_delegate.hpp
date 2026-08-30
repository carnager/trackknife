// SPDX-License-Identifier: GPL-3.0-only

#pragma once

class QAbstractItemDelegate;
class QObject;

namespace trackknife::bench {

[[nodiscard]] QAbstractItemDelegate* createMetadataScalarDelegate(QObject* parent = nullptr);

} // namespace trackknife::bench
