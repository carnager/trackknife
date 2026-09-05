// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace trackknife::persistence {

struct LibraryRoot {
    std::string raw_path;
    bool available{false};
    std::string error;
};

enum class LibraryEntryKind { artist, album, track };

struct LibraryQuery {
    LibraryEntryKind kind{LibraryEntryKind::artist};
    std::string text;
    std::optional<std::string> artist;
    std::optional<std::string> album_key;
    std::optional<std::string> raw_path;
    std::size_t offset{0};
    std::size_t limit{200};
};

struct LibraryEntry {
    LibraryEntryKind kind{LibraryEntryKind::track};
    std::string key;
    std::string label;
    std::string artist;
    std::string album;
    std::size_t tracks{0};
    std::size_t available{0};
};

struct LibraryPage {
    std::vector<LibraryEntry> entries;
    bool more{false};
};

struct LibraryScanProgress {
    std::atomic<std::size_t> visited{0};
    std::atomic<std::size_t> indexed{0};
    std::atomic<std::size_t> failed{0};
};

struct LibraryScanResult {
    bool cancelled{false};
    bool incomplete{false};
};

// Synchronous, Qt-free service. Each worker owns its own connection; all
// traversal, probing, SQL, and query formatting must run off the UI thread.
class LocalLibrary final {
  public:
    static core::Result<LocalLibrary> open(const std::filesystem::path& database_path);
    LocalLibrary(LocalLibrary&&) noexcept;
    LocalLibrary& operator=(LocalLibrary&&) noexcept;
    ~LocalLibrary();
    LocalLibrary(const LocalLibrary&) = delete;
    LocalLibrary& operator=(const LocalLibrary&) = delete;

    core::Result<std::vector<LibraryRoot>> roots() const;
    core::Result<void> add_root(const std::string& raw_path);
    core::Result<void> remove_root(const std::string& raw_path);
    core::Result<LibraryPage> query(const LibraryQuery& query,
                                    const core::CancellationToken& cancellation = {}) const;
    core::Result<std::vector<std::string>>
    paths(const LibraryQuery& query, const core::CancellationToken& cancellation = {}) const;
    core::Result<LibraryScanResult> scan(const core::CancellationToken& cancellation,
                                         LibraryScanProgress& progress);

  private:
    struct Impl;
    explicit LocalLibrary(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

// Called inside ListRepository's existing metadata/relocation transaction.
// No filesystem I/O or nested transaction; only already indexed rows change.
core::Result<void> refresh_library_source(sqlite3* database, const std::string& source,
                                          const std::string& target,
                                          const metadata::MetadataDocument* document);

} // namespace trackknife::persistence
