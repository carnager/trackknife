// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <QByteArray>
#include <QString>

namespace trackknife::ui {

inline constexpr qsizetype metadata_transformation_interchange_maximum_bytes = 8 * 1'024 * 1'024;

// Serializes one validated typed chain. Saved identity and automatic-selection
// state deliberately live outside this portable definition.
[[nodiscard]] core::Result<QByteArray>
serializeMetadataTransformationChain(const metadata::MetadataTransformationChain& chain);

// Accepts only the complete version-1 native envelope. Unknown keys and newer
// versions fail closed instead of being silently discarded on a later save.
[[nodiscard]] core::Result<metadata::MetadataTransformationChain>
deserializeMetadataTransformationChain(const QByteArray& bytes);

// These bounded synchronous helpers are intended for an I/O worker. Export
// uses QSaveFile so an existing definition is replaced atomically.
[[nodiscard]] core::Result<metadata::MetadataTransformationChain>
loadMetadataTransformationChainFile(const QString& path);
[[nodiscard]] core::Result<void>
saveMetadataTransformationChainFile(const QString& path,
                                    const metadata::MetadataTransformationChain& chain);

} // namespace trackknife::ui
