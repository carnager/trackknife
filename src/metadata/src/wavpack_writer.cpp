// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/wavpack_writer.hpp"

#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <wavpackfile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

using text_writer_detail::PreparedPathGuard;
using text_writer_detail::system_error;
using text_writer_detail::writer_error;

constexpr std::string_view wavpack_label = "WavPack";
constexpr std::string_view wavpack_adapter = "taglib-wavpack-v1";
constexpr std::size_t ape_footer_size = 32U;
constexpr std::size_t id3v1_size = 128U;
constexpr std::size_t maximum_ape_items = 4'096U;
constexpr std::uint32_t ape_header_present_flag = 0x8000'0000U;
constexpr std::uint32_t ape_item_kind_mask = 0x0000'0006U;

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(wavpack_label, source_raw_path, prepared_raw_path);
}

struct ApeItem {
    std::string key;
    std::vector<unsigned char> value;
    std::uint32_t flags{0U};
};

struct WavPackLayout {
    // Bytes before any trailing APEv2 tag: the WavPack blocks themselves.
    std::uint64_t audio_size{0U};
    std::uint64_t file_size{0U};
    // Every non-text APEv2 item (binary and external locators), by key.
    std::map<std::string, ApeItem> preserved_items;
};

[[nodiscard]] std::uint32_t little_endian_32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_file_bytes(const std::string& raw_path, const std::string& source_raw_path,
                const std::string& prepared_raw_path) {
    std::ifstream input{raw_path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening a WavPack file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "observing a WavPack file size failed", source_raw_path,
                                            prepared_raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "reading a WavPack file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    return bytes;
}

// Parses the file layout from the trailer inward: an optional ID3v1 block is
// rejected outright, then an optional APEv2 tag (footer required, header
// honoured through its flag) is itemized so binary payloads can be compared
// byte-exactly. Everything before the tag is the audio region.
[[nodiscard]] core::Result<WavPackLayout>
parse_wavpack_layout(const std::vector<unsigned char>& bytes, const std::string& source_raw_path,
                     const std::string& prepared_raw_path) {
    WavPackLayout layout{
        .audio_size = bytes.size(), .file_size = bytes.size(), .preserved_items = {}};
    if (bytes.size() < 4U ||
        !(bytes[0] == 'w' && bytes[1] == 'v' && bytes[2] == 'p' && bytes[3] == 'k')) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the file does not start with a WavPack block",
                                            source_raw_path, prepared_raw_path));
    }
    std::size_t trailer_end = bytes.size();
    if (trailer_end >= id3v1_size && bytes[trailer_end - id3v1_size] == 'T' &&
        bytes[trailer_end - id3v1_size + 1U] == 'A' &&
        bytes[trailer_end - id3v1_size + 2U] == 'G') {
        return std::unexpected(
            writer_error(core::ErrorCode::unsupported,
                         "WavPack sources carrying an ID3v1 trailer are not qualified",
                         source_raw_path, prepared_raw_path));
    }
    if (trailer_end < ape_footer_size) {
        return layout;
    }
    const auto* footer = bytes.data() + trailer_end - ape_footer_size;
    static constexpr std::array<unsigned char, 8> preamble{'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X'};
    if (!std::equal(preamble.begin(), preamble.end(), footer)) {
        return layout;
    }
    const auto tag_size = little_endian_32(footer + 12U);
    const auto item_count = little_endian_32(footer + 16U);
    const auto flags = little_endian_32(footer + 20U);
    const auto header_size = (flags & ape_header_present_flag) != 0U ? ape_footer_size : 0U;
    const std::uint64_t total = static_cast<std::uint64_t>(tag_size) + header_size;
    if (tag_size < ape_footer_size || total > trailer_end || item_count > maximum_ape_items) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the WavPack APEv2 trailer is malformed",
                                            source_raw_path, prepared_raw_path));
    }
    layout.audio_size = trailer_end - total;
    std::size_t cursor = static_cast<std::size_t>(layout.audio_size) + header_size;
    const auto items_end = trailer_end - ape_footer_size;
    for (std::uint32_t index = 0U; index < item_count; ++index) {
        if (cursor + 8U > items_end) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "a WavPack APEv2 item is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        const auto value_size = little_endian_32(bytes.data() + cursor);
        const auto item_flags = little_endian_32(bytes.data() + cursor + 4U);
        cursor += 8U;
        const auto key_begin = cursor;
        while (cursor < items_end && bytes[cursor] != 0U) {
            ++cursor;
        }
        if (cursor >= items_end || cursor + 1U + value_size > items_end + 1U ||
            value_size > items_end - cursor - 1U) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "a WavPack APEv2 item is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        std::string key{reinterpret_cast<const char*>(bytes.data() + key_begin),
                        cursor - key_begin};
        cursor += 1U;
        if ((item_flags & ape_item_kind_mask) != 0U) {
            ApeItem item{
                .key = std::move(key),
                .value = {bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                          bytes.begin() + static_cast<std::ptrdiff_t>(cursor + value_size)},
                .flags = item_flags,
            };
            auto folded = canonicalize_native_field_name(item.key);
            layout.preserved_items.emplace(std::move(folded), std::move(item));
        }
        cursor += value_size;
    }
    return layout;
}

