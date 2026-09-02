// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <string>

namespace trackknife::metadata {

// The prepared text write carries the same evidence for every format.
using PreparedWavPackMetadataWrite = PreparedFlacMetadataWrite;

// Creates and verifies a distinct WavPack copy for one ready write-plan
// source (ADR-0095). The source is never opened for writing and
// prepared_raw_path must not exist. Verification proves the WavPack audio
// blocks byte-identical, every APEv2 binary item byte-identical, and the
// reread text exactly the planned result; sources carrying an ID3v1
// trailer are rejected rather than half-preserved. This synchronous I/O
// primitive belongs on a bounded mutation worker; it is not a
// commit/publish operation and does not replace the user's file.
[[nodiscard]] core::Result<PreparedWavPackMetadataWrite>
prepare_wavpack_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                    const std::string& prepared_raw_path,
                                    const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
