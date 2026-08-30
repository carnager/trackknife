// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/file_publication.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

namespace trackknife::operations {
namespace {

using State = FilePublicationJournalState;
constexpr auto lock_retry_interval = std::chrono::milliseconds{10};

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

  private:
    [[nodiscard]] bool close() noexcept {
        if (!valid()) {
            return true;
        }
        return ::close(std::exchange(value_, -1)) == 0;
    }

    int value_{-1};
};

struct DirectoryWalk {
    Descriptor descriptor;
    std::vector<std::string> missing_raw_paths;
};

struct LockedSource {
    Descriptor parent;
    Descriptor source;
    core::LocalSourceRevision revision;
};

[[nodiscard]] core::Error
publication_error(const core::ErrorCode code, std::string message,
                  const std::string& source_raw_path, const std::string& target_raw_path = {},
                  const std::optional<core::StableId>& journal_id = std::nullopt) {
    core::Error result{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "source", .value = core::escape_raw_path(source_raw_path)}},
    };
    if (!target_raw_path.empty()) {
        result.context.push_back(
            {.key = "target", .value = core::escape_raw_path(target_raw_path)});
    }
    if (journal_id) {
        result.context.push_back({.key = "journal_id", .value = journal_id->to_string()});
    }
    return result;
}

[[nodiscard]] core::Error
system_error(const std::string_view operation, const int number, const std::string& source_raw_path,
             const std::string& target_raw_path = {},
             const std::optional<core::StableId>& journal_id = std::nullopt) {
    return publication_error(core::ErrorCode::io,
                             std::string{operation} + ": " +
                                 std::error_code{number, std::generic_category()}.message(),
                             source_raw_path, target_raw_path, journal_id);
}

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& target_raw_path = {}) {
    return publication_error(core::ErrorCode::cancelled,
                             "File publication was cancelled before atomic rename", source_raw_path,
                             target_raw_path);
}

[[nodiscard]] core::Error journal_failure(const core::Error& failure) {
    return {.code = failure.code, .message = failure.message, .context = {}};
}

