// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/write_plan.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] trackknife::core::LocalSourceRevision revision(const std::uint64_t inode,
                                                             const std::uint64_t size = 1'024U) {
    return trackknife::core::LocalSourceRevision{
        .device = 7U,
        .inode = inode,
        .size = size,
        .modification_time_seconds = 100,
        .modification_time_nanoseconds = 200,
    };
}

[[nodiscard]] trackknife::metadata::MetadataField
field(std::string name, std::vector<std::string> values,
      const trackknife::metadata::FieldProvenance provenance =
          trackknife::metadata::FieldProvenance::embedded) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = provenance,
    };
}

[[nodiscard]] trackknife::metadata::StagedMetadataSource
source(std::string path, std::optional<trackknife::core::LocalSourceRevision> source_revision,
       std::vector<trackknife::metadata::MetadataField> fields) {
    return trackknife::metadata::StagedMetadataSource{
        .raw_path = std::move(path),
        .source_revision = source_revision,
        .baseline =
            trackknife::metadata::MetadataDocument{
                .fields = std::move(fields),
                .unsupported_native_objects = {},
            },
    };
}

[[nodiscard]] trackknife::metadata::StagedMetadataSelection shared_source_selection() {
    using trackknife::metadata::FieldProvenance;
    using trackknife::metadata::StagedMetadataSelection;
    auto result = StagedMetadataSelection::create({
        source("/music/shared.flac", revision(11U),
               {field("TITLE", {"Physical title"}),
                field("TRACKNUMBER", {"1"}, FieldProvenance::segment)}),
        source("/music/shared.flac", revision(11U),
               {field("TITLE", {"Physical title"}),
                field("TRACKNUMBER", {"2"}, FieldProvenance::segment)}),
        source("/music/other.flac", revision(12U), {field("DATE", {"2024"})}),
    });
    CHECK(result.has_value());
    return result ? std::move(*result) : StagedMetadataSelection{};
}

[[nodiscard]] trackknife::metadata::LocalMetadataRead
read(std::string path, const trackknife::core::LocalSourceRevision source_revision,
     std::vector<trackknife::metadata::MetadataField> fields, const bool writable = true,
     const bool preserves_unknown = true) {
    return trackknife::metadata::LocalMetadataRead{
        .raw_path = std::move(path),
        .source_revision = source_revision,
        .document =
            trackknife::metadata::MetadataDocument{
                .fields = std::move(fields),
                .unsupported_native_objects = {},
            },
        .adapter_name = "fixture-writer-v1",
        .capabilities =
            trackknife::metadata::MetadataCapabilities{
                .fields_readable = true,
                .fields_writable = writable,
                .pictures_readable = true,
                .pictures_writable = false,
                .unknown_data_preserved_on_write = preserves_unknown,
            },
    };
}

[[nodiscard]] bool has_issue(const trackknife::metadata::MetadataWritePlanSource& source_plan,
                             const trackknife::metadata::MetadataWritePlanIssueKind kind) {
    return std::ranges::any_of(source_plan.issues,
                               [kind](const auto& issue) { return issue.kind == kind; });
}

