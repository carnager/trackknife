// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <string>

namespace trackknife::metadata {

using PreparedMp3MetadataWrite = PreparedFlacMetadataWrite;

// Creates and verifies a distinct MP3 copy for one ready write-plan source
// (ADR-0103). Tags live in the leading ID3v2 block; verification proves the
// MPEG audio region — everything between the leading tag and any trailing
// ID3v1/APEv2 tags — byte-identical, every trailing APEv2 binary item
// byte-exact, and the reread text exactly the planned result. ID3 has no
// Picard-paired totals spellings, so paired writing is disabled. This
// synchronous I/O primitive belongs on a bounded mutation worker; it is not
// a commit/publish operation and does not replace the user's file.
[[nodiscard]] core::Result<PreparedMp3MetadataWrite>
prepare_mp3_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation = {});

// Dispatches one ready source plan to its qualified format writer by
// adapter name (taglib-flac-v1, taglib-wavpack-v1, taglib-mpeg-v1).
[[nodiscard]] core::Result<PreparedFlacMetadataWrite>
prepare_qualified_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                      const std::string& prepared_raw_path,
                                      const core::CancellationToken& cancellation = {});

// The adapters whose text writers are preservation-qualified.
[[nodiscard]] bool is_qualified_text_adapter(std::string_view adapter_name);

} // namespace trackknife::metadata
