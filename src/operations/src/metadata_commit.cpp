// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/metadata_commit.hpp"

#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1U)
#endif

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_xattr_name_bytes = 1U * 1024U * 1024U;
constexpr std::size_t maximum_xattr_count = 4'096U;
constexpr std::size_t maximum_xattr_value_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_total_xattr_bytes = 64U * 1024U * 1024U;
constexpr auto lock_retry_interval = std::chrono::milliseconds{10};

using State = MetadataOperationJournalState;
using BackupState = MetadataOperationBackupState;

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
        const auto value = std::exchange(value_, -1);
        return ::close(value) == 0;
    }

  private:
    int value_{-1};
};

struct PhysicalKey {
    std::uint64_t device{0U};
    std::uint64_t inode{0U};

    friend bool operator==(const PhysicalKey&, const PhysicalKey&) = default;
};

struct PhysicalKeyHash {
    [[nodiscard]] std::size_t operator()(const PhysicalKey& key) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(key.device);
        const auto second = std::hash<std::uint64_t>{}(key.inode);
        return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
    }
};

class ProcessSourceLock final {
  public:
    ProcessSourceLock(std::shared_ptr<std::timed_mutex> mutex,
                      std::unique_lock<std::timed_mutex> lock)
        : mutex_{std::move(mutex)}, lock_{std::move(lock)} {}
    ProcessSourceLock(ProcessSourceLock&&) noexcept = default;
    ProcessSourceLock& operator=(ProcessSourceLock&&) noexcept = default;
    ProcessSourceLock(const ProcessSourceLock&) = delete;
    ProcessSourceLock& operator=(const ProcessSourceLock&) = delete;

  private:
    std::shared_ptr<std::timed_mutex> mutex_;
    std::unique_lock<std::timed_mutex> lock_;
};

std::mutex process_lock_registry_mutex;
std::unordered_map<PhysicalKey, std::weak_ptr<std::timed_mutex>, PhysicalKeyHash>
    process_lock_registry;

[[nodiscard]] core::Error
operation_error(const core::ErrorCode code, std::string message, const std::string& source_raw_path,
                const std::optional<core::StableId>& journal_id = std::nullopt) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (journal_id) {
        result.context.push_back({.key = "journal_id", .value = journal_id->to_string()});
    }
    return result;
}

[[nodiscard]] core::Error
system_error(const std::string_view operation, const int number, const std::string& source_raw_path,
             const std::optional<core::StableId>& journal_id = std::nullopt) {
    return operation_error(core::ErrorCode::io,
                           std::string{operation} + ": " +
                               std::error_code{number, std::generic_category()}.message(),
                           source_raw_path, journal_id);
}

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path) {
    return operation_error(core::ErrorCode::cancelled,
                           "metadata commit was cancelled before atomic publication",
                           source_raw_path);
}

[[nodiscard]] core::Result<ProcessSourceLock>
acquire_process_lock(const core::LocalSourceRevision& revision,
                     const core::CancellationToken& cancellation,
                     const std::string& source_raw_path) {
    const PhysicalKey key{.device = revision.device, .inode = revision.inode};
    std::shared_ptr<std::timed_mutex> mutex;
    {
        std::scoped_lock registry_lock{process_lock_registry_mutex};
        for (auto iterator = process_lock_registry.begin();
             iterator != process_lock_registry.end();) {
            if (iterator->second.expired()) {
                iterator = process_lock_registry.erase(iterator);
            } else {
                ++iterator;
            }
        }
        auto& retained = process_lock_registry[key];
        mutex = retained.lock();
        if (!mutex) {
            mutex = std::make_shared<std::timed_mutex>();
            retained = mutex;
        }
    }
    std::unique_lock<std::timed_mutex> lock{*mutex, std::defer_lock};
    while (!lock.try_lock_for(lock_retry_interval)) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path));
        }
    }
    return ProcessSourceLock{std::move(mutex), std::move(lock)};
}

[[nodiscard]] core::Result<Descriptor>
open_and_lock_file(const std::string& raw_path, const core::CancellationToken& cancellation,
                   const std::string& source_raw_path,
                   const std::optional<core::StableId>& journal_id = std::nullopt) {
    Descriptor descriptor{::open(raw_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor.valid()) {
        return std::unexpected(
            system_error("opening mutation source failed", errno, source_raw_path, journal_id));
    }
    // Exclusive when the filesystem can express it; NFS needs write-open
    // descriptors for exclusive flock emulation, so read-only descriptors
    // degrade to shared or unlocked there. Revision revalidation before
    // every mutation carries correctness either way (ADR-0111).
    auto operation = LOCK_EX | LOCK_NB;
    while (::flock(descriptor.get(), operation) != 0) {
        const auto number = errno;
        if (number == EBADF && (operation & LOCK_EX) != 0) {
            operation = LOCK_SH | LOCK_NB;
            continue;
        }
        if (number == ENOLCK || number == EOPNOTSUPP || number == ENOTSUP ||
            (number == EBADF && (operation & LOCK_SH) != 0)) {
            return descriptor;
        }
        if (number != EWOULDBLOCK && number != EAGAIN && number != EINTR) {
            return std::unexpected(system_error("locking mutation source failed", number,
                                                source_raw_path, journal_id));
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path));
        }
        std::this_thread::sleep_for(lock_retry_interval);
    }
    return descriptor;
}

[[nodiscard]] core::LocalSourceRevision revision_from_stat(const struct stat& status) {
    return core::LocalSourceRevision{
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .modification_time_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
        .modification_time_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec),
    };
}

[[nodiscard]] core::Result<struct stat>
locked_status(const Descriptor& descriptor, const std::string& source_raw_path,
              const std::optional<core::StableId>& journal_id = std::nullopt) {
    struct stat status{};
    if (::fstat(descriptor.get(), &status) != 0) {
        return std::unexpected(
            system_error("observing locked source failed", errno, source_raw_path, journal_id));
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return std::unexpected(operation_error(core::ErrorCode::unsupported,
                                               "metadata commit requires a regular file",
                                               source_raw_path, journal_id));
    }
    return status;
}

[[nodiscard]] core::Result<struct stat>
require_direct_single_link_source(const Descriptor& descriptor, const std::string& source_raw_path,
                                  const core::LocalSourceRevision& expected_revision) {
    struct stat path_status{};
    if (::lstat(source_raw_path.c_str(), &path_status) != 0) {
        return std::unexpected(
            system_error("observing metadata source path failed", errno, source_raw_path));
    }
    auto descriptor_status = locked_status(descriptor, source_raw_path);
    if (!descriptor_status) {
        return std::unexpected(std::move(descriptor_status.error()));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        path_status.st_dev != descriptor_status->st_dev ||
        path_status.st_ino != descriptor_status->st_ino) {
        return std::unexpected(operation_error(
            core::ErrorCode::unsupported,
            "metadata commit does not yet support symlink or hard-linked source paths",
            source_raw_path));
    }
    if (revision_from_stat(*descriptor_status) != expected_revision) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict, "metadata source changed after the write plan was previewed",
            source_raw_path));
    }
    return *descriptor_status;
}

struct ExtendedAttribute {
    std::string name;
    std::vector<unsigned char> value;

    friend bool operator==(const ExtendedAttribute&, const ExtendedAttribute&) = default;
};

// Tag commits keep source and prepared copy on one filesystem, so a
// filesystem without extended attributes has none to lose on either side;
// the unsupported listing degrades preservation silently (ADR-0111).
struct ExtendedAttributeListing {
    std::vector<ExtendedAttribute> attributes;
    bool supported{true};
};