void groupsEveryIntentAndReportsLogicalConflicts() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    const auto selection = shared_source_selection();
    const auto title = *selection.field_index("title");
    const auto track_number = *selection.field_index("tracknumber");
    const auto date = *selection.field_index("date");
    auto extended = selection;
    const auto genre = extended.ensure_missing_field("GENRE", "GENRE");
    CHECK(genre.has_value());

    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(extended, 0U, title, {"First intent"}).has_value());
    CHECK(patches.replace_values(extended, 1U, title, {"Second intent"}).has_value());
    CHECK(patches.replace_values(extended, 0U, *genre, {"Rock"}).has_value());
    CHECK(patches.replace_values(extended, 1U, *genre, {"Rock"}).has_value());
    CHECK(patches.replace_values(extended, 0U, track_number, {"9"}).has_value());
    CHECK(patches.remove_field(extended, 2U, date).has_value());

    std::unordered_map<std::string, int> reads;
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        extended, patches,
        [&reads](const std::string& path, const trackknife::core::CancellationToken&) {
            ++reads[path];
            if (path == "/music/shared.flac") {
                return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                    read(path, revision(11U),
                         {field("TITLE", {"Physical title"}), field("TRACKNUMBER", {"1"})})};
            }
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(12U), {field("DATE", {"2024"})})};
        });
    CHECK(planned.has_value());
    if (!planned) {
        return;
    }
    CHECK(planned->patch_count == 6U);
    CHECK(planned->sources.size() == 2U);
    CHECK(reads["/music/shared.flac"] == 1);
    CHECK(reads["/music/other.flac"] == 1);

    const auto& shared = planned->sources[0];
    CHECK(shared.raw_path == "/music/shared.flac");
    CHECK(shared.occurrence_indexes == (std::vector<std::size_t>{0U, 1U}));
    CHECK(shared.changes.size() == 3U);
    CHECK(!shared.ready());
    CHECK(has_issue(shared, MetadataWritePlanIssueKind::conflicting_logical_edits));
    CHECK(has_issue(shared, MetadataWritePlanIssueKind::unresolved_non_embedded_target));

    const auto title_change = std::ranges::find(
        shared.changes, title, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(title_change != shared.changes.end());
    CHECK(title_change != shared.changes.end() && title_change->original_present);
    CHECK(title_change != shared.changes.end() &&
          title_change->original_values == (std::vector<std::string>{"Physical title"}));
    CHECK(title_change != shared.changes.end() && title_change->intents.size() == 2U);
    CHECK(title_change != shared.changes.end() && title_change->conflicting_intents);

    const auto genre_change = std::ranges::find(
        shared.changes, *genre, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(genre_change != shared.changes.end());
    CHECK(genre_change != shared.changes.end() && !genre_change->original_present);
    CHECK(genre_change != shared.changes.end() && genre_change->intents.size() == 2U);
    CHECK(genre_change != shared.changes.end() && !genre_change->conflicting_intents);

    const auto track_change = std::ranges::find(
        shared.changes, track_number, &trackknife::metadata::MetadataWritePlanChange::field_index);
    CHECK(track_change != shared.changes.end());
    CHECK(track_change != shared.changes.end() && track_change->unresolved_non_embedded_target);

    const auto& other = planned->sources[1];
    CHECK(other.ready());
    CHECK(other.changes.size() == 1U);
    CHECK(other.changes.front().intents.front().kind == StagedMetadataPatchKind::remove_field);
    CHECK(planned->ready_source_count() == 1U);
    CHECK(planned->blocking_issue_count() == 2U);
    CHECK(!planned->ready());
}

void reportsRevisionCapabilityReadAndAliasBlockers() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;

    auto changed_selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/changed.flac", revision(21U), {field("TITLE", {"Old"})}),
        source("/music/missing-revision.flac", std::nullopt, {field("TITLE", {"Old"})}),
        source("/music/alias.flac", revision(21U), {field("TITLE", {"Old"})}),
    });
    CHECK(changed_selection.has_value());
    if (!changed_selection) {
        return;
    }
    const auto title = *changed_selection->field_index("title");
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*changed_selection, 0U, title, {"New"}).has_value());
    CHECK(patches.replace_values(*changed_selection, 1U, title, {"New"}).has_value());
    CHECK(patches.replace_values(*changed_selection, 2U, title, {"New"}).has_value());

    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *changed_selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            if (path == "/music/missing-revision.flac") {
                return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                    std::unexpected(trackknife::core::Error{
                        .code = trackknife::core::ErrorCode::io,
                        .message = "fixture read failure",
                        .context = {},
                    })};
            }
            // changed.flac and alias.flac deliberately resolve to the same
            // observed physical identity. Neither exposes a proven writer.
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(99U, 2'048U), {field("TITLE", {"Current"})}, false, false)};
        });
    CHECK(planned.has_value());
    if (!planned) {
        return;
    }
    const auto& changed = planned->sources[0];
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::source_changed));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::writer_unavailable));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::preservation_unproven));
    CHECK(has_issue(changed, MetadataWritePlanIssueKind::physical_source_alias));

    const auto& missing = planned->sources[1];
    CHECK(has_issue(missing, MetadataWritePlanIssueKind::missing_baseline_revision));
    CHECK(has_issue(missing, MetadataWritePlanIssueKind::source_revalidation_failed));
    CHECK(!missing.observed_revision.has_value());

    const auto& alias = planned->sources[2];
    CHECK(has_issue(alias, MetadataWritePlanIssueKind::source_changed));
    CHECK(has_issue(alias, MetadataWritePlanIssueKind::physical_source_alias));
}

