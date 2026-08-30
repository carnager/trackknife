// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/flac_writer.hpp"

#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <flacfile.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

constexpr std::size_t copy_buffer_size = 1U * 1024U * 1024U;
constexpr std::size_t comparison_buffer_size = 64U * 1024U;
constexpr std::size_t maximum_metadata_blocks = 4'096U;
constexpr std::uint8_t padding_block_type = 1U;
constexpr std::uint8_t vorbis_comment_block_type = 4U;

class Descriptor final {
  public:
    Descriptor() = default;
    explicit Descriptor(const int value) : value_{value} {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            static_cast<void>(close());
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~Descriptor() { static_cast<void>(close()); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

    [[nodiscard]] bool close() noexcept {
        if (!valid()) {
            return true;
        }
        const auto descriptor = std::exchange(value_, -1);
        return ::close(descriptor) == 0;
    }

  private:
    int value_{-1};
};

class PreparedPathGuard final {
  public:
    explicit PreparedPathGuard(std::string raw_path) : raw_path_{std::move(raw_path)} {}
    PreparedPathGuard(const PreparedPathGuard&) = delete;
    PreparedPathGuard& operator=(const PreparedPathGuard&) = delete;
    ~PreparedPathGuard() {
        if (owned_) {
            static_cast<void>(::unlink(raw_path_.c_str()));
        }
    }

    void take_ownership() noexcept { owned_ = true; }
    void release() noexcept { owned_ = false; }

  private:
    std::string raw_path_;
    bool owned_{false};
};

struct FlacBlock {
    std::uint8_t type{0U};
    std::uint64_t data_offset{0U};
    std::uint64_t length{0U};
};

struct FlacLayout {
    std::vector<FlacBlock> preserved_blocks;
    std::uint64_t audio_offset{0U};
    std::uint64_t file_size{0U};
};

[[nodiscard]] core::Error writer_error(const core::ErrorCode code, std::string message,
                                       const std::string& source_raw_path,
                                       const std::string& prepared_raw_path = {}) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (!prepared_raw_path.empty()) {
        result.context.push_back(
            {.key = "prepared", .value = core::escape_raw_path(prepared_raw_path)});
    }
    return result;
}

[[nodiscard]] core::Error system_error(const std::string_view operation, const int number,
                                       const std::string& source_raw_path,
                                       const std::string& prepared_raw_path = {}) {
    return writer_error(core::ErrorCode::io,
                        std::string{operation} + ": " +
                            std::error_code{number, std::generic_category()}.message(),
                        source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return writer_error(core::ErrorCode::cancelled, "prepared FLAC metadata write was cancelled",
                        source_raw_path, prepared_raw_path);
}

[[nodiscard]] bool same_revision(const struct stat& observed,
                                 const core::LocalSourceRevision& expected) {
    return observed.st_size >= 0 &&
           static_cast<std::uint64_t>(observed.st_dev) == expected.device &&
           static_cast<std::uint64_t>(observed.st_ino) == expected.inode &&
           static_cast<std::uint64_t>(observed.st_size) == expected.size &&
           static_cast<std::int64_t>(observed.st_mtim.tv_sec) ==
               expected.modification_time_seconds &&
           static_cast<std::int64_t>(observed.st_mtim.tv_nsec) ==
               expected.modification_time_nanoseconds;
}

[[nodiscard]] core::Result<mode_t> copy_source_exclusively(
    const std::string& source_raw_path, const core::LocalSourceRevision& expected_revision,
    const std::string& prepared_raw_path, const core::CancellationToken& cancellation,
    PreparedPathGuard& prepared_guard) {
    Descriptor source{::open(source_raw_path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!source.valid()) {
        return std::unexpected(system_error("opening FLAC source for copy failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    struct stat source_status{};
    if (::fstat(source.get(), &source_status) != 0) {
        return std::unexpected(system_error("observing opened FLAC source failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    if (!S_ISREG(source_status.st_mode) || !same_revision(source_status, expected_revision)) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "FLAC source changed after the write plan was previewed",
            source_raw_path, prepared_raw_path));
    }

    Descriptor destination{
        ::open(prepared_raw_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (!destination.valid()) {
        const auto number = errno;
        const auto code = number == EEXIST ? core::ErrorCode::conflict : core::ErrorCode::io;
        return std::unexpected(
            writer_error(code,
                         std::string{"creating exclusive prepared FLAC copy failed: "} +
                             std::error_code{number, std::generic_category()}.message(),
                         source_raw_path, prepared_raw_path));
    }
    prepared_guard.take_ownership();

    std::vector<char> buffer(copy_buffer_size);
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path, prepared_raw_path));
        }
        const auto read_count = ::read(source.get(), buffer.data(), buffer.size());
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(system_error("reading FLAC source copy failed", errno,
                                                source_raw_path, prepared_raw_path));
        }
        if (read_count == 0) {
            break;
        }
        std::size_t written = 0U;
        while (written < static_cast<std::size_t>(read_count)) {
            const auto write_count = ::write(destination.get(), buffer.data() + written,
                                             static_cast<std::size_t>(read_count) - written);
            if (write_count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(system_error("writing prepared FLAC copy failed", errno,
                                                    source_raw_path, prepared_raw_path));
            }
            if (write_count == 0) {
                return std::unexpected(writer_error(core::ErrorCode::io,
                                                    "prepared FLAC copy made no write progress",
                                                    source_raw_path, prepared_raw_path));
            }
            written += static_cast<std::size_t>(write_count);
        }
    }
    if (!destination.close()) {
        return std::unexpected(system_error("closing prepared FLAC copy failed", errno,
                                            source_raw_path, prepared_raw_path));
    }
    return source_status.st_mode & 07777;
}

[[nodiscard]] core::Result<FlacLayout> parse_flac_layout(const std::string& raw_path,
                                                         const std::string& source_raw_path,
                                                         const std::string& prepared_raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening FLAC layout for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io, "reading FLAC layout size failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    input.seekg(0, std::ios::beg);
    std::array<unsigned char, 4> marker{};
    input.read(reinterpret_cast<char*>(marker.data()), static_cast<std::streamsize>(marker.size()));
    if (!input || marker != std::array<unsigned char, 4>{'f', 'L', 'a', 'C'}) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "metadata writer requires native FLAC", source_raw_path,
                                            prepared_raw_path));
    }