// The qualification proof: audio bytes identical, every binary/external
// APEv2 item carried over byte-exactly, and none invented.
[[nodiscard]] core::Result<void>
verify_wavpack_binary_preservation(const std::string& source_raw_path,
                                   const std::string& prepared_raw_path,
                                   const core::CancellationToken& cancellation) {
    auto source_bytes = read_file_bytes(source_raw_path, source_raw_path, prepared_raw_path);
    if (!source_bytes) {
        return std::unexpected(std::move(source_bytes.error()));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_raw_path, prepared_raw_path));
    }
    auto prepared_bytes = read_file_bytes(prepared_raw_path, source_raw_path, prepared_raw_path);
    if (!prepared_bytes) {
        return std::unexpected(std::move(prepared_bytes.error()));
    }
    auto source_layout = parse_wavpack_layout(*source_bytes, source_raw_path, prepared_raw_path);
    if (!source_layout) {
        return std::unexpected(std::move(source_layout.error()));
    }
    auto prepared_layout =
        parse_wavpack_layout(*prepared_bytes, source_raw_path, prepared_raw_path);
    if (!prepared_layout) {
        return std::unexpected(std::move(prepared_layout.error()));
    }
    if (source_layout->audio_size != prepared_layout->audio_size ||
        !std::equal(source_bytes->begin(),
                    source_bytes->begin() + static_cast<std::ptrdiff_t>(source_layout->audio_size),
                    prepared_bytes->begin())) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared WavPack audio blocks differ from the source",
                                            source_raw_path, prepared_raw_path));
    }
    if (source_layout->preserved_items.size() != prepared_layout->preserved_items.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared WavPack changed the APEv2 binary item set",
                                            source_raw_path, prepared_raw_path));
    }
    for (const auto& [key, item] : source_layout->preserved_items) {
        const auto found = prepared_layout->preserved_items.find(key);
        if (found == prepared_layout->preserved_items.end() || found->second.value != item.value ||
            (found->second.flags & ape_item_kind_mask) != (item.flags & ape_item_kind_mask)) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "a prepared WavPack APEv2 binary item differs from the source",
                             source_raw_path, prepared_raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    TagLib::WavPack::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared WavPack copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return text_writer_detail::apply_text_changes_to_properties(wavpack_label, source_plan, file,
                                                                prepared_raw_path, cancellation);
}

} // namespace

core::Result<PreparedWavPackMetadataWrite>
prepare_wavpack_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                    const std::string& prepared_raw_path,
                                    const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared WavPack write paths must be nonempty raw paths",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared WavPack path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != wavpack_adapter ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared WavPack write requires one ready taglib-wavpack-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(
                writer_error(core::ErrorCode::invalid_argument,
                             "prepared WavPack plan contains an unresolved change",
                             source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(
                writer_error(core::ErrorCode::invalid_argument,
                             "prepared WavPack plan contains conflicting intents",
                             source_plan.raw_path, prepared_raw_path));
        }
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           text_writer_detail::mapping_native_name(change),
                                           first.kind, first.values);
        if (!mapping) {
            return std::unexpected(std::move(mapping.error()));
        }
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_revision) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "WavPack source changed after the write plan was previewed",
            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != wavpack_adapter) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "WavPack source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified = text_writer_detail::verify_plan_originals(
        wavpack_label, before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        wavpack_label, source_plan.raw_path, *source_plan.observed_revision, prepared_raw_path,
        cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared WavPack permissions failed", errno,
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    auto after = read_local_metadata(prepared_raw_path, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto text_verified = text_writer_detail::verify_text_result(
        wavpack_label, before->document, after->document, source_plan, prepared_raw_path);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto binary_verified =
        verify_wavpack_binary_preservation(source_plan.raw_path, prepared_raw_path, cancellation);
    if (!binary_verified) {
        return std::unexpected(std::move(binary_verified.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "WavPack source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedWavPackMetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

} // namespace trackknife::metadata
