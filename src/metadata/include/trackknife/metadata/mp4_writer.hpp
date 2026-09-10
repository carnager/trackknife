// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <string>

namespace trackknife::metadata {

using PreparedMp4MetadataWrite = PreparedFlacMetadataWrite;

// Creates and verifies a distinct MP4/M4A copy for one ready write-plan
// source (ADR-0136). Standard atoms map onto the conventional logical
// names through TagLib's documented table; every other property becomes an
// exact freeform `----:com.apple.iTunes:<NAME>` atom, and `trkn` carries
// its total inside the combined TRACKNUMBER value, so paired-totals
// expansion stays off. TagLib rewrites the `moov` box, so preservation is
// proven at the box layer: `ftyp`, every `mdat`, and every other
// non-`moov`/`free` top-level box must be byte-identical between source
// and prepared copy, and the reread text must exactly match the plan. This
// synchronous I/O primitive belongs on a bounded mutation worker; it never
// replaces the user's file.
[[nodiscard]] core::Result<PreparedMp4MetadataWrite>
prepare_mp4_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