    FlacLayout layout{.preserved_blocks = {}, .audio_offset = 0U, .file_size = file_size};
    std::uint64_t offset = marker.size();
    std::size_t block_count = 0U;
    bool last = false;
    while (!last) {
        if (block_count == maximum_metadata_blocks) {
            return std::unexpected(writer_error(core::ErrorCode::limit_exceeded,
                                                "FLAC metadata block count exceeds the write limit",
                                                source_raw_path, prepared_raw_path));
        }
        ++block_count;
        std::array<unsigned char, 4> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (!input) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "FLAC metadata block header is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        last = (header[0] & 0x80U) != 0U;
        const auto type = static_cast<std::uint8_t>(header[0] & 0x7FU);
        const auto length = (static_cast<std::uint64_t>(header[1]) << 16U) |
                            (static_cast<std::uint64_t>(header[2]) << 8U) |
                            static_cast<std::uint64_t>(header[3]);
        offset += header.size();
        if (offset > file_size || length > file_size - offset) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "FLAC metadata block payload is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        if (type != padding_block_type && type != vorbis_comment_block_type) {
            layout.preserved_blocks.push_back(
                FlacBlock{.type = type, .data_offset = offset, .length = length});
        }
        offset += length;
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "seeking across FLAC metadata failed",
                                                source_raw_path, prepared_raw_path));
        }
    }
    layout.audio_offset = offset;
    return layout;
}

[[nodiscard]] core::Result<void>
compare_region(std::ifstream& source, const std::uint64_t source_offset, std::ifstream& prepared,
               const std::uint64_t prepared_offset, const std::uint64_t length,
               const std::string_view identity, const std::string& source_raw_path,
               const std::string& prepared_raw_path, const core::CancellationToken& cancellation) {
    source.seekg(static_cast<std::streamoff>(source_offset), std::ios::beg);
    prepared.seekg(static_cast<std::streamoff>(prepared_offset), std::ios::beg);
    if (!source || !prepared) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "seeking FLAC preservation regions failed",
                                            source_raw_path, prepared_raw_path));
    }
    std::array<char, comparison_buffer_size> source_buffer{};
    std::array<char, comparison_buffer_size> prepared_buffer{};
    std::uint64_t compared = 0U;
    while (compared < length) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path, prepared_raw_path));
        }
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(source_buffer.size(), length - compared));
        source.read(source_buffer.data(), static_cast<std::streamsize>(amount));
        prepared.read(prepared_buffer.data(), static_cast<std::streamsize>(amount));
        if (source.gcount() != static_cast<std::streamsize>(amount) ||
            prepared.gcount() != static_cast<std::streamsize>(amount)) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "reading FLAC preservation regions failed",
                                                source_raw_path, prepared_raw_path));
        }
        if (!std::equal(source_buffer.begin(), source_buffer.begin() + amount,
                        prepared_buffer.begin())) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "prepared FLAC changed preserved " + std::string{identity},
                             source_raw_path, prepared_raw_path));
        }
        compared += amount;
    }
    return {};
}