[[nodiscard]] core::Result<ExtendedAttributeListing>
read_extended_attributes(const Descriptor& descriptor, const std::string& source_raw_path,
                         const std::optional<core::StableId>& journal_id = std::nullopt) {
    const auto listed = ::flistxattr(descriptor.get(), nullptr, 0U);
    if (listed < 0 && (errno == ENOTSUP || errno == EOPNOTSUPP)) {
        return ExtendedAttributeListing{.attributes = {}, .supported = false};
    }
    if (listed < 0) {
        return std::unexpected(
            system_error("listing extended attributes failed", errno, source_raw_path, journal_id));
    }
    if (static_cast<std::size_t>(listed) > maximum_xattr_name_bytes) {
        return std::unexpected(operation_error(core::ErrorCode::limit_exceeded,
                                               "extended-attribute names exceed the commit limit",
                                               source_raw_path, journal_id));
    }
    std::vector<char> names(static_cast<std::size_t>(listed));
    if (listed > 0) {
        const auto repeated = ::flistxattr(descriptor.get(), names.data(), names.size());
        if (repeated < 0 || repeated != listed) {
            return std::unexpected(
                operation_error(repeated < 0 ? core::ErrorCode::io : core::ErrorCode::conflict,
                                repeated < 0 ? "reading extended-attribute names failed"
                                             : "extended attributes changed while being read",
                                source_raw_path, journal_id));
        }
    }

    std::vector<ExtendedAttribute> attributes;
    std::size_t offset = 0U;
    std::size_t total_value_bytes = 0U;
    while (offset < names.size()) {
        const auto end =
            std::find(names.begin() + static_cast<std::ptrdiff_t>(offset), names.end(), '\0');
        if (end == names.end() || end == names.begin() + static_cast<std::ptrdiff_t>(offset) ||
            attributes.size() == maximum_xattr_count) {
            return std::unexpected(operation_error(core::ErrorCode::backend,
                                                   "extended-attribute name list is malformed",
                                                   source_raw_path, journal_id));
        }
        std::string name{names.begin() + static_cast<std::ptrdiff_t>(offset), end};
        const auto value_size = ::fgetxattr(descriptor.get(), name.c_str(), nullptr, 0U);
        if (value_size < 0) {
            return std::unexpected(system_error("sizing extended attribute failed", errno,
                                                source_raw_path, journal_id));
        }
        if (static_cast<std::size_t>(value_size) > maximum_xattr_value_bytes ||
            static_cast<std::size_t>(value_size) > maximum_total_xattr_bytes - total_value_bytes) {
            return std::unexpected(operation_error(core::ErrorCode::limit_exceeded,
                                                   "extended-attribute values exceed commit limits",
                                                   source_raw_path, journal_id));
        }
        std::vector<unsigned char> value(static_cast<std::size_t>(value_size));
        if (value_size > 0) {
            const auto read =
                ::fgetxattr(descriptor.get(), name.c_str(), value.data(), value.size());
            if (read < 0 || read != value_size) {
                return std::unexpected(
                    operation_error(read < 0 ? core::ErrorCode::io : core::ErrorCode::conflict,
                                    read < 0 ? "reading extended attribute failed"
                                             : "extended attribute changed while being read",
                                    source_raw_path, journal_id));
            }
        }
        total_value_bytes += value.size();
        // Only user-namespace attributes are user metadata; system.,
        // security., and trusted. names are kernel- or filesystem-owned
        // representations (NFS ACLs, SELinux labels) that the destination
        // manages itself and often refuses to accept or remove.
        if (name.starts_with("user.")) {
            attributes.push_back(
                ExtendedAttribute{.name = std::move(name), .value = std::move(value)});
        }
        offset = static_cast<std::size_t>(std::distance(names.begin(), end)) + 1U;
    }
    std::ranges::sort(attributes, {}, &ExtendedAttribute::name);
    return ExtendedAttributeListing{.attributes = std::move(attributes), .supported = true};
}

[[nodiscard]] core::Result<void>
apply_filesystem_metadata(const Descriptor& source, const struct stat& source_status,
                          const ExtendedAttributeListing& source_attributes,
                          const Descriptor& prepared, const std::string& source_raw_path,
                          const core::StableId& journal_id) {
    auto prepared_attributes = read_extended_attributes(prepared, source_raw_path, journal_id);
    if (!prepared_attributes) {
        return std::unexpected(std::move(prepared_attributes.error()));
    }
    if (prepared_attributes->supported) {
        for (const auto& attribute : prepared_attributes->attributes) {
            if (std::ranges::none_of(source_attributes.attributes,
                                     [&attribute](const auto& source_attribute) {
                                         return source_attribute.name == attribute.name;
                                     }) &&
                ::fremovexattr(prepared.get(), attribute.name.c_str()) != 0) {
                return std::unexpected(system_error("removing unowned extended attribute failed",
                                                    errno, source_raw_path, journal_id));
            }
        }
    }
    struct stat prepared_status{};
    if (::fstat(prepared.get(), &prepared_status) != 0) {
        return std::unexpected(system_error("observing prepared ownership failed", errno,
                                            source_raw_path, journal_id));
    }
    // Filesystems with server-side identity mapping refuse ownership
    // changes; source and prepared copy share the filesystem, so the
    // published file keeps the identity the filesystem enforces anyway.
    bool ownership_preserved = true;
    if (prepared_status.st_uid != source_status.st_uid ||
        prepared_status.st_gid != source_status.st_gid) {
        if (::fchown(prepared.get(), source_status.st_uid, source_status.st_gid) != 0) {
            if (errno == EPERM || errno == ENOTSUP || errno == EOPNOTSUPP) {
                ownership_preserved = false;
            } else {
                return std::unexpected(system_error("preserving prepared ownership failed", errno,
                                                    source_raw_path, journal_id));
            }
        }
    }
    if (::fchmod(prepared.get(), source_status.st_mode & 07777) != 0) {
        return std::unexpected(system_error("preserving prepared permissions failed", errno,
                                            source_raw_path, journal_id));
    }
    if (prepared_attributes->supported) {
        for (const auto& attribute : source_attributes.attributes) {
            const auto* data = attribute.value.empty() ? nullptr : attribute.value.data();
            if (::fsetxattr(prepared.get(), attribute.name.c_str(), data, attribute.value.size(),
                            0) != 0) {
                return std::unexpected(system_error("preserving extended attribute failed", errno,
                                                    source_raw_path, journal_id));
            }
        }
    }
    auto source_after = read_extended_attributes(source, source_raw_path, journal_id);
    auto prepared_after = read_extended_attributes(prepared, source_raw_path, journal_id);
    if (!source_after || !prepared_after) {
        return std::unexpected(!source_after ? std::move(source_after.error())
                                             : std::move(prepared_after.error()));
    }
    if ((source_after->supported && source_attributes.supported &&
         source_after->attributes != source_attributes.attributes) ||
        (prepared_after->supported && source_attributes.supported &&
         prepared_after->attributes != source_attributes.attributes)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "filesystem metadata changed during preparation",
                                               source_raw_path, journal_id));
    }
    if (::fstat(prepared.get(), &prepared_status) != 0 ||
        (ownership_preserved && (prepared_status.st_uid != source_status.st_uid ||
                                 prepared_status.st_gid != source_status.st_gid)) ||
        (prepared_status.st_mode & 07777) != (source_status.st_mode & 07777)) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "prepared ownership or permissions differ",
                                               source_raw_path, journal_id));
    }
    return {};
}

[[nodiscard]] std::filesystem::path parent_directory(const std::string& raw_path) {
    auto parent = std::filesystem::path{raw_path}.parent_path();
    return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] core::Result<void> fsync_parent(const std::string& raw_path,
                                              const std::string& source_raw_path,
                                              const core::StableId& journal_id) {
    const auto directory = parent_directory(raw_path);
    Descriptor descriptor{
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor.valid() || ::fsync(descriptor.get()) != 0) {
        return std::unexpected(system_error("syncing metadata source directory failed", errno,
                                            source_raw_path, journal_id));
    }
    return {};
}

[[nodiscard]] std::pair<std::string, std::string> sibling_paths(const std::string& source_raw_path,
                                                                const core::StableId& journal_id) {
    const auto parent = std::filesystem::path{source_raw_path}.parent_path();
    const auto stem = ".trackknife-" + journal_id.to_string() + ".metadata-";
    return {(parent / (stem + "prepared")).native(), (parent / (stem + "backup")).native()};
}

[[nodiscard]] bool expected_sibling_paths(const MetadataOperationJournalRecord& record) {
    const auto [prepared, backup] = sibling_paths(record.source_raw_path, record.id);
    return prepared == record.prepared_raw_path && backup == record.backup_raw_path;
}

[[nodiscard]] std::map<std::string, std::vector<std::string>>
effective_text(const metadata::MetadataDocument& document) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& field : document.effective_fields()) {
        result.emplace(field.canonical_name, field.values);
    }
    return result;
}

[[nodiscard]] std::map<std::string, std::vector<std::string>>
effective_native_text(const metadata::MetadataDocument& document) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& field : document.effective_native_fields()) {
        result.emplace(metadata::canonicalize_native_field_name(field.native_name), field.values);
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::string>>
addressed_values(const std::map<std::string, std::vector<std::string>>& fields,
                 const std::map<std::string, std::vector<std::string>>& native_fields,
                 const std::string& canonical_name,
                 const std::optional<std::string>& exact_native_name) {
    const auto& addressed_fields = exact_native_name ? native_fields : fields;
    const auto& addressed_name = exact_native_name ? *exact_native_name : canonical_name;
    const auto found = addressed_fields.find(addressed_name);
    return found == addressed_fields.end() ? std::nullopt
                                           : std::optional<std::vector<std::string>>{found->second};
}

[[nodiscard]] bool planned_fields_match(const metadata::MetadataDocument& document,
                                        const MetadataOperationJournalRecord& record) {
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : record.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        if (change.kind == metadata::StagedMetadataPatchKind::remove_field) {
            if (values) {
                return false;
            }
        } else if (!values || *values != change.planned_values) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::Result<void>
verify_plan_originals(const metadata::MetadataDocument& document,
                      const metadata::MetadataWritePlanSource& source_plan) {
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : source_plan.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        const bool present = values.has_value();
        if (present != change.original_present || (present && *values != change.original_values)) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict,
                "fresh metadata differs from the previewed original values", source_plan.raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<MetadataOperationJournalRecord>
make_journal_record(const metadata::MetadataWritePlanSource& source_plan,
                    const core::StableId& journal_id) {
    const auto [prepared_path, backup_path] = sibling_paths(source_plan.raw_path, journal_id);
    MetadataOperationJournalRecord record{
        .id = journal_id,
        .state = State::planned,
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_path,
        .backup_raw_path = backup_path,
        .expected_revision = *source_plan.observed_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = source_plan.occurrence_indexes,
        .content_kind = MetadataOperationContentKind::text_fields,
        .changes = {},
        .artwork = std::nullopt,
        .failure = std::nullopt,
    };
    record.changes.reserve(source_plan.changes.size());
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(
                operation_error(core::ErrorCode::invalid_argument,
                                "metadata commit plan contains an unresolved physical change",
                                source_plan.raw_path, journal_id));
        }
        const auto& intent = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&intent](const auto& candidate) {
                return candidate.kind == intent.kind && candidate.values == intent.values;
            })) {
            return std::unexpected(
                operation_error(core::ErrorCode::invalid_argument,
                                "metadata commit plan contains conflicting logical intents",
                                source_plan.raw_path, journal_id));
        }
        const auto mapping_native_name = change.exact_native_name && change.native_name.empty()
                                             ? std::string_view{change.display_name}
                                             : std::string_view{change.native_name};
        auto mapping =
            metadata::map_flac_text_field(change.canonical_name, change.display_name,
                                          mapping_native_name, intent.kind, intent.values);
        if (!mapping) {
            return std::unexpected(std::move(mapping.error()));
        }
        std::vector<std::size_t> item_indexes;
        item_indexes.reserve(change.intents.size());
        for (const auto& logical_intent : change.intents) {
            item_indexes.push_back(logical_intent.item_index);
        }
        record.changes.push_back(MetadataOperationJournalChange{
            .field_index = change.field_index,
            .canonical_name = change.canonical_name,
            .property_name = std::move(mapping->property_name),
            .original_present = change.original_present,
            .original_values = change.original_values,
            .kind = intent.kind,
            .planned_values = intent.values,
            .item_indexes = std::move(item_indexes),
            .exact_native_name = change.exact_native_name,
        });
    }
    return record;
}

