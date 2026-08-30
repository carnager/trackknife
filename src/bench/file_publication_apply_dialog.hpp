// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

class QDialog;
class QWidget;

namespace trackknife::bench {

struct FilePublicationApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::FilePublicationApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

using FilePublicationApplyCancelCallback = std::function<void()>;

[[nodiscard]] QDialog*
createFilePublicationApplyDialog(const operations::OutputPathPreflight& preflight,
                                 FilePublicationApplyCancelCallback cancel, QWidget* parent);
void updateFilePublicationApplyDialog(QDialog& dialog,
                                      const FilePublicationApplyProgressState& progress);
void finishFilePublicationApplyDialog(
    QDialog& dialog, const core::Result<operations::FilePublicationApplyResult>& result);

} // namespace trackknife::bench