[[nodiscard]] core::Result<void>
verify_flac_binary_preservation(const std::string& source_raw_path,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation) {
    auto source_layout = parse_flac_layout(source_raw_path, source_raw_path, prepared_raw_path);
    if (!source_layout) {
        return std::unexpected(std::move(source_layout.error()));
    }
    auto prepared_layout = parse_flac_layout(prepared_raw_path, source_raw_path, prepared_raw_path);
    if (!prepared_layout) {
        return std::unexpected(std::move(prepared_layout.error()));
    }
    if (source_layout->preserved_blocks.size() != prepared_layout->preserved_blocks.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC changed preserved metadata blocks",
                                            source_raw_path, prepared_raw_path));
    }

    std::ifstream source{std::filesystem::path{source_raw_path}, std::ios::binary};
    std::ifstream prepared{std::filesystem::path{prepared_raw_path}, std::ios::binary};
    if (!source || !prepared) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening FLAC preservation streams failed",
                                            source_raw_path, prepared_raw_path));
    }
    for (std::size_t index = 0U; index < source_layout->preserved_blocks.size(); ++index) {
        const auto& source_block = source_layout->preserved_blocks[index];
        const auto& prepared_block = prepared_layout->preserved_blocks[index];
        if (source_block.type != prepared_block.type ||
            source_block.length != prepared_block.length) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "prepared FLAC changed a preserved metadata block",
                                                source_raw_path, prepared_raw_path));
        }
        auto compared =
            compare_region(source, source_block.data_offset, prepared, prepared_block.data_offset,
                           source_block.length, "metadata block", source_raw_path,
                           prepared_raw_path, cancellation);
        if (!compared) {
            return compared;
        }
    }
    const auto source_audio_size = source_layout->file_size - source_layout->audio_offset;
    const auto prepared_audio_size = prepared_layout->file_size - prepared_layout->audio_offset;
    if (source_audio_size != prepared_audio_size) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC changed compressed audio size",
                                            source_raw_path, prepared_raw_path));
    }
    return compare_region(source, source_layout->audio_offset, prepared,
                          prepared_layout->audio_offset, source_audio_size, "compressed audio",
                          source_raw_path, prepared_raw_path, cancellation);
}

using EffectiveText = std::map<std::string, std::vector<std::string>>;

struct EffectiveNativeTextField {
    std::string canonical_name;
    std::vector<std::string> values;

    friend bool operator==(const EffectiveNativeTextField&,
                           const EffectiveNativeTextField&) = default;
};

using EffectiveNativeText = std::map<std::string, EffectiveNativeTextField>;

[[nodiscard]] std::string_view mapping_native_name(const MetadataWritePlanChange& change) noexcept {
    return change.exact_native_name && change.native_name.empty()
               ? std::string_view{change.display_name}
               : std::string_view{change.native_name};
}

[[nodiscard]] EffectiveText effective_text(const MetadataDocument& document) {
    EffectiveText result;
    for (const auto& field : document.effective_fields()) {
        result.emplace(field.canonical_name, field.values);
    }
    return result;
}

[[nodiscard]] EffectiveNativeText effective_native_text(const MetadataDocument& document) {
    EffectiveNativeText result;
    for (const auto& field : document.effective_native_fields()) {
        result.emplace(canonicalize_native_field_name(field.native_name),
                       EffectiveNativeTextField{.canonical_name = field.canonical_name,
                                                .values = field.values});
    }
    return result;
}