[[nodiscard]] metadata::ArtworkInventoryPolicy embedded_artwork_policy() {
    auto policy = metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    return policy;
}

[[nodiscard]] core::Result<metadata::LocalArtworkInventory>
read_embedded_artwork_inventory(const std::string& raw_path,
                                const core::CancellationToken& cancellation = {}) {
    return metadata::read_local_artwork_inventory(raw_path, embedded_artwork_policy(),
                                                  cancellation);
}

[[nodiscard]] std::string canonical_flac_picture_type(const metadata::ArtworkRole role) {
    switch (role) {
    case metadata::ArtworkRole::front:
        return "Front Cover";
    case metadata::ArtworkRole::back:
        return "Back Cover";
    case metadata::ArtworkRole::artist:
        return "Artist";
    case metadata::ArtworkRole::disc:
        return "Media";
    case metadata::ArtworkRole::icon:
        return "File Icon";
    case metadata::ArtworkRole::other:
        return "Other";
    }
    return "Other";
}

[[nodiscard]] core::Result<std::vector<metadata::ArtworkInventoryItem>>
project_artwork_inventory(const metadata::LocalArtworkInventory& inventory,
                          const metadata::ArtworkWritePlanSource& source_plan) {
    auto projected = inventory.items;
    if (source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::add) {
        if (source_plan.change.original || !source_plan.change.replacement ||
            source_plan.change.target_ordinal != projected.size()) {
            return std::unexpected(
                operation_error(core::ErrorCode::conflict,
                                "fresh embedded artwork differs from the previewed insertion point",
                                source_plan.raw_media_path));
        }
        const auto& replacement = *source_plan.change.replacement;
        if (std::ranges::any_of(projected, [&](const auto& item) {
                return item.content_fingerprint == replacement.content_fingerprint;
            })) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "added artwork already exists in the target",
                                                   source_plan.raw_media_path));
        }
        projected.push_back(metadata::ArtworkInventoryItem{
            .role = source_plan.change.added_role,
            .native_type = canonical_flac_picture_type(source_plan.change.added_role),
            .mime_type = replacement.mime_type,
            .description = source_plan.change.added_description,
            .width = replacement.width,
            .height = replacement.height,
            .byte_size = replacement.byte_size,
            .content_fingerprint = replacement.content_fingerprint,
            .provenance = metadata::ArtworkProvenance::embedded,
            .raw_source_path = source_plan.raw_media_path,
            .source_revision = inventory.media_revision,
            .source_ordinal = projected.size(),
            .duplicate_of = std::nullopt,
        });
        return projected;
    }
    const auto target = std::ranges::find_if(projected, [&](const auto& item) {
        return item.provenance == metadata::ArtworkProvenance::embedded &&
               item.source_ordinal == source_plan.change.target_ordinal;
    });
    if (target == projected.end() || !source_plan.change.original ||
        *target != *source_plan.change.original ||
        target->content_fingerprint != source_plan.change.expected_target_fingerprint) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict, "fresh embedded artwork differs from the previewed target",
            source_plan.raw_media_path));
    }
    if (source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::replace) {
        if (!source_plan.change.replacement) {
            return std::unexpected(
                operation_error(core::ErrorCode::invalid_argument,
                                "artwork replacement plan has no verified replacement",
                                source_plan.raw_media_path));
        }
        const auto& replacement = *source_plan.change.replacement;
        target->mime_type = replacement.mime_type;
        target->width = replacement.width;
        target->height = replacement.height;
        target->byte_size = replacement.byte_size;
        target->content_fingerprint = replacement.content_fingerprint;
        target->duplicate_of.reset();
    } else {
        projected.erase(target);
        for (std::size_t index = 0U; index < projected.size(); ++index) {
            projected[index].source_ordinal = index;
            projected[index].duplicate_of.reset();
        }
    }
    return projected;
}

[[nodiscard]] core::Result<MetadataOperationJournalRecord>
make_artwork_journal_record(const metadata::ArtworkWritePlanSource& source_plan,
                            const metadata::LocalArtworkInventory& inventory,
                            const core::StableId& journal_id) {
    auto projected = project_artwork_inventory(inventory, source_plan);
    if (!projected) {
        return std::unexpected(std::move(projected.error()));
    }
    auto original_fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory.items);
    auto planned_fingerprint = metadata::fingerprint_embedded_artwork_inventory(*projected);
    if (!original_fingerprint || !planned_fingerprint) {
        return std::unexpected(!original_fingerprint ? std::move(original_fingerprint.error())
                                                     : std::move(planned_fingerprint.error()));
    }
    const auto [prepared_path, backup_path] = sibling_paths(source_plan.raw_media_path, journal_id);
    return MetadataOperationJournalRecord{
        .id = journal_id,
        .state = State::planned,
        .source_raw_path = source_plan.raw_media_path,
        .prepared_raw_path = prepared_path,
        .backup_raw_path = backup_path,
        .expected_revision = *source_plan.observed_media_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = source_plan.occurrence_indexes,
        .content_kind = MetadataOperationContentKind::embedded_artwork,
        .changes = {},
        .artwork =
            MetadataOperationJournalArtwork{
                .kind = source_plan.change.kind,
                .target_ordinal = source_plan.change.target_ordinal,
                .original_item_count = inventory.items.size(),
                .planned_item_count = projected->size(),
                .original_target_fingerprint =
                    source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::add
                        ? std::nullopt
                        : std::optional{source_plan.change.expected_target_fingerprint},
                .replacement_fingerprint =
                    source_plan.change.replacement
                        ? std::optional{source_plan.change.replacement->content_fingerprint}
                        : std::nullopt,
                .original_inventory_fingerprint = *original_fingerprint,
                .planned_inventory_fingerprint = *planned_fingerprint,
            },
        .failure = std::nullopt,
    };
}

[[nodiscard]] core::Result<metadata::MetadataDocument>
verify_published_content(const MetadataOperationJournalRecord& record,
                         const core::LocalSourceRevision& revision,
                         const core::CancellationToken& cancellation = {}) {
    auto reread = metadata::read_local_metadata(record.source_raw_path, cancellation);
    if (!reread || reread->source_revision != revision) {
        return std::unexpected(
            !reread ? std::move(reread.error())
                    : operation_error(core::ErrorCode::conflict,
                                      "published metadata has an unexpected revision",
                                      record.source_raw_path, record.id));
    }
    if (record.content_kind == MetadataOperationContentKind::text_fields) {
        if (!planned_fields_match(reread->document, record)) {
            return std::unexpected(operation_error(core::ErrorCode::conflict,
                                                   "published metadata fields failed verification",
                                                   record.source_raw_path, record.id));
        }
        return std::move(reread->document);
    }
    if (!record.artwork) {
        return std::unexpected(operation_error(core::ErrorCode::invariant,
                                               "artwork journal evidence is missing",
                                               record.source_raw_path, record.id));
    }
    auto inventory = read_embedded_artwork_inventory(record.source_raw_path, cancellation);
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory->items);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    if (inventory->media_revision != revision ||
        inventory->items.size() != record.artwork->planned_item_count ||
        *fingerprint != record.artwork->planned_inventory_fingerprint) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "published embedded artwork failed verification",
                                               record.source_raw_path, record.id));
    }
    return std::move(reread->document);
}

[[nodiscard]] core::Result<void>
transition(MetadataOperationJournal& journal, const MetadataOperationJournalRecord& record,
           const State from, const State to,
           const std::optional<core::LocalSourceRevision>& prepared_revision,
           const std::optional<core::LocalSourceRevision>& published_revision,
           const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition(record.id, MetadataOperationJournalTransition{
                                             .expected_state = from,
                                             .state = to,
                                             .prepared_revision = prepared_revision,
                                             .published_revision = published_revision,
                                             .failure = failure,
                                         });
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
optional_revision(const std::string& raw_path) {
    auto revision = core::observe_local_source_revision(raw_path);
    if (revision) {
        return std::optional{*revision};
    }
    if (revision.error().code == core::ErrorCode::not_found) {
        return std::optional<core::LocalSourceRevision>{};
    }
    return std::unexpected(std::move(revision.error()));
}

[[nodiscard]] core::Result<void>
unlink_if_matches(const std::string& raw_path,
                  const std::optional<core::LocalSourceRevision>& expected_revision,
                  const std::string& source_raw_path, const core::StableId& journal_id,
                  const bool permit_unobserved_owned_path = false) {
    auto observed = optional_revision(raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (!*observed) {
        return {};
    }
    if ((!expected_revision || **observed != *expected_revision) && !permit_unobserved_owned_path) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "recovery path has an unexpected identity",
                                               source_raw_path, journal_id));
    }
    struct stat status{};
    if (::lstat(raw_path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        ::unlink(raw_path.c_str()) != 0) {
        return std::unexpected(system_error("removing owned recovery path failed", errno,
                                            source_raw_path, journal_id));
    }
    return fsync_parent(raw_path, source_raw_path, journal_id);
}

