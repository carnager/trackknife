// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_apply.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

struct MetadataApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::MetadataApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

using MetadataApplyCancelCallback = std::function<void()>;

[[nodiscard]] QDialog* createMetadataApplyDialog(const metadata::MetadataWritePlan& plan,
                                                 MetadataApplyCancelCallback cancel,
                                                 QWidget* parent);
void updateMetadataApplyDialog(QDialog& dialog, const MetadataApplyProgressState& progress);
void finishMetadataApplyDialog(QDialog& dialog,
                               const core::Result<operations::MetadataApplyResult>& result);

} // namespace trackknife::bench