[[nodiscard]] core::Result<void> verify_text_result(const MetadataDocument& before,
                                                    const MetadataDocument& after,
                                                    const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path) {
    auto expected = effective_native_text(before);
    for (const auto& change : source_plan.changes) {
        const auto& intent = change.intents.front();
        if (change.exact_native_name) {
            expected.erase(*change.exact_native_name);
            if (intent.kind == StagedMetadataPatchKind::replace_values) {
                auto mapping =
                    map_flac_text_field(change.canonical_name, change.display_name,
                                        mapping_native_name(change), intent.kind, intent.values);
                if (!mapping) {
                    return std::unexpected(std::move(mapping.error()));
                }
                const auto identity = resolve_text_property_identity(mapping->property_name);
                expected[canonicalize_native_field_name(mapping->property_name)] =
                    EffectiveNativeTextField{.canonical_name = identity.canonical_name,
                                             .values = intent.values};
            }
            continue;
        }
        std::erase_if(expected, [&change](const auto& entry) {
            const auto identity = resolve_text_property_identity(entry.first);
            return identity.conventional && identity.canonical_name == change.canonical_name;
        });
        if (intent.kind == StagedMetadataPatchKind::replace_values) {
            auto mapping =
                map_flac_text_field(change.canonical_name, change.display_name,
                                    mapping_native_name(change), intent.kind, intent.values);
            if (!mapping) {
                return std::unexpected(std::move(mapping.error()));
            }
            const auto identity = resolve_text_property_identity(mapping->property_name);
            expected[canonicalize_native_field_name(mapping->property_name)] =
                EffectiveNativeTextField{.canonical_name = identity.canonical_name,
                                         .values = intent.values};
        }
    }
    if (before.unsupported_native_objects != after.unsupported_native_objects) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "prepared FLAC changed unsupported native metadata identities",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (expected != effective_native_text(after)) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC metadata reread differs from the plan",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> verify_plan_originals(const MetadataDocument& before,
                                                       const MetadataWritePlanSource& source_plan,
                                                       const std::string& prepared_raw_path) {
    const auto current = effective_text(before);
    const auto current_native = effective_native_text(before);
    for (const auto& change : source_plan.changes) {
        bool present = false;
        std::vector<std::string> values;
        if (change.exact_native_name) {
            const auto found = current_native.find(*change.exact_native_name);
            present = found != current_native.end();
            if (present) {
                values = found->second.values;
            }
        } else {
            const auto found = current.find(change.canonical_name);
            present = found != current.end();
            if (present) {
                values = found->second;
            }
        }
        if (present != change.original_present || (present && values != change.original_values)) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "fresh FLAC metadata differs from the previewed original values",
                             source_plan.raw_path, prepared_raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    TagLib::FLAC::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared FLAC copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    auto properties = file.properties();
    for (const auto& change : source_plan.changes) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
        }
        const auto& intent = change.intents.front();
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           mapping_native_name(change), intent.kind, intent.values);
        if (!mapping) {
            auto mapping_error = std::move(mapping.error());
            mapping_error.context.push_back(
                {.key = "source", .value = core::escape_raw_path(source_plan.raw_path)});
            return std::unexpected(std::move(mapping_error));
        }

        std::vector<TagLib::String> aliases;
        for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
            const auto native_name = property->first.to8Bit(true);
            const auto matches =
                change.exact_native_name
                    ? canonicalize_native_field_name(native_name) == *change.exact_native_name
                    : [&] {
                          const auto identity = resolve_text_property_identity(native_name);
                          return identity.conventional &&
                                 identity.canonical_name == change.canonical_name;
                      }();
            if (matches) {
                aliases.push_back(property->first);
            }
        }
        for (const auto& alias : aliases) {
            properties.erase(alias);
        }
        if (intent.kind == StagedMetadataPatchKind::replace_values) {
            TagLib::StringList values;
            for (const auto& value : intent.values) {
                values.append(TagLib::String{value, TagLib::String::UTF8});
            }
            if (!properties.replace(TagLib::String{mapping->property_name, TagLib::String::UTF8},
                                    values)) {
                return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                                    "TagLib rejected the FLAC property mapping",
                                                    source_plan.raw_path, prepared_raw_path));
            }
        }
    }
    const auto unsupported = file.setProperties(properties);
    if (!unsupported.isEmpty()) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "TagLib could not map every planned FLAC property",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib failed to save the prepared FLAC copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return {};
}

} // namespace

core::Result<PreparedFlacMetadataWrite>
prepare_flac_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                 const std::string& prepared_raw_path,
                                 const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared FLAC write paths must be nonempty raw paths",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared FLAC path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != "taglib-flac-v1" ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared FLAC write requires one ready taglib-flac-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared FLAC plan contains an unresolved change",
                                                source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared FLAC plan contains conflicting intents",
                                                source_plan.raw_path, prepared_raw_path));
        }
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           mapping_native_name(change), first.kind, first.values);
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
            core::ErrorCode::conflict, "FLAC source changed after the write plan was previewed",
            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != "taglib-flac-v1") {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "FLAC source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified =
        verify_plan_originals(before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = copy_source_exclusively(source_plan.raw_path, *source_plan.observed_revision,
                                               prepared_raw_path, cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared FLAC permissions failed", errno,
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    auto after = read_local_metadata(prepared_raw_path, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto text_verified =
        verify_text_result(before->document, after->document, source_plan, prepared_raw_path);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto binary_verified =
        verify_flac_binary_preservation(source_plan.raw_path, prepared_raw_path, cancellation);
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
                         "FLAC source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedFlacMetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

} // namespace trackknife::metadata