[[nodiscard]] core::Result<void>
rollback_published(const MetadataOperationJournalRecord& record,
                   const core::LocalSourceRevision& published_revision) {
    auto source = optional_revision(record.source_raw_path);
    auto backup = optional_revision(record.backup_raw_path);
    if (!source || !backup) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(backup.error()));
    }
    if (!*source || !*backup || **source != published_revision ||
        **backup != record.expected_revision) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            "published metadata source cannot be rolled back from the recorded identities",
            record.source_raw_path, record.id));
    }
    struct stat source_status{};
    struct stat backup_status{};
    if (::lstat(record.source_raw_path.c_str(), &source_status) != 0 ||
        ::lstat(record.backup_raw_path.c_str(), &backup_status) != 0) {
        return std::unexpected(system_error("observing metadata rollback topology failed", errno,
                                            record.source_raw_path, record.id));
    }
    if (!S_ISREG(source_status.st_mode) || !S_ISREG(backup_status.st_mode) ||
        source_status.st_nlink != 1 || backup_status.st_nlink != 1) {
        return std::unexpected(
            operation_error(core::ErrorCode::conflict,
                            "metadata rollback refuses changed symlink or hard-link topology",
                            record.source_raw_path, record.id));
    }

    bool exchanged = false;
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, AT_FDCWD, record.source_raw_path.c_str(), AT_FDCWD,
                  record.backup_raw_path.c_str(), RENAME_EXCHANGE) == 0) {
        exchanged = true;
    } else if (errno != ENOSYS && errno != EINVAL) {
        return std::unexpected(system_error("atomically restoring metadata backup failed", errno,
                                            record.source_raw_path, record.id));
    }
#endif
    if (!exchanged) {
        if (::rename(record.source_raw_path.c_str(), record.prepared_raw_path.c_str()) != 0) {
            return std::unexpected(system_error("parking failed metadata publication failed", errno,
                                                record.source_raw_path, record.id));
        }
        if (::rename(record.backup_raw_path.c_str(), record.source_raw_path.c_str()) != 0) {
            const auto restore_error = errno;
            static_cast<void>(
                ::rename(record.prepared_raw_path.c_str(), record.source_raw_path.c_str()));
            return std::unexpected(system_error("restoring metadata backup failed", restore_error,
                                                record.source_raw_path, record.id));
        }
        if (::unlink(record.prepared_raw_path.c_str()) != 0) {
            return std::unexpected(system_error("removing failed metadata publication failed",
                                                errno, record.source_raw_path, record.id));
        }
    } else if (::unlink(record.backup_raw_path.c_str()) != 0) {
        return std::unexpected(system_error("removing failed metadata publication failed", errno,
                                            record.source_raw_path, record.id));
    }
    auto synced = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    if (!synced) {
        return synced;
    }
    auto restored = core::observe_local_source_revision(record.source_raw_path);
    if (!restored || *restored != record.expected_revision) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "metadata rollback could not verify the original",
                                               record.source_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
record_terminal_failure(MetadataOperationJournal& journal,
                        const MetadataOperationJournalRecord& record, const State current_state,
                        const std::optional<core::LocalSourceRevision>& prepared_revision,
                        const std::optional<core::LocalSourceRevision>& published_revision,
                        const core::Error& failure, const bool reconciled) {
    return transition(journal, record, current_state,
                      reconciled ? State::rolled_back : State::needs_reconciliation,
                      prepared_revision, published_revision, failure);
}

[[nodiscard]] core::Result<MetadataCommitResult>
verified_commit_result(const MetadataOperationJournalRecord& record,
                       const core::LocalSourceRevision& published_revision,
                       metadata::MetadataDocument document) {
    return MetadataCommitResult{
        .journal_id = record.id,
        .source_raw_path = record.source_raw_path,
        .backup_raw_path = record.backup_raw_path,
        .previous_revision = record.expected_revision,
        .published_revision = published_revision,
        .document = std::move(document),
        .occurrence_indexes = record.occurrence_indexes,
    };
}

[[nodiscard]] bool original_fields_match(const metadata::MetadataDocument& document,
                                         const MetadataOperationJournalRecord& record) {
    const auto fields = effective_text(document);
    const auto native_fields = effective_native_text(document);
    for (const auto& change : record.changes) {
        const auto values = addressed_values(fields, native_fields, change.canonical_name,
                                             change.exact_native_name);
        if (!change.original_present) {
            if (values) {
                return false;
            }
        } else if (!values || *values != change.original_values) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::Result<metadata::MetadataDocument>
verify_original_content(const MetadataOperationJournalRecord& record,
                        const core::CancellationToken& cancellation = {}) {
    auto reread = metadata::read_local_metadata(record.source_raw_path, cancellation);
    if (!reread || reread->source_revision != record.expected_revision) {
        return std::unexpected(!reread
                                   ? std::move(reread.error())
                                   : operation_error(core::ErrorCode::conflict,
                                                     "restored metadata has an unexpected revision",
                                                     record.source_raw_path, record.id));
    }
    if (record.content_kind == MetadataOperationContentKind::text_fields) {
        if (!original_fields_match(reread->document, record)) {
            return std::unexpected(operation_error(
                core::ErrorCode::conflict, "restored metadata fields failed undo verification",
                record.source_raw_path, record.id));
        }
        return std::move(reread->document);
    }
    if (!record.artwork) {
        return std::unexpected(operation_error(core::ErrorCode::invariant,
                                               "artwork journal evidence is missing",
                                               record.source_raw_path, record.id));
    }
    auto inventory = read_embedded_artwork_inventory(record.source_raw_path, cancellation);
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto fingerprint = metadata::fingerprint_embedded_artwork_inventory(inventory->items);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    if (inventory->media_revision != record.expected_revision ||
        inventory->items.size() != record.artwork->original_item_count ||
        *fingerprint != record.artwork->original_inventory_fingerprint) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "restored embedded artwork failed undo verification",
                                               record.source_raw_path, record.id));
    }
    return std::move(reread->document);
}

[[nodiscard]] core::Result<void>
transition_backup(MetadataOperationJournal& journal, const core::StableId& journal_id,
                  const BackupState from, const BackupState to,
                  const std::optional<core::StableId>& undo_id = std::nullopt,
                  const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition_backup(
        journal_id,
        MetadataOperationBackupTransition{
            .expected_state = from, .state = to, .undo_id = undo_id, .failure = failure});
}

[[nodiscard]] core::Result<void>
verify_direct_single_link(const std::string& raw_path, const Descriptor& descriptor,
                          const core::LocalSourceRevision& expected,
                          const MetadataOperationJournalRecord& record,
                          const std::string_view description) {
    struct stat path_status{};
    struct stat descriptor_status{};
    if (::lstat(raw_path.c_str(), &path_status) != 0 ||
        ::fstat(descriptor.get(), &descriptor_status) != 0) {
        return std::unexpected(
            system_error(std::string{"observing "} + std::string{description} + " failed", errno,
                         record.source_raw_path, record.id));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        path_status.st_dev != descriptor_status.st_dev ||
        path_status.st_ino != descriptor_status.st_ino ||
        revision_from_stat(descriptor_status) != expected) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            std::string{description} + " no longer has its recorded identity and topology",
            record.source_raw_path, record.id));
    }
    return {};
}

// Swaps the source and backup directory entries without deleting either inode.
// Undo is unavailable on a filesystem that cannot provide an atomic exchange;
// a multi-rename emulation would create additional crash states.
[[nodiscard]] core::Result<void>
exchange_source_and_backup(const MetadataOperationJournalRecord& record) {
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, AT_FDCWD, record.source_raw_path.c_str(), AT_FDCWD,
                  record.backup_raw_path.c_str(), RENAME_EXCHANGE) == 0) {
        return fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    }
    const auto number = errno;
    if (number == ENOSYS || number == EINVAL || number == EOPNOTSUPP || number == EXDEV) {
        return std::unexpected(operation_error(
            core::ErrorCode::unsupported,
            "metadata undo requires atomic directory-entry exchange on this filesystem",
            record.source_raw_path, record.id));
    }
    return std::unexpected(system_error("atomically exchanging metadata backup failed", number,
                                        record.source_raw_path, record.id));
#else
    return std::unexpected(
        operation_error(core::ErrorCode::unsupported,
                        "metadata undo requires atomic directory-entry exchange on this platform",
                        record.source_raw_path, record.id));
#endif
}