void rejectsInconsistentCapturedRevisionsForOnePath() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/shared.flac", revision(31U), {field("TITLE", {"One"})}),
        source("/music/shared.flac", revision(32U), {field("TITLE", {"Two"})}),
    });
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = *selection->field_index("title");
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, title, {"Shared"}).has_value());
    CHECK(patches.replace_values(*selection, 1U, title, {"Shared"}).has_value());
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                read(path, revision(31U), {field("TITLE", {"One"})})};
        });
    CHECK(planned.has_value());
    CHECK(planned && planned->sources.size() == 1U);
    CHECK(planned && has_issue(planned->sources.front(),
                               MetadataWritePlanIssueKind::inconsistent_baseline_revision));
}

void rejectsEmptyInvalidAndCancelledPlanning() {
    const auto selection = shared_source_selection();
    trackknife::metadata::StagedMetadataPatchSet patches;
    const auto reader = [](const std::string&, const trackknife::core::CancellationToken&) {
        return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
            std::unexpected(trackknife::core::Error{
                .code = trackknife::core::ErrorCode::invariant,
                .message = "reader should not run",
                .context = {},
            })};
    };
    const auto empty = trackknife::metadata::build_metadata_write_plan(selection, patches, reader);
    CHECK(!empty.has_value());
    CHECK(empty.error().code == trackknife::core::ErrorCode::invalid_argument);

    const auto title = *selection.field_index("title");
    CHECK(patches.replace_values(selection, 0U, title, {"Draft"}).has_value());
    const auto no_reader = trackknife::metadata::build_metadata_write_plan(
        selection, patches, trackknife::metadata::MetadataWritePlanReader{});
    CHECK(!no_reader.has_value());
    CHECK(no_reader.error().code == trackknife::core::ErrorCode::invalid_argument);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::metadata::build_metadata_write_plan(
        selection, patches, reader, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);
}

void blocksUntouchedExactEmptyFlacValues() {
    using trackknife::metadata::MetadataWritePlanIssueKind;
    using trackknife::metadata::StagedMetadataPatchSet;
    auto selection = trackknife::metadata::StagedMetadataSelection::create({
        source("/music/empty-value.flac", revision(41U),
               {field("TITLE", {"Old"}), field("CUSTOM_FIELD", {""})}),
    });
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"New"}).has_value());
    const auto planned = trackknife::metadata::build_metadata_write_plan(
        *selection, patches,
        [](const std::string& path, const trackknife::core::CancellationToken&) {
            auto result =
                read(path, revision(41U), {field("TITLE", {"Old"}), field("CUSTOM_FIELD", {""})});
            result.adapter_name = "taglib-flac-v1";
            return trackknife::core::Result<trackknife::metadata::LocalMetadataRead>{
                std::move(result)};
        });
    CHECK(planned.has_value());
    CHECK(planned && !planned->ready());
    CHECK(planned && has_issue(planned->sources.front(),
                               MetadataWritePlanIssueKind::unsupported_field_mapping));
}

} // namespace

int main() {
    groupsEveryIntentAndReportsLogicalConflicts();
    reportsRevisionCapabilityReadAndAliasBlockers();
    rejectsInconsistentCapturedRevisionsForOnePath();
    rejectsEmptyInvalidAndCancelledPlanning();
    blocksUntouchedExactEmptyFlacValues();
    return failures == 0 ? 0 : 1;
}