[[nodiscard]] bool normal_absolute_path(const std::string_view raw_path) {
    if (raw_path.empty() || raw_path.find('\0') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && path == path.lexically_normal();
}

[[nodiscard]] core::LocalSourceRevision revision_from_stat(const struct stat& status) {
    return {
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .modification_time_seconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
        .modification_time_nanoseconds = static_cast<std::int64_t>(status.st_mtim.tv_nsec),
    };
}

[[nodiscard]] core::Result<DirectoryWalk>
walk_directory(const std::string& raw_path, const bool allow_missing,
               const std::string& source_raw_path, const std::string& target_raw_path,
               const std::optional<core::StableId>& journal_id = std::nullopt) {
    if (!normal_absolute_path(raw_path)) {
        return std::unexpected(
            publication_error(core::ErrorCode::invalid_argument,
                              "File publication requires normalized absolute directory paths",
                              source_raw_path, target_raw_path, journal_id));
    }
    DirectoryWalk result{
        .descriptor = Descriptor{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)},
        .missing_raw_paths = {},
    };
    if (!result.descriptor.valid()) {
        return std::unexpected(system_error("Opening the filesystem root failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    const std::filesystem::path path{raw_path};
    auto current = path.root_path();
    std::vector<std::string> components;
    for (const auto& component : path.relative_path()) {
        components.push_back(component.native());
    }
    for (std::size_t index = 0U; index < components.size(); ++index) {
        Descriptor next{::openat(result.descriptor.get(), components[index].c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (next.valid()) {
            result.descriptor = std::move(next);
            current /= components[index];
            continue;
        }
        const auto number = errno;
        if (number == ENOENT && allow_missing) {
            for (; index < components.size(); ++index) {
                current /= components[index];
                result.missing_raw_paths.push_back(current.native());
            }
            return result;
        }
        return std::unexpected(
            system_error(number == ELOOP ? "Refusing a symbolic-link directory component"
                                         : "Opening a publication directory component failed",
                         number, source_raw_path, target_raw_path, journal_id));
    }
    return result;
}

[[nodiscard]] core::Result<DirectoryWalk>
create_planned_directories(const FilePublicationJournalRecord& record) {
    const auto parent = std::filesystem::path{record.target_raw_path}.parent_path();
    DirectoryWalk result{
        .descriptor = Descriptor{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)},
        .missing_raw_paths = {},
    };
    if (!result.descriptor.valid()) {
        return std::unexpected(system_error("Opening the filesystem root failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    auto current = parent.root_path();
    std::size_t missing_index = 0U;
    for (const auto& component_path : parent.relative_path()) {
        const auto& component = component_path.native();
        current /= component;
        Descriptor next{::openat(result.descriptor.get(), component.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (next.valid()) {
            if (missing_index < record.planned_missing_directory_raw_paths.size() &&
                current.native() == record.planned_missing_directory_raw_paths[missing_index]) {
                return std::unexpected(publication_error(
                    core::ErrorCode::conflict,
                    "A planned missing target directory appeared before publication",
                    record.source_raw_path, record.target_raw_path, record.id));
            }
            result.descriptor = std::move(next);
            continue;
        }
        const auto number = errno;
        if (number != ENOENT ||
            missing_index == record.planned_missing_directory_raw_paths.size() ||
            current.native() != record.planned_missing_directory_raw_paths[missing_index]) {
            return std::unexpected(system_error(
                number == ELOOP ? "Refusing a symbolic-link target directory"
                                : "Target directory topology changed before publication",
                number, record.source_raw_path, record.target_raw_path, record.id));
        }
        if (::mkdirat(result.descriptor.get(), component.c_str(), 0777) != 0) {
            return std::unexpected(system_error("Creating a planned target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        if (::fsync(result.descriptor.get()) != 0) {
            return std::unexpected(system_error("Syncing a created target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        next = Descriptor{::openat(result.descriptor.get(), component.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
        if (!next.valid()) {
            return std::unexpected(system_error("Opening a created target directory failed", errno,
                                                record.source_raw_path, record.target_raw_path,
                                                record.id));
        }
        result.descriptor = std::move(next);
        ++missing_index;
    }
    if (missing_index != record.planned_missing_directory_raw_paths.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "The planned target-directory chain no longer reaches the target parent",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return result;
}

[[nodiscard]] core::Result<void>
lock_descriptor(const Descriptor& descriptor, const core::CancellationToken& cancellation,
                const std::string& source_raw_path, const std::string& target_raw_path,
                const std::optional<core::StableId>& journal_id = std::nullopt) {
    while (::flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
        const auto number = errno;
        if (number != EWOULDBLOCK && number != EAGAIN && number != EINTR) {
            return std::unexpected(system_error("Locking the publication source failed", number,
                                                source_raw_path, target_raw_path, journal_id));
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path, target_raw_path));
        }
        std::this_thread::sleep_for(lock_retry_interval);
    }
    return {};
}

[[nodiscard]] core::Result<LockedSource>
open_locked_source(const std::string& raw_path, const core::LocalSourceRevision& expected_revision,
                   const core::CancellationToken& cancellation, const std::string& target_raw_path,
                   const std::optional<core::StableId>& journal_id = std::nullopt) {
    const std::filesystem::path path{raw_path};
    auto parent =
        walk_directory(path.parent_path().native(), false, raw_path, target_raw_path, journal_id);
    if (!parent) {
        return std::unexpected(std::move(parent.error()));
    }
    Descriptor source{::openat(parent->descriptor.get(), path.filename().c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!source.valid()) {
        return std::unexpected(system_error("Opening the publication source failed", errno,
                                            raw_path, target_raw_path, journal_id));
    }
    if (auto locked = lock_descriptor(source, cancellation, raw_path, target_raw_path, journal_id);
        !locked) {
        return std::unexpected(std::move(locked.error()));
    }
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(source.get(), &descriptor_status) != 0 ||
        ::fstatat(parent->descriptor.get(), path.filename().c_str(), &path_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(system_error("Observing the locked publication source failed", errno,
                                            raw_path, target_raw_path, journal_id));
    }
    if (!S_ISREG(descriptor_status.st_mode) || descriptor_status.st_size < 0 ||
        !S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino) {
        return std::unexpected(publication_error(
            core::ErrorCode::unsupported,
            "File publication requires one direct regular source path with one hard link", raw_path,
            target_raw_path, journal_id));
    }
    const auto revision = revision_from_stat(descriptor_status);
    if (revision != expected_revision) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Publication source changed after filesystem preflight",
            raw_path, target_raw_path, journal_id));
    }
    return LockedSource{
        .parent = std::move(parent->descriptor), .source = std::move(source), .revision = revision};
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
observe_direct_revision(const std::string& raw_path, const std::string& source_raw_path,
                        const std::string& target_raw_path,
                        const std::optional<core::StableId>& journal_id = std::nullopt) {
    const std::filesystem::path path{raw_path};
    auto parent = walk_directory(path.parent_path().native(), true, source_raw_path,
                                 target_raw_path, journal_id);
    if (!parent) {
        return std::unexpected(std::move(parent.error()));
    }
    if (!parent->missing_raw_paths.empty()) {
        return std::optional<core::LocalSourceRevision>{};
    }
    Descriptor descriptor{::openat(parent->descriptor.get(), path.filename().c_str(),
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!descriptor.valid()) {
        if (errno == ENOENT) {
            return std::optional<core::LocalSourceRevision>{};
        }
        return std::unexpected(system_error("Opening a publication path failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(descriptor.get(), &descriptor_status) != 0 ||
        ::fstatat(parent->descriptor.get(), path.filename().c_str(), &path_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(system_error("Observing a publication path failed", errno,
                                            source_raw_path, target_raw_path, journal_id));
    }
    if (!S_ISREG(descriptor_status.st_mode) || descriptor_status.st_size < 0 ||
        !S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "A publication path has changed identity or hard-link topology",
                              source_raw_path, target_raw_path, journal_id));
    }
    return std::optional{revision_from_stat(descriptor_status)};
}

[[nodiscard]] core::Result<void> require_target_absent(const Descriptor& target_parent,
                                                       const std::string& target_name,
                                                       const FilePublicationJournalRecord& record) {
    struct stat status{};
    if (::fstatat(target_parent.get(), target_name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        const auto same_source = S_ISREG(status.st_mode) &&
                                 revision_from_stat(status) == record.expected_source_revision;
        return std::unexpected(publication_error(
            same_source ? core::ErrorCode::unsupported : core::ErrorCode::conflict,
            same_source ? "Case-only aliases are not yet safe for crash-recoverable atomic rename"
                        : "Publication target appeared after filesystem preflight",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    if (errno != ENOENT) {
        return std::unexpected(system_error("Observing the publication target failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
require_locked_source_entry(const LockedSource& source, const std::string& source_name,
                            const FilePublicationJournalRecord& record) {
    struct stat descriptor_status{};
    struct stat path_status{};
    if (::fstat(source.source.get(), &descriptor_status) != 0 ||
        ::fstatat(source.parent.get(), source_name.c_str(), &path_status, AT_SYMLINK_NOFOLLOW) !=
            0) {
        return std::unexpected(system_error("Rechecking the locked publication source failed",
                                            errno, record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    if (!S_ISREG(path_status.st_mode) || path_status.st_nlink != 1 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino ||
        revision_from_stat(descriptor_status) != record.expected_source_revision ||
        revision_from_stat(path_status) != record.expected_source_revision) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Locked source path changed immediately before atomic rename",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void> rename_no_replace(const Descriptor& source_parent,
                                                   const std::string& source_name,
                                                   const Descriptor& target_parent,
                                                   const std::string& target_name,
                                                   const FilePublicationJournalRecord& record) {
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, source_parent.get(), source_name.c_str(), target_parent.get(),
                  target_name.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    const auto number = errno;
    if (number == ENOSYS || number == EINVAL || number == EOPNOTSUPP) {
        return std::unexpected(
            publication_error(core::ErrorCode::unsupported,
                              "This filesystem cannot atomically rename without replacing a target",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return std::unexpected(system_error("Atomically publishing the renamed file failed", number,
                                        record.source_raw_path, record.target_raw_path, record.id));
#else
    static_cast<void>(source_parent);
    static_cast<void>(source_name);
    static_cast<void>(target_parent);
    static_cast<void>(target_name);
    return std::unexpected(
        publication_error(core::ErrorCode::unsupported,
                          "This platform cannot atomically rename without replacing a target",
                          record.source_raw_path, record.target_raw_path, record.id));
#endif
}

[[nodiscard]] core::Result<void>
verify_published_topology(const FilePublicationJournalRecord& record,
                          const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !target) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (*source || !*target || **target != target_revision) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Published rename no longer has the recorded source/target identity",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
sync_publication_directories(const Descriptor& source_parent, const Descriptor& target_parent,
                             const FilePublicationJournalRecord& record) {
    if (::fsync(source_parent.get()) != 0 || ::fsync(target_parent.get()) != 0) {
        return std::unexpected(system_error("Syncing renamed-file directories failed", errno,
                                            record.source_raw_path, record.target_raw_path,
                                            record.id));
    }
    return {};
}

[[nodiscard]] core::Result<void>
transition(FilePublicationJournal& journal, const FilePublicationJournalRecord& record,
           const State from, const State to,
           const std::optional<core::LocalSourceRevision>& target_revision,
           const std::optional<core::Error>& failure = std::nullopt) {
    return journal.transition(record.id,
                              FilePublicationJournalTransition{.expected_state = from,
                                                               .state = to,
                                                               .prepared_revision = std::nullopt,
                                                               .target_revision = target_revision,
                                                               .failure = failure});
}

[[nodiscard]] core::Result<void>
terminal_transition(FilePublicationJournal& journal, const FilePublicationJournalRecord& record,
                    const State from,
                    const std::optional<core::LocalSourceRevision>& target_revision,
                    const core::Error& failure, const bool rolled_back) {
    return transition(journal, record, from,
                      rolled_back ? State::rolled_back : State::needs_reconciliation,
                      target_revision, journal_failure(failure));
}

[[nodiscard]] core::Result<void> rollback_rename(const FilePublicationJournalRecord& record,
                                                 const core::LocalSourceRevision& target_revision) {
    auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                          record.target_raw_path, record.id);
    if (!source || !target) {
        return std::unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (*source || !*target || **target != target_revision) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Published rename cannot be rolled back from the recorded identities",
                              record.source_raw_path, record.target_raw_path, record.id));
    }
    const auto source_path = std::filesystem::path{record.source_raw_path};
    const auto target_path = std::filesystem::path{record.target_raw_path};
    auto source_parent = walk_directory(source_path.parent_path().native(), false,
                                        record.source_raw_path, record.target_raw_path, record.id);
    auto target_parent = walk_directory(target_path.parent_path().native(), false,
                                        record.source_raw_path, record.target_raw_path, record.id);
    if (!source_parent || !target_parent) {
        return std::unexpected(!source_parent ? std::move(source_parent.error())
                                              : std::move(target_parent.error()));
    }
    FilePublicationJournalRecord reverse = record;
    reverse.source_raw_path = record.target_raw_path;
    reverse.target_raw_path = record.source_raw_path;
    if (auto renamed =
            rename_no_replace(target_parent->descriptor, target_path.filename().native(),
                              source_parent->descriptor, source_path.filename().native(), reverse);
        !renamed) {
        return renamed;
    }
    if (auto synced = sync_publication_directories(source_parent->descriptor,
                                                   target_parent->descriptor, record);
        !synced) {
        return synced;
    }
    auto restored = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                            record.target_raw_path, record.id);
    auto removed = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                           record.target_raw_path, record.id);
    if (!restored || !removed) {
        return std::unexpected(!restored ? std::move(restored.error())
                                         : std::move(removed.error()));
    }
    if (!*restored || **restored != record.expected_source_revision || *removed) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict, "Rename rollback could not verify the original path",
            record.source_raw_path, record.target_raw_path, record.id));
    }
    return {};
}

[[nodiscard]] FilePublicationCommitResult
commit_result(const FilePublicationJournalRecord& record,
              const core::LocalSourceRevision& target_revision) {
    return {
        .journal_id = record.id,
        .source_raw_path = record.source_raw_path,
        .target_raw_path = record.target_raw_path,
        .source_revision = record.expected_source_revision,
        .target_revision = target_revision,
        .occurrence_indexes = record.occurrence_indexes,
    };
}

[[nodiscard]] core::Result<void> mark_reconciliation(FilePublicationJournal& journal,
                                                     const FilePublicationJournalRecord& record,
                                                     const core::Error& issue) {
    return terminal_transition(journal, record, record.state, record.target_revision, issue, false);
}

} // namespace

core::Result<FilePublicationCommitResult> commit_same_filesystem_publication(
    const OutputPathPreflight& preflight, const std::size_t source_index,
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        const auto source = source_index < preflight.sources.size()
                                ? preflight.sources[source_index].planned.source_raw_path
                                : std::string{};
        return std::unexpected(cancelled(source));
    }
    if (!dependent_state_committer || !preflight.ready() ||
        source_index >= preflight.sources.size()) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "File publication requires a ready preflight, source, and state committer", {}));
    }
    const auto& checked = preflight.sources[source_index];
    if (checked.publication != OutputPathPublicationKind::same_filesystem_rename ||
        checked.planned.no_change || checked.observed_revision != checked.planned.source_revision ||
        checked.target_filesystem_device != checked.observed_revision.device) {
        return std::unexpected(
            publication_error(core::ErrorCode::invalid_argument,
                              "This executor requires one ready same-filesystem rename",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }

    auto source = open_locked_source(checked.planned.source_raw_path, checked.observed_revision,
                                     cancellation, checked.planned.target_raw_path);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const auto target_path = std::filesystem::path{checked.planned.target_raw_path};
    auto fresh_target =
        walk_directory(target_path.parent_path().native(), true, checked.planned.source_raw_path,
                       checked.planned.target_raw_path);
    if (!fresh_target) {
        return std::unexpected(std::move(fresh_target.error()));
    }
    if (fresh_target->missing_raw_paths != checked.missing_directory_raw_paths) {
        return std::unexpected(
            publication_error(core::ErrorCode::conflict,
                              "Target directory topology changed after filesystem preflight",
                              checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    struct stat target_parent_status{};
    if (::fstat(fresh_target->descriptor.get(), &target_parent_status) != 0) {
        return std::unexpected(system_error("Observing the target filesystem failed", errno,
                                            checked.planned.source_raw_path,
                                            checked.planned.target_raw_path));
    }
    if (static_cast<std::uint64_t>(target_parent_status.st_dev) !=
            checked.target_filesystem_device ||
        ::faccessat(source->parent.get(), ".", W_OK | X_OK, 0) != 0 ||
        ::faccessat(fresh_target->descriptor.get(), ".", W_OK | X_OK, 0) != 0) {
        return std::unexpected(publication_error(
            core::ErrorCode::conflict,
            "Publication filesystem or directory permissions changed after preflight",
            checked.planned.source_raw_path, checked.planned.target_raw_path));
    }
    if (fresh_target->missing_raw_paths.empty()) {
        FilePublicationJournalRecord provisional{
            .id = core::StableId::random(),
            .state = State::planned,
            .publication = OutputPathPublicationKind::same_filesystem_rename,
            .source_raw_path = checked.planned.source_raw_path,
            .target_raw_path = checked.planned.target_raw_path,
            .prepared_raw_path = {},
            .expected_source_revision = checked.observed_revision,
            .prepared_revision = std::nullopt,
            .target_revision = std::nullopt,
            .occurrence_indexes = checked.planned.item_indexes,
            .planned_missing_directory_raw_paths = checked.missing_directory_raw_paths,
            .failure = std::nullopt,
        };
        if (auto absent = require_target_absent(fresh_target->descriptor,
                                                target_path.filename().native(), provisional);
            !absent) {
            return std::unexpected(std::move(absent.error()));
        }
    }

    auto record =
        make_file_publication_journal_record(preflight, source_index, core::StableId::random());
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    if (auto created = journal.create(*record); !created) {
        return std::unexpected(std::move(created.error()));
    }
    const auto finish_before_publication =
        [&](const core::Error& failure) -> core::Result<FilePublicationCommitResult> {
        auto terminal =
            terminal_transition(journal, *record, State::planned, std::nullopt, failure, true);
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(failure);
    };
    if (cancellation.is_cancellation_requested()) {
        return finish_before_publication(
            cancelled(record->source_raw_path, record->target_raw_path));
    }

    auto target_parent = create_planned_directories(*record);
    if (!target_parent) {
        return finish_before_publication(target_parent.error());
    }
    struct stat created_parent_status{};
    if (::fstat(target_parent->descriptor.get(), &created_parent_status) != 0 ||
        static_cast<std::uint64_t>(created_parent_status.st_dev) !=
            checked.target_filesystem_device) {
        const auto failure = publication_error(
            core::ErrorCode::conflict, "Created target parent is not on the preflight filesystem",
            record->source_raw_path, record->target_raw_path, record->id);
        return finish_before_publication(failure);
    }
    if (auto absent = require_target_absent(target_parent->descriptor,
                                            target_path.filename().native(), *record);
        !absent) {
        return finish_before_publication(absent.error());
    }
    if (cancellation.is_cancellation_requested()) {
        return finish_before_publication(
            cancelled(record->source_raw_path, record->target_raw_path));
    }
    const auto source_path = std::filesystem::path{record->source_raw_path};
    if (auto current_source =
            require_locked_source_entry(*source, source_path.filename().native(), *record);
        !current_source) {
        return finish_before_publication(current_source.error());
    }
    if (auto renamed =
            rename_no_replace(source->parent, source_path.filename().native(),
                              target_parent->descriptor, target_path.filename().native(), *record);
        !renamed) {
        return finish_before_publication(renamed.error());
    }
    const auto target_revision = source->revision;
    auto synced = sync_publication_directories(source->parent, target_parent->descriptor, *record);
    auto verified = verify_published_topology(*record, target_revision);
    if (!synced || !verified) {
        const auto failure = !synced ? synced.error() : verified.error();
        const auto rolled_back = rollback_rename(*record, target_revision);
        auto terminal = terminal_transition(journal, *record, State::planned, std::nullopt, failure,
                                            rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    if (auto published =
            transition(journal, *record, State::planned, State::target_published, target_revision);
        !published) {
        const auto& failure = published.error();
        const auto rolled_back = rollback_rename(*record, target_revision);
        auto terminal = terminal_transition(journal, *record, State::planned, std::nullopt, failure,
                                            rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    record->state = State::target_published;
    record->target_revision = target_revision;
    auto result = commit_result(*record, target_revision);
    if (auto dependent = dependent_state_committer(result); !dependent) {
        const auto& failure = dependent.error();
        const auto rolled_back = rollback_rename(*record, target_revision);
        auto terminal = terminal_transition(journal, *record, State::target_published,
                                            target_revision, failure, rolled_back.has_value());
        if (!terminal) {
            return std::unexpected(std::move(terminal.error()));
        }
        return std::unexpected(rolled_back ? failure : rolled_back.error());
    }
    if (auto final_topology = verify_published_topology(*record, target_revision);
        !final_topology) {
        auto marked = terminal_transition(journal, *record, State::target_published,
                                          target_revision, final_topology.error(), false);
        return std::unexpected(marked ? final_topology.error() : std::move(marked.error()));
    }
    if (auto dependent = transition(journal, *record, State::target_published,
                                    State::dependent_state_committed, target_revision);
        !dependent) {
        return std::unexpected(std::move(dependent.error()));
    }
    if (auto completed = transition(journal, *record, State::dependent_state_committed,
                                    State::complete, target_revision);
        !completed) {
        return std::unexpected(std::move(completed.error()));
    }
    return result;
}

core::Result<std::vector<FilePublicationRecoveryResult>> recover_same_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation) {
    if (!dependent_state_committer) {
        return std::unexpected(publication_error(
            core::ErrorCode::invalid_argument,
            "File-publication recovery requires a dependent-state committer", {}));
    }
    auto records = journal.load_incomplete();
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    std::vector<FilePublicationRecoveryResult> results;
    results.reserve(records->size());
    for (auto& record : *records) {
        if (record.publication != OutputPathPublicationKind::same_filesystem_rename) {
            continue;
        }
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(record.source_raw_path, record.target_raw_path));
        }
        if (record.state == State::needs_reconciliation) {
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = record.failure});
            continue;
        }
        auto source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        auto target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                              record.target_raw_path, record.id);
        if (!source || !target) {
            auto issue = !source ? std::move(source.error()) : std::move(target.error());
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const bool source_matches = *source && **source == record.expected_source_revision;
        const bool target_matches = *target && **target == record.expected_source_revision;
        if (source_matches == target_matches) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Interrupted rename has ambiguous source and target identities",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const auto lock_path = source_matches ? record.source_raw_path : record.target_raw_path;
        auto locked = open_locked_source(lock_path, record.expected_source_revision, cancellation,
                                         record.target_raw_path, record.id);
        if (!locked) {
            auto issue = std::move(locked.error());
            if (issue.code == core::ErrorCode::cancelled) {
                return std::unexpected(std::move(issue));
            }
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        source = observe_direct_revision(record.source_raw_path, record.source_raw_path,
                                         record.target_raw_path, record.id);
        target = observe_direct_revision(record.target_raw_path, record.source_raw_path,
                                         record.target_raw_path, record.id);
        if (!source || !target || static_cast<bool>(*source) == static_cast<bool>(*target) ||
            (*source && **source != record.expected_source_revision) ||
            (*target && **target != record.expected_source_revision)) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Rename topology changed while startup recovery locked the file",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::planned && *source) {
            auto issue =
                publication_error(core::ErrorCode::cancelled,
                                  "Recovered a journaled rename before atomic publication",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto terminal =
                terminal_transition(journal, record, State::planned, std::nullopt, issue, true);
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::rolled_back,
                               .issue = std::nullopt});
            continue;
        }
        if (record.state == State::planned) {
            auto published = transition(journal, record, State::planned, State::target_published,
                                        record.expected_source_revision);
            if (!published) {
                return std::unexpected(std::move(published.error()));
            }
            record.state = State::target_published;
            record.target_revision = record.expected_source_revision;
        }
        if (!*target || !record.target_revision || **target != *record.target_revision) {
            auto issue =
                publication_error(core::ErrorCode::conflict,
                                  "Interrupted rename target differs from its journal identity",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        if (record.state == State::dependent_state_committed) {
            auto completed = transition(journal, record, State::dependent_state_committed,
                                        State::complete, *record.target_revision);
            if (!completed) {
                return std::unexpected(std::move(completed.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::completed,
                               .issue = std::nullopt});
            continue;
        }
        if (record.state != State::target_published) {
            auto issue =
                publication_error(core::ErrorCode::invariant,
                                  "Same-filesystem recovery encountered an invalid journal state",
                                  record.source_raw_path, record.target_raw_path, record.id);
            auto marked = mark_reconciliation(journal, record, issue);
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = issue});
            continue;
        }
        const auto recovered_result = commit_result(record, *record.target_revision);
        if (auto dependent = dependent_state_committer(recovered_result); !dependent) {
            const auto& issue = dependent.error();
            const auto rolled_back = rollback_rename(record, *record.target_revision);
            auto terminal =
                terminal_transition(journal, record, State::target_published,
                                    record.target_revision, issue, rolled_back.has_value());
            if (!terminal) {
                return std::unexpected(std::move(terminal.error()));
            }
            results.push_back({
                .journal_id = record.id,
                .outcome = rolled_back ? FilePublicationRecoveryOutcome::rolled_back
                                       : FilePublicationRecoveryOutcome::needs_reconciliation,
                .issue =
                    rolled_back ? std::nullopt : std::optional<core::Error>{rolled_back.error()},
            });
            continue;
        }
        if (auto verified = verify_published_topology(record, *record.target_revision); !verified) {
            auto marked = mark_reconciliation(journal, record, verified.error());
            if (!marked) {
                return std::unexpected(std::move(marked.error()));
            }
            results.push_back({.journal_id = record.id,
                               .outcome = FilePublicationRecoveryOutcome::needs_reconciliation,
                               .issue = verified.error()});
            continue;
        }
        if (auto dependent = transition(journal, record, State::target_published,
                                        State::dependent_state_committed, *record.target_revision);
            !dependent) {
            return std::unexpected(std::move(dependent.error()));
        }
        if (auto completed = transition(journal, record, State::dependent_state_committed,
                                        State::complete, *record.target_revision);
            !completed) {
            return std::unexpected(std::move(completed.error()));
        }
        results.push_back({.journal_id = record.id,
                           .outcome = FilePublicationRecoveryOutcome::completed,
                           .issue = std::nullopt});
    }
    return results;
}

} // namespace trackknife::operations