[[nodiscard]] core::Result<MetadataCommitResult>
finish_metadata_undo(MetadataOperationBackupRecord backup, MetadataOperationJournal& journal,
                     const MetadataDependentStateCommitter& dependent_state_committer,
                     const core::CancellationToken& cancellation) {
    const auto& record = backup.operation;
    if (backup.state != BackupState::undoing || !backup.undo_id || !record.published_revision) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata undo has incomplete journal evidence",
                                               record.source_raw_path, record.id));
    }

    auto process_lock =
        acquire_process_lock(*record.published_revision, cancellation, record.source_raw_path);
    if (!process_lock) {
        if (process_lock.error().code == core::ErrorCode::cancelled) {
            static_cast<void>(
                transition_backup(journal, record.id, BackupState::undoing, BackupState::retained));
        }
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_revision = optional_revision(record.source_raw_path);
    auto backup_revision = optional_revision(record.backup_raw_path);
    if (!source_revision || !backup_revision) {
        auto issue = !source_revision ? std::move(source_revision.error())
                                      : std::move(backup_revision.error());
        static_cast<void>(transition_backup(journal, record.id, BackupState::undoing,
                                            BackupState::needs_reconciliation, backup.undo_id,
                                            issue));
        return std::unexpected(std::move(issue));
    }

    const bool before_exchange = *source_revision &&
                                 **source_revision == *record.published_revision &&
                                 *backup_revision && **backup_revision == record.expected_revision;
    const bool after_exchange =
        *source_revision && **source_revision == record.expected_revision &&
        ((!*backup_revision) || **backup_revision == *record.published_revision);
    if (!before_exchange && !after_exchange) {
        auto issue =
            operation_error(core::ErrorCode::conflict,
                            "metadata undo has ambiguous source or retained-backup identities",
                            record.source_raw_path, record.id);
        auto marked = transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::needs_reconciliation, backup.undo_id, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }

    auto source_descriptor =
        open_and_lock_file(record.source_raw_path, cancellation, record.source_raw_path, record.id);
    if (!source_descriptor) {
        auto issue = std::move(source_descriptor.error());
        static_cast<void>(
            issue.code == core::ErrorCode::cancelled
                ? transition_backup(journal, record.id, BackupState::undoing, BackupState::retained)
                : transition_backup(journal, record.id, BackupState::undoing,
                                    BackupState::needs_reconciliation, backup.undo_id, issue));
        return std::unexpected(std::move(issue));
    }
    const auto expected_source_revision =
        before_exchange ? *record.published_revision : record.expected_revision;
    if (auto verified =
            verify_direct_single_link(record.source_raw_path, *source_descriptor,
                                      expected_source_revision, record, "metadata undo source");
        !verified) {
        const auto& issue = verified.error();
        static_cast<void>(transition_backup(journal, record.id, BackupState::undoing,
                                            BackupState::needs_reconciliation, backup.undo_id,
                                            issue));
        return std::unexpected(issue);
    }
    std::optional<Descriptor> backup_descriptor;
    if (*backup_revision) {
        auto locked = open_and_lock_file(record.backup_raw_path, cancellation,
                                         record.source_raw_path, record.id);
        if (!locked) {
            const auto& issue = locked.error();
            static_cast<void>(issue.code == core::ErrorCode::cancelled
                                  ? transition_backup(journal, record.id, BackupState::undoing,
                                                      BackupState::retained)
                                  : transition_backup(journal, record.id, BackupState::undoing,
                                                      BackupState::needs_reconciliation,
                                                      backup.undo_id, issue));
            return std::unexpected(issue);
        }
        const auto expected_backup_revision =
            before_exchange ? record.expected_revision : *record.published_revision;
        if (auto verified =
                verify_direct_single_link(record.backup_raw_path, *locked, expected_backup_revision,
                                          record, "retained metadata backup");
            !verified) {
            const auto& issue = verified.error();
            static_cast<void>(transition_backup(journal, record.id, BackupState::undoing,
                                                BackupState::needs_reconciliation, backup.undo_id,
                                                issue));
            return std::unexpected(issue);
        }
        backup_descriptor.emplace(std::move(*locked));
    }

    bool exchanged = after_exchange;
    if (before_exchange) {
        if (auto swapped = exchange_source_and_backup(record); !swapped) {
            const auto& issue = swapped.error();
            auto current_source = optional_revision(record.source_raw_path);
            auto current_backup = optional_revision(record.backup_raw_path);
            const bool remained_unchanged = current_source && *current_source &&
                                            **current_source == *record.published_revision &&
                                            current_backup && *current_backup &&
                                            **current_backup == record.expected_revision;
            auto settled =
                remained_unchanged
                    ? transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::retained)
                    : transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::needs_reconciliation, backup.undo_id, issue);
            if (!settled) {
                return std::unexpected(settled.error());
            }
            return std::unexpected(issue);
        }
        exchanged = true;
    }

    const auto restore_publication =
        [&](const core::Error& issue) -> core::Result<MetadataCommitResult> {
        if (exchanged) {
            auto restored = exchange_source_and_backup(record);
            if (!restored) {
                const auto& restore_issue = restored.error();
                auto marked = transition_backup(journal, record.id, BackupState::undoing,
                                                BackupState::needs_reconciliation, backup.undo_id,
                                                restore_issue);
                if (!marked) {
                    return std::unexpected(marked.error());
                }
                return std::unexpected(restore_issue);
            }
        }
        auto retained =
            transition_backup(journal, record.id, BackupState::undoing, BackupState::retained);
        if (!retained) {
            return std::unexpected(retained.error());
        }
        return std::unexpected(issue);
    };

    auto reread = verify_original_content(record, cancellation);
    if (!reread) {
        return restore_publication(reread.error());
    }
    MetadataCommitResult result{
        .journal_id = *backup.undo_id,
        .source_raw_path = record.source_raw_path,
        .backup_raw_path = record.backup_raw_path,
        .previous_revision = *record.published_revision,
        .published_revision = record.expected_revision,
        .document = *reread,
        .occurrence_indexes = record.occurrence_indexes,
    };
    if (auto dependent = dependent_state_committer(result); !dependent) {
        return restore_publication(dependent.error());
    }
    auto final_revision = core::observe_local_source_revision(record.source_raw_path);
    if (!final_revision || *final_revision != record.expected_revision) {
        const auto issue = !final_revision
                               ? final_revision.error()
                               : operation_error(core::ErrorCode::conflict,
                                                 "metadata source changed during undo state commit",
                                                 record.source_raw_path, record.id);
        auto marked = transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::needs_reconciliation, backup.undo_id, issue);
        if (!marked) {
            return std::unexpected(marked.error());
        }
        return std::unexpected(issue);
    }
    auto cleaned = unlink_if_matches(record.backup_raw_path, record.published_revision,
                                     record.source_raw_path, record.id);
    if (!cleaned) {
        const auto& issue = cleaned.error();
        auto marked = transition_backup(journal, record.id, BackupState::undoing,
                                        BackupState::needs_reconciliation, backup.undo_id, issue);
        if (!marked) {
            return std::unexpected(marked.error());
        }
        return std::unexpected(issue);
    }
    auto completed = transition_backup(journal, record.id, BackupState::undoing,
                                       BackupState::undone, backup.undo_id);
    if (!completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

[[nodiscard]] core::Result<MetadataCommitResult> publish_prepared_metadata_copy(
    const MetadataOperationJournalRecord& record, const Descriptor& source_descriptor,
    const struct stat& source_status, const ExtendedAttributeListing& source_attributes,
    const core::LocalSourceRevision& initial_prepared_revision,
    const metadata::MetadataDocument& prepared_document, MetadataOperationJournal& journal,
    const MetadataDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    auto prepared_descriptor = open_and_lock_file(record.prepared_raw_path, cancellation,
                                                  record.source_raw_path, record.id);
    if (!prepared_descriptor) {
        const auto& failure = prepared_descriptor.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        const bool restored = cleaned.has_value();
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(restored ? failure : cleaned.error());
    }
    auto metadata_applied =
        apply_filesystem_metadata(source_descriptor, source_status, source_attributes,
                                  *prepared_descriptor, record.source_raw_path, record.id);
    if (metadata_applied && ::fsync(prepared_descriptor->get()) != 0) {
        metadata_applied = std::unexpected(system_error("syncing prepared metadata file failed",
                                                        errno, record.source_raw_path, record.id));
    }
    if (!metadata_applied) {
        const auto& failure = metadata_applied.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        const bool restored = cleaned.has_value();
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(restored ? failure : cleaned.error());
    }
    auto prepared_status = locked_status(*prepared_descriptor, record.source_raw_path, record.id);
    if (!prepared_status) {
        const auto& failure = prepared_status.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, initial_prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal =
            record_terminal_failure(journal, record, State::planned, initial_prepared_revision,
                                    std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    const auto prepared_revision = revision_from_stat(*prepared_status);
    auto prepared_transition = transition(journal, record, State::planned, State::prepared,
                                          prepared_revision, std::nullopt);
    if (!prepared_transition) {
        const auto& failure = prepared_transition.error();
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        if (cleaned) {
            static_cast<void>(record_terminal_failure(
                journal, record, State::planned, prepared_revision, std::nullopt, failure, true));
        } else {
            static_cast<void>(record_terminal_failure(journal, record, State::planned,
                                                      prepared_revision, std::nullopt,
                                                      cleaned.error(), false));
        }
        return std::unexpected(failure);
    }

    if (cancellation.is_cancellation_requested()) {
        const auto failure = cancelled(record.source_raw_path);
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }

    auto current_source = core::observe_local_source_revision(record.source_raw_path);
    if (!current_source || *current_source != record.expected_revision ||
        ::link(record.source_raw_path.c_str(), record.backup_raw_path.c_str()) != 0) {
        auto failure = !current_source
                           ? std::move(current_source.error())
                           : (*current_source != record.expected_revision
                                  ? operation_error(core::ErrorCode::conflict,
                                                    "metadata source changed before publication",
                                                    record.source_raw_path, record.id)
                                  : system_error("creating metadata backup failed", errno,
                                                 record.source_raw_path, record.id));
        const auto cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                               record.source_raw_path, record.id);
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    auto backup_source = core::observe_local_source_revision(record.source_raw_path);
    auto backup = core::observe_local_source_revision(record.backup_raw_path);
    if (!backup_source || !backup || *backup_source != record.expected_revision ||
        *backup != record.expected_revision) {
        const auto failure =
            !backup_source
                ? backup_source.error()
                : (!backup
                       ? backup.error()
                       : operation_error(core::ErrorCode::conflict,
                                         "metadata backup does not preserve the expected source",
                                         record.source_raw_path, record.id));
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, record.expected_revision, record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(
            restored ? failure
                     : (!backup_cleaned ? backup_cleaned.error() : prepared_cleaned.error()));
    }
    auto directory_synced = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    if (!directory_synced) {
        const auto& failure = directory_synced.error();
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, record.expected_revision, record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    if (cancellation.is_cancellation_requested()) {
        const auto failure = cancelled(record.source_raw_path);
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, record.expected_revision, record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }

    if (::rename(record.prepared_raw_path.c_str(), record.source_raw_path.c_str()) != 0) {
        const auto failure = system_error("publishing prepared metadata failed", errno,
                                          record.source_raw_path, record.id);
        const auto backup_cleaned = unlink_if_matches(
            record.backup_raw_path, record.expected_revision, record.source_raw_path, record.id);
        const auto prepared_cleaned = unlink_if_matches(record.prepared_raw_path, prepared_revision,
                                                        record.source_raw_path, record.id);
        const bool restored = backup_cleaned.has_value() && prepared_cleaned.has_value();
        auto terminal = record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                                std::nullopt, failure, restored);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto published_sync = fsync_parent(record.source_raw_path, record.source_raw_path, record.id);
    auto published = core::observe_local_source_revision(record.source_raw_path);
    const bool published_identity = published && *published == prepared_revision;
    if (!published_sync || !published_identity) {
        const auto failure =
            !published_sync
                ? published_sync.error()
                : (!published ? published.error()
                              : operation_error(core::ErrorCode::conflict,
                                                "published metadata has an unexpected revision",
                                                record.source_raw_path, record.id));
        const auto rolled_back = rollback_published(record, prepared_revision);
        auto terminal =
            record_terminal_failure(journal, record, State::prepared, prepared_revision,
                                    prepared_revision, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }

    auto published_transition = transition(journal, record, State::prepared, State::published,
                                           prepared_revision, *published);
    if (!published_transition) {
        const auto& failure = published_transition.error();
        const auto rolled_back = rollback_published(record, *published);
        static_cast<void>(record_terminal_failure(journal, record, State::prepared,
                                                  prepared_revision, *published, failure,
                                                  rolled_back.has_value()));
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }

    auto reread = verify_published_content(record, *published, cancellation);
    if (!reread || *reread != prepared_document) {
        const auto failure =
            !reread ? reread.error()
                    : operation_error(core::ErrorCode::conflict,
                                      "published metadata document failed reread verification",
                                      record.source_raw_path, record.id);
        const auto rolled_back = rollback_published(record, *published);
        auto terminal =
            record_terminal_failure(journal, record, State::published, prepared_revision,
                                    *published, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    auto result = verified_commit_result(record, *published, std::move(*reread));
    auto dependent = dependent_state_committer(*result);
    if (!dependent) {
        const auto& failure = dependent.error();
        const auto rolled_back = rollback_published(record, *published);
        auto terminal =
            record_terminal_failure(journal, record, State::published, prepared_revision,
                                    *published, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    auto final_revision = core::observe_local_source_revision(record.source_raw_path);
    if (!final_revision || *final_revision != *published) {
        const auto failure =
            !final_revision
                ? final_revision.error()
                : operation_error(core::ErrorCode::conflict,
                                  "metadata source changed during dependent-state commit",
                                  record.source_raw_path, record.id);
        auto terminal = record_terminal_failure(journal, record, State::published,
                                                prepared_revision, *published, failure, false);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto completed = transition(journal, record, State::published, State::complete,
                                prepared_revision, *published);
    if (!completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

} // namespace

core::Result<MetadataCommitResult>
commit_flac_metadata_source(const metadata::MetadataWritePlanSource& source_plan,
                            MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path));
    }
    if (!dependent_state_committer || source_plan.raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos || !source_plan.ready() ||
        !metadata::is_qualified_text_adapter(source_plan.adapter_name) ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            operation_error(core::ErrorCode::invalid_argument,
                            "metadata commit requires a ready native-FLAC plan and state committer",
                            source_plan.raw_path));
    }

    auto process_lock =
        acquire_process_lock(*source_plan.observed_revision, cancellation, source_plan.raw_path);
    if (!process_lock) {
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_descriptor =
        open_and_lock_file(source_plan.raw_path, cancellation, source_plan.raw_path);
    if (!source_descriptor) {
        return std::unexpected(std::move(source_descriptor.error()));
    }
    auto source_status = require_direct_single_link_source(*source_descriptor, source_plan.raw_path,
                                                           *source_plan.observed_revision);
    if (!source_status) {
        return std::unexpected(std::move(source_status.error()));
    }
    auto source_attributes = read_extended_attributes(*source_descriptor, source_plan.raw_path);
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    auto fresh = metadata::read_local_metadata(source_plan.raw_path, cancellation);
    if (!fresh) {
        return std::unexpected(std::move(fresh.error()));
    }
    if (fresh->source_revision != *source_plan.observed_revision ||
        fresh->adapter_name != source_plan.adapter_name) {
        return std::unexpected(operation_error(
            core::ErrorCode::conflict,
            "metadata source changed before its commit journal was created", source_plan.raw_path));
    }
    auto originals = verify_plan_originals(fresh->document, source_plan);
    if (!originals) {
        return std::unexpected(std::move(originals.error()));
    }

    const auto journal_id = core::StableId::random();
    auto record = make_journal_record(source_plan, journal_id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    auto created = journal.create(*record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }

    auto prepared = metadata::prepare_qualified_metadata_write_copy(
        source_plan, record->prepared_raw_path, cancellation);
    if (!prepared) {
        const auto& failure = prepared.error();
        auto terminal = record_terminal_failure(journal, *record, State::planned, std::nullopt,
                                                std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    return publish_prepared_metadata_copy(*record, *source_descriptor, *source_status,
                                          *source_attributes, prepared->prepared_revision,
                                          prepared->document, journal, dependent_state_committer,
                                          cancellation);
}

core::Result<MetadataCommitResult>
commit_flac_artwork_source(const metadata::ArtworkWritePlanSource& source_plan,
                           MetadataOperationJournal& journal,
                           const MetadataDependentStateCommitter& dependent_state_committer,
                           const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_media_path));
    }
    if (!dependent_state_committer || source_plan.raw_media_path.empty() ||
        source_plan.raw_media_path.find('\0') != std::string::npos || !source_plan.ready() ||
        source_plan.adapter_name != "taglib-flac-picture-v1" ||
        !source_plan.expected_media_revision || !source_plan.observed_media_revision ||
        *source_plan.expected_media_revision != *source_plan.observed_media_revision ||
        (source_plan.change.kind != metadata::ArtworkWritePlanIntentKind::add &&
         !source_plan.change.original) ||
        ((source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::replace ||
          source_plan.change.kind == metadata::ArtworkWritePlanIntentKind::add) &&
         !source_plan.change.replacement)) {
        return std::unexpected(
            operation_error(core::ErrorCode::invalid_argument,
                            "artwork commit requires a ready native-FLAC plan and state committer",
                            source_plan.raw_media_path));
    }

    auto process_lock = acquire_process_lock(*source_plan.observed_media_revision, cancellation,
                                             source_plan.raw_media_path);
    if (!process_lock) {
        return std::unexpected(std::move(process_lock.error()));
    }
    auto source_descriptor =
        open_and_lock_file(source_plan.raw_media_path, cancellation, source_plan.raw_media_path);
    if (!source_descriptor) {
        return std::unexpected(std::move(source_descriptor.error()));
    }
    auto source_status = require_direct_single_link_source(
        *source_descriptor, source_plan.raw_media_path, *source_plan.observed_media_revision);
    if (!source_status) {
        return std::unexpected(std::move(source_status.error()));
    }
    auto source_attributes =
        read_extended_attributes(*source_descriptor, source_plan.raw_media_path);
    if (!source_attributes) {
        return std::unexpected(std::move(source_attributes.error()));
    }
    auto fresh_document = metadata::read_local_metadata(source_plan.raw_media_path, cancellation);
    auto fresh_inventory =
        read_embedded_artwork_inventory(source_plan.raw_media_path, cancellation);
    if (!fresh_document || !fresh_inventory) {
        return std::unexpected(!fresh_document ? std::move(fresh_document.error())
                                               : std::move(fresh_inventory.error()));
    }
    if (fresh_document->source_revision != *source_plan.observed_media_revision ||
        fresh_document->adapter_name != "taglib-flac-v1" ||
        fresh_inventory->media_revision != *source_plan.observed_media_revision ||
        fresh_inventory->embedded_adapter_name != "taglib-flac-picture-v1") {
        return std::unexpected(
            operation_error(core::ErrorCode::conflict,
                            "artwork source changed before its commit journal was created",
                            source_plan.raw_media_path));
    }

    const auto journal_id = core::StableId::random();
    auto record = make_artwork_journal_record(source_plan, *fresh_inventory, journal_id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    auto created = journal.create(*record);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }

    auto prepared = metadata::prepare_flac_artwork_write_copy(
        source_plan, record->prepared_raw_path, cancellation);
    if (!prepared) {
        const auto& failure = prepared.error();
        auto terminal = record_terminal_failure(journal, *record, State::planned, std::nullopt,
                                                std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    }
    auto prepared_inventory_fingerprint =
        metadata::fingerprint_embedded_artwork_inventory(prepared->inventory.items);
    if (!prepared_inventory_fingerprint ||
        prepared->inventory.items.size() != record->artwork->planned_item_count ||
        *prepared_inventory_fingerprint != record->artwork->planned_inventory_fingerprint) {
        const auto failure =
            !prepared_inventory_fingerprint
                ? prepared_inventory_fingerprint.error()
                : operation_error(core::ErrorCode::conflict,
                                  "prepared embedded artwork differs from journal evidence",
                                  source_plan.raw_media_path, record->id);
        const auto cleaned =
            unlink_if_matches(record->prepared_raw_path, prepared->prepared_revision,
                              record->source_raw_path, record->id);
        auto terminal =
            record_terminal_failure(journal, *record, State::planned, prepared->prepared_revision,
                                    std::nullopt, failure, cleaned.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(cleaned ? failure : cleaned.error());
    }
    return publish_prepared_metadata_copy(*record, *source_descriptor, *source_status,
                                          *source_attributes, prepared->prepared_revision,
                                          prepared->document, journal, dependent_state_committer,
                                          cancellation);
}

core::Result<std::vector<MetadataRecoveryResult>>
recover_metadata_operations(MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation) {
    if (!dependent_state_committer) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata recovery requires a state committer", {}));
    }
    auto records = journal.load_incomplete();
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    std::vector<MetadataRecoveryResult> results;
    results.reserve(records->size());
    for (auto& record : *records) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path));
        }
        if (record.state == State::needs_reconciliation || !expected_sibling_paths(record)) {
            results.push_back(MetadataRecoveryResult{
                .journal_id = record.id,
                .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                .issue = record.failure,
            });
            continue;
        }
        auto process_lock =
            acquire_process_lock(record.expected_revision, cancellation, record.source_raw_path);
        if (!process_lock) {
            return std::unexpected(std::move(process_lock.error()));
        }
        auto source_revision = optional_revision(record.source_raw_path);
        auto prepared_revision = optional_revision(record.prepared_raw_path);
        auto backup_revision = optional_revision(record.backup_raw_path);
        if (!source_revision || !prepared_revision || !backup_revision) {
            auto issue = !source_revision     ? std::move(source_revision.error())
                         : !prepared_revision ? std::move(prepared_revision.error())
                                              : std::move(backup_revision.error());
            static_cast<void>(record_terminal_failure(journal, record, record.state,
                                                      record.prepared_revision,
                                                      record.published_revision, issue, false));
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = std::move(issue)});
            continue;
        }

        std::optional<Descriptor> recovery_source_lock;
        if (*source_revision) {
            auto locked_source = open_and_lock_file(record.source_raw_path, cancellation,
                                                    record.source_raw_path, record.id);
            if (!locked_source) {
                if (locked_source.error().code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(locked_source.error()));
                }
                const auto& issue = locked_source.error();
                auto terminal =
                    record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                            record.published_revision, issue, false);
                if (!terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                                   .issue = issue});
                continue;
            }
            auto status = locked_status(*locked_source, record.source_raw_path, record.id);
            if (!status || revision_from_stat(*status) != **source_revision) {
                const auto issue =
                    !status ? status.error()
                            : operation_error(core::ErrorCode::conflict,
                                              "metadata source changed while recovery locked it",
                                              record.source_raw_path, record.id);
                auto terminal =
                    record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                            record.published_revision, issue, false);
                if (!terminal) {
                    return std::unexpected(std::move(terminal.error()));
                }
                results.push_back({.journal_id = record.id,
                                   .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                                   .issue = issue});
                continue;
            }
            recovery_source_lock.emplace(std::move(*locked_source));
        }

        if (*source_revision && **source_revision == record.expected_revision) {
            auto prepared_cleaned = unlink_if_matches(
                record.prepared_raw_path, record.prepared_revision, record.source_raw_path,
                record.id, record.state == State::planned && !record.prepared_revision);
            auto backup_cleaned =
                unlink_if_matches(record.backup_raw_path, record.expected_revision,
                                  record.source_raw_path, record.id);
            const bool restored = prepared_cleaned.has_value() && backup_cleaned.has_value();
            const auto issue =
                restored ? operation_error(core::ErrorCode::cancelled,
                                           "interrupted metadata commit was rolled back",
                                           record.source_raw_path, record.id)
                         : (!prepared_cleaned ? prepared_cleaned.error() : backup_cleaned.error());
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, issue, restored);
            results.push_back({.journal_id = record.id,
                               .outcome = restored ? MetadataRecoveryOutcome::rolled_back
                                                   : MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = restored ? std::nullopt : std::optional{issue}});
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            continue;
        }

        const auto candidate_revision =
            record.published_revision ? record.published_revision : record.prepared_revision;
        const bool published_candidate =
            *source_revision && candidate_revision && **source_revision == *candidate_revision &&
            *backup_revision && **backup_revision == record.expected_revision;
        if (!published_candidate || record.state == State::planned) {
            const auto issue = operation_error(
                core::ErrorCode::conflict,
                "interrupted metadata operation has ambiguous filesystem identities",
                record.source_raw_path, record.id);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        record.published_revision, issue, false);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }

        auto reread = verify_published_content(record, *candidate_revision, cancellation);
        if (!reread) {
            const auto& issue = reread.error();
            const auto rolled_back = rollback_published(record, *candidate_revision);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? MetadataRecoveryOutcome::rolled_back
                                       : MetadataRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }

        auto commit_result =
            verified_commit_result(record, *candidate_revision, std::move(*reread));
        auto dependent = dependent_state_committer(*commit_result);
        if (!dependent) {
            const auto& issue = dependent.error();
            const auto rolled_back = rollback_published(record, *candidate_revision);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? MetadataRecoveryOutcome::rolled_back
                                       : MetadataRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }
        auto final_revision = core::observe_local_source_revision(record.source_raw_path);
        if (!final_revision || *final_revision != *candidate_revision) {
            const auto issue =
                !final_revision
                    ? final_revision.error()
                    : operation_error(core::ErrorCode::conflict,
                                      "metadata source changed during recovery state commit",
                                      record.source_raw_path, record.id);
            auto terminal =
                record_terminal_failure(journal, record, record.state, record.prepared_revision,
                                        *candidate_revision, issue, false);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::prepared) {
            auto published_transition =
                transition(journal, record, State::prepared, State::published,
                           record.prepared_revision, *candidate_revision);
            if (!published_transition) {
                return std::unexpected(std::move(published_transition.error()));
            }
            record.state = State::published;
            record.published_revision = candidate_revision;
        }
        auto completed = transition(journal, record, State::published, State::complete,
                                    record.prepared_revision, *candidate_revision);
        if (!completed) {
            return std::unexpected(std::move(completed.error()));
        }
        results.push_back({.journal_id = record.id,
                           .outcome = MetadataRecoveryOutcome::completed,
                           .issue = std::nullopt});
    }

    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    for (auto& backup : *backups) {
        if (backup.state == BackupState::needs_reconciliation) {
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataRecoveryOutcome::needs_reconciliation,
                               .issue = backup.failure});
            continue;
        }
        if (backup.state != BackupState::undoing) {
            continue;
        }
        auto undone =
            finish_metadata_undo(backup, journal, dependent_state_committer, cancellation);
        if (undone) {
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataRecoveryOutcome::undone,
                               .issue = std::nullopt});
            continue;
        }
        auto current = journal.load_backup(backup.operation.id);
        if (!current || !*current) {
            return std::unexpected(
                current ? operation_error(core::ErrorCode::database,
                                          "metadata undo recovery lost its journal record",
                                          backup.operation.source_raw_path, backup.operation.id)
                        : std::move(current.error()));
        }
        const auto outcome = (**current).state == BackupState::retained
                                 ? MetadataRecoveryOutcome::rolled_back
                                 : MetadataRecoveryOutcome::needs_reconciliation;
        results.push_back(
            {.journal_id = backup.operation.id, .outcome = outcome, .issue = undone.error()});
    }
    return results;
}

