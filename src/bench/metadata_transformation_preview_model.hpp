// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QStringList>

#include <memory>

class QAbstractItemModel;
class QObject;

namespace trackknife::metadata {
struct MetadataTransformationPreview;
}

namespace trackknife::bench {

[[nodiscard]] QAbstractItemModel* createMetadataTransformationPreviewModel(
    std::shared_ptr<const metadata::MetadataTransformationPreview> preview,
    QStringList track_labels, QObject* parent = nullptr);

} // namespace trackknife::bench
