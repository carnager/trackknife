// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <cstddef>
#include <string>

namespace trackknife::metadata {

struct PreparedFlacMetadataWrite {
    std::string source_raw_path;
    std::string prepared_raw_path;
    core::LocalSourceRevision source_revision;
    core::LocalSourceRevision prepared_revision;
    MetadataDocument document;
    std::size_t field_change_count{0U};

    friend bool operator==(const PreparedFlacMetadataWrite&,
                           const PreparedFlacMetadataWrite&) = default;
};

// Creates and verifies a distinct FLAC copy for one ready write-plan source.
// The source is never opened for writing and prepared_raw_path must not exist.
// This synchronous I/O primitive belongs on a bounded mutation worker; it is
// not a commit/publish operation and does not replace the user's file.
[[nodiscard]] core::Result<PreparedFlacMetadataWrite>
prepare_flac_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                 const std::string& prepared_raw_path,
                                 const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