core::Result<MetadataCommitResult>
undo_flac_metadata_operation(const core::StableId& journal_id, MetadataOperationJournal& journal,
                             const MetadataDependentStateCommitter& dependent_state_committer,
                             const core::CancellationToken& cancellation) {
    if (journal_id.is_nil() || !dependent_state_committer) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata undo requires an operation and state "
                                               "committer",
                                               {}));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled({}));
    }
    auto loaded = journal.load_backup(journal_id);
    if (!loaded || !*loaded) {
        return std::unexpected(loaded ? operation_error(core::ErrorCode::not_found,
                                                        "metadata operation has no retained backup",
                                                        {}, journal_id)
                                      : std::move(loaded.error()));
    }
    auto backup = std::move(**loaded);
    const auto& record = backup.operation;
    if (backup.state != BackupState::retained || !record.published_revision) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "metadata backup is not available for undo",
                                               record.source_raw_path, journal_id));
    }
    auto source_revision = optional_revision(record.source_raw_path);
    auto backup_revision = optional_revision(record.backup_raw_path);
    if (!source_revision || !backup_revision || !*source_revision || !*backup_revision ||
        **source_revision != *record.published_revision ||
        **backup_revision != record.expected_revision) {
        auto issue =
            !source_revision ? std::move(source_revision.error())
            : !backup_revision
                ? std::move(backup_revision.error())
                : operation_error(core::ErrorCode::conflict,
                                  "metadata source or retained backup changed since the completed "
                                  "operation",
                                  record.source_raw_path, journal_id);
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    const auto undo_id = core::StableId::random();
    auto begun = transition_backup(journal, journal_id, BackupState::retained, BackupState::undoing,
                                   undo_id);
    if (!begun) {
        return std::unexpected(std::move(begun.error()));
    }
    backup.state = BackupState::undoing;
    backup.undo_id = undo_id;
    return finish_metadata_undo(std::move(backup), journal, dependent_state_committer,
                                cancellation);
}

