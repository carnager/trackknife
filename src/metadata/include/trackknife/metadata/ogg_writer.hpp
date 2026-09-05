// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <string>

namespace trackknife::metadata {

using PreparedOggMetadataWrite = PreparedFlacMetadataWrite;

// Creates and verifies a distinct Ogg Vorbis or Ogg Opus copy for one ready
// write-plan source (ADR-0114). Tags live in the Vorbis comment packet
// inside the Ogg stream, and rewriting it legitimately relayouts pages, so
// preservation is proven at the packet layer: every logical packet except
// the comment packet — codec headers, setup, and all audio — must be
// byte-identical between source and prepared copy, and the reread text must
// exactly match the plan. Vorbis comments carry Picard-paired totals
// spellings, so paired writing is enabled. This synchronous I/O primitive
// belongs on a bounded mutation worker; it never replaces the user's file.
[[nodiscard]] core::Result<PreparedOggMetadataWrite>
prepare_ogg_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
