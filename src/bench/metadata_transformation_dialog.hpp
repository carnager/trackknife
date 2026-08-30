// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/metadata_properties_dialog.hpp"

#include <QStringList>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

using MetadataTransformationStageCallback =
    std::function<bool(const metadata::MetadataTransformationPreview& preview)>;

[[nodiscard]] QDialog* createMetadataTransformationDialog(
    std::shared_ptr<const metadata::StagedMetadataSelection> selection,
    metadata::StagedMetadataPatchSet draft, std::vector<std::size_t> item_indexes,
    QStringList track_labels, MetadataTransformationStageCallback stage,
    MetadataTransformationStore store, QWidget* parent,
    std::optional<core::StableId> initially_selected = std::nullopt,
    bool preview_initially_selected = false, MetadataDialogLayoutStore layout_store = {});

} // namespace trackknife::bench