core::Result<void> release_metadata_backup(const core::StableId& journal_id,
                                           MetadataOperationJournal& journal,
                                           const core::CancellationToken& cancellation) {
    if (journal_id.is_nil()) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata backup release requires an operation",
                                               {}));
    }
    auto loaded = journal.load_backup(journal_id);
    if (!loaded || !*loaded) {
        return std::unexpected(loaded ? operation_error(core::ErrorCode::not_found,
                                                        "metadata operation has no backup record",
                                                        {}, journal_id)
                                      : std::move(loaded.error()));
    }
    const auto& backup = **loaded;
    const auto& record = backup.operation;
    if (backup.state != BackupState::retained) {
        return std::unexpected(operation_error(core::ErrorCode::conflict,
                                               "metadata backup is not retained",
                                               record.source_raw_path, journal_id));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(record.source_raw_path));
    }
    auto observed = optional_revision(record.backup_raw_path);
    if (!observed) {
        auto issue = std::move(observed.error());
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    if (*observed && **observed != record.expected_revision) {
        auto issue = operation_error(core::ErrorCode::conflict,
                                     "retained metadata backup has an unexpected identity",
                                     record.source_raw_path, journal_id);
        auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                        BackupState::needs_reconciliation, std::nullopt, issue);
        return std::unexpected(marked ? issue : std::move(marked.error()));
    }
    std::optional<Descriptor> locked_backup;
    if (*observed) {
        auto locked = open_and_lock_file(record.backup_raw_path, cancellation,
                                         record.source_raw_path, journal_id);
        if (!locked) {
            auto issue = std::move(locked.error());
            if (issue.code == core::ErrorCode::cancelled) {
                return std::unexpected(std::move(issue));
            }
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
        if (auto verified =
                verify_direct_single_link(record.backup_raw_path, *locked, record.expected_revision,
                                          record, "retained metadata backup");
            !verified) {
            const auto& issue = verified.error();
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
        locked_backup.emplace(std::move(*locked));
        auto removed = unlink_if_matches(record.backup_raw_path, record.expected_revision,
                                         record.source_raw_path, journal_id);
        if (!removed) {
            const auto& issue = removed.error();
            auto marked = transition_backup(journal, journal_id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            if (!marked) {
                return std::unexpected(marked.error());
            }
            return std::unexpected(issue);
        }
    }
    return transition_backup(journal, journal_id, BackupState::retained, BackupState::released);
}

core::Result<std::vector<MetadataBackupMaintenanceResult>> maintain_metadata_backups(
    MetadataOperationJournal& journal, const MetadataBackupRetentionPolicy& policy,
    const std::int64_t now_unix_seconds, const core::CancellationToken& cancellation) {
    if (policy.maximum_age_seconds < 0 || now_unix_seconds <= 0) {
        return std::unexpected(operation_error(core::ErrorCode::invalid_argument,
                                               "metadata backup retention policy is invalid", {}));
    }
    auto backups = journal.load_backups();
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    // load_backups() is newest-first. Validate the current source against the
    // newest completed operation for each exact raw path once; only then is it
    // safe to release older backups for that path.
    std::unordered_map<std::string, bool> source_is_unambiguous;
    source_is_unambiguous.reserve(backups->size());
    std::size_t retained_count = 0U;
    std::uint64_t retained_bytes = 0U;
    std::vector<MetadataBackupMaintenanceResult> results;
    results.reserve(backups->size());
    for (const auto& backup : *backups) {
        const auto [source_state, is_newest] =
            source_is_unambiguous.emplace(backup.operation.source_raw_path, false);
        if (is_newest) {
            std::optional<core::LocalSourceRevision> expected_source;
            if (backup.state == BackupState::retained || backup.state == BackupState::released) {
                expected_source = backup.operation.published_revision;
            } else if (backup.state == BackupState::undone) {
                expected_source = backup.operation.expected_revision;
            }
            if (expected_source) {
                auto locked =
                    open_and_lock_file(backup.operation.source_raw_path, cancellation,
                                       backup.operation.source_raw_path, backup.operation.id);
                if (locked && verify_direct_single_link(backup.operation.source_raw_path, *locked,
                                                        *expected_source, backup.operation,
                                                        "published metadata source")) {
                    source_state->second = true;
                }
            }
        }
        if (backup.state == BackupState::needs_reconciliation) {
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                               .issue = backup.failure});
            continue;
        }
        if (backup.state != BackupState::retained) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(backup.operation.source_raw_path));
        }
        if (!source_state->second) {
            auto issue = operation_error(
                core::ErrorCode::conflict,
                "published metadata source changed while an undo backup is retained",
                backup.operation.source_raw_path, backup.operation.id);
            auto marked = transition_backup(journal, backup.operation.id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                : std::optional<core::Error>{std::move(marked.error())},
            });
            continue;
        }
        auto retained_revision = optional_revision(backup.operation.backup_raw_path);
        if (!retained_revision) {
            auto issue = std::move(retained_revision.error());
            auto marked = transition_backup(journal, backup.operation.id, BackupState::retained,
                                            BackupState::needs_reconciliation, std::nullopt, issue);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                : std::optional<core::Error>{std::move(marked.error())},
            });
            continue;
        }
        if (!*retained_revision || **retained_revision != backup.operation.expected_revision) {
            auto released = release_metadata_backup(backup.operation.id, journal, cancellation);
            results.push_back({
                .journal_id = backup.operation.id,
                .outcome = released ? MetadataBackupMaintenanceOutcome::released
                                    : MetadataBackupMaintenanceOutcome::needs_reconciliation,
                .issue = released ? std::nullopt
                                  : std::optional<core::Error>{std::move(released.error())},
            });
            continue;
        }
        const bool expired =
            now_unix_seconds > backup.completed_at_unix_seconds &&
            now_unix_seconds - backup.completed_at_unix_seconds > policy.maximum_age_seconds;
        const auto bytes = backup.operation.expected_revision.size;
        const bool count_available = retained_count < policy.maximum_entries;
        const bool bytes_available =
            bytes <=
            policy.maximum_total_bytes - std::min(retained_bytes, policy.maximum_total_bytes);
        const bool keep = is_newest && expired == false && count_available && bytes_available;
        if (keep) {
            auto locked = open_and_lock_file(backup.operation.backup_raw_path, cancellation,
                                             backup.operation.source_raw_path, backup.operation.id);
            if (!locked) {
                auto issue = std::move(locked.error());
                if (issue.code == core::ErrorCode::cancelled) {
                    return std::unexpected(std::move(issue));
                }
                auto marked =
                    transition_backup(journal, backup.operation.id, BackupState::retained,
                                      BackupState::needs_reconciliation, std::nullopt, issue);
                results.push_back({
                    .journal_id = backup.operation.id,
                    .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                    .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                    : std::optional<core::Error>{std::move(marked.error())},
                });
                continue;
            }
            if (auto verified = verify_direct_single_link(
                    backup.operation.backup_raw_path, *locked, backup.operation.expected_revision,
                    backup.operation, "retained metadata backup");
                !verified) {
                auto issue = std::move(verified.error());
                auto marked =
                    transition_backup(journal, backup.operation.id, BackupState::retained,
                                      BackupState::needs_reconciliation, std::nullopt, issue);
                results.push_back({
                    .journal_id = backup.operation.id,
                    .outcome = MetadataBackupMaintenanceOutcome::needs_reconciliation,
                    .issue = marked ? std::optional<core::Error>{std::move(issue)}
                                    : std::optional<core::Error>{std::move(marked.error())},
                });
                continue;
            }
            ++retained_count;
            retained_bytes += bytes;
            results.push_back({.journal_id = backup.operation.id,
                               .outcome = MetadataBackupMaintenanceOutcome::retained,
                               .issue = std::nullopt});
            continue;
        }
        auto released = release_metadata_backup(backup.operation.id, journal, cancellation);
        results.push_back({
            .journal_id = backup.operation.id,
            .outcome = released ? MetadataBackupMaintenanceOutcome::released
                                : MetadataBackupMaintenanceOutcome::needs_reconciliation,
            .issue =
                released ? std::nullopt : std::optional<core::Error>{std::move(released.error())},
        });
    }
    return results;
}

} // namespace trackknife::operations
