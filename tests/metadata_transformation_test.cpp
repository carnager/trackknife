// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/transformation.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

trackknife::metadata::MetadataField field(std::string name, std::vector<std::string> values) {
    return trackknife::metadata::MetadataField{
        .canonical_name = trackknife::metadata::canonicalize_field_name(name),
        .native_name = std::move(name),
        .values = std::move(values),
        .qualifier = {},
        .provenance = trackknife::metadata::FieldProvenance::embedded,
    };
}

trackknife::metadata::StagedMetadataSelection selection() {
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::StagedMetadataSelection;
    using trackknife::metadata::StagedMetadataSource;
    const std::array<std::string_view, 6> preferred{"Title",   "Artist",  "Genre",
                                                    "Comment", "Summary", "Mood"};
    auto result = StagedMetadataSelection::create(
        {
            StagedMetadataSource{
                .raw_path = "/music/one.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"  One  "}), field("ARTIST", {"Alpha", "Beta"}),
                                   field("COMMENT", {"Remove me"})},
                        .unsupported_native_objects = {},
                    },
            },
            StagedMetadataSource{
                .raw_path = "/music/two.flac",
                .source_revision = std::nullopt,
                .baseline =
                    MetadataDocument{
                        .fields = {field("TITLE", {"  Two  "}), field("ARTIST", {"Alpha", "Beta"})},
                        .unsupported_native_objects = {},
                    },
            },
        },
        preferred);
    CHECK(result.has_value());
    return result ? std::move(*result) : StagedMetadataSelection{};
}

void orderedChainsSeeEarlierActionsAndCurrentDraft() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    StagedMetadataPatchSet draft;
    const auto artist = *baseline.field_index("artist");
    CHECK(draft.replace_values(baseline, 0U, artist, {"Draft Artist"}).has_value());

    MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Prepare display fields",
        .actions =
            {
                MetadataTransformValuesAction{.target_field = "Title",
                                              .transform = MetadataValueTransformKind::trim_ascii},
                MetadataTransformValuesAction{.target_field = "Title",
                                              .transform = MetadataValueTransformKind::uppercase},
                MetadataSetValuesAction{.target_field = "Genre", .values = {"Rock", "Alt"}},
                MetadataFormatValueAction{
                    .target_field = "Summary",
                    .dialect = {},
                    .source = "$join(artist, / ) — %title%",
                },
                MetadataRemoveFieldAction{.target_field = "Comment"},
            },
    };
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    if (!preview) {
        std::cerr << preview.error().message << '\n';
    }
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->chain.schema_version == chain.schema_version);
    CHECK(preview->chain.name == chain.name);
    CHECK(preview->chain.actions.size() == chain.actions.size());
    CHECK(std::get_if<MetadataTransformValuesAction>(&preview->chain.actions[0]) != nullptr);
    CHECK(std::get_if<MetadataFormatValueAction>(&preview->chain.actions[3]) != nullptr);
    CHECK(preview->item_indexes == (std::vector<std::size_t>{0U, 1U}));
    CHECK(preview->changed_item_count == 2U);
    CHECK(preview->cells.size() == 7U);
    CHECK(preview->cells[0].item_index == 0U);
    CHECK(preview->cells[0].canonical_field == "title");
    CHECK(preview->cells[0].before == std::optional<std::vector<std::string>>{{"  One  "}});
    CHECK(preview->cells[0].after == std::optional<std::vector<std::string>>{{"ONE"}});
    CHECK(preview->cells[0].last_action_index == 1U);
    CHECK(preview->cells[1].canonical_field == "genre");
    CHECK(preview->cells[1].before == std::nullopt);
    CHECK(preview->cells[1].after == (std::optional<std::vector<std::string>>{{"Rock", "Alt"}}));
    CHECK(preview->cells[2].canonical_field == "summary");
    CHECK(preview->cells[2].after ==
          std::optional<std::vector<std::string>>{{"Draft Artist — ONE"}});
    CHECK(preview->cells[3].canonical_field == "comment");
    CHECK(preview->cells[3].after == std::nullopt);
    CHECK(preview->cells[6].canonical_field == "summary");
    CHECK(preview->cells[6].after ==
          std::optional<std::vector<std::string>>{{"Alpha / Beta — TWO"}});
    CHECK(draft.patch_count() == 1U);
    CHECK(draft.patch(0U, artist)->values == (std::vector<std::string>{"Draft Artist"}));
}

void exactAddCopySplitAndJoinPreserveOrderedState() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Exact value operations",
        .actions =
            {
                MetadataAddValuesAction{.target_field = "Artist", .values = {"Alpha", ""}},
                MetadataCopyFieldAction{.target_field = "Credits", .source_field = "Artist"},
                MetadataSetValuesAction{.target_field = "Tags", .values = {"one||two", ""}},
                MetadataSplitValuesAction{.target_field = "Tags", .separator = "|"},
                MetadataAddValuesAction{.target_field = "Tags", .values = {"tail"}},
                MetadataJoinValuesAction{.target_field = "Tags", .separator = "::"},
                MetadataCopyFieldAction{.target_field = "Comment", .source_field = "Missing"},
                MetadataSetValuesAction{.target_field = "Mood",
                                        .values = {"élan", "two WORDS", ""}},
                MetadataTransformValuesAction{.target_field = "Mood",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
            },
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->cells.size() == 5U);
    CHECK(preview->cells[0].canonical_field == "artist");
    CHECK(preview->cells[0].after ==
          (std::optional<std::vector<std::string>>{{"Alpha", "Beta", "Alpha", ""}}));
    CHECK(preview->cells[1].canonical_field == "credits");
    CHECK(preview->cells[1].after ==
          (std::optional<std::vector<std::string>>{{"Alpha", "Beta", "Alpha", ""}}));
    CHECK(preview->cells[2].canonical_field == "tags");
    CHECK(preview->cells[2].after ==
          (std::optional<std::vector<std::string>>{{"one::::two::::tail"}}));
    CHECK(preview->cells[3].canonical_field == "comment");
    CHECK(preview->cells[3].before == (std::optional<std::vector<std::string>>{{"Remove me"}}));
    CHECK(preview->cells[3].after == std::nullopt);
    CHECK(preview->cells[4].canonical_field == "mood");
    CHECK(preview->cells[4].after ==
          (std::optional<std::vector<std::string>>{{"Élan", "Two WORDS", ""}}));
}

void capitalizationNoOpCountsPresentAndMissingTargets() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Already capitalized",
        .actions =
            {
                MetadataTransformValuesAction{.target_field = "Artist",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
                MetadataTransformValuesAction{.target_field = "Comment",
                                              .transform =
                                                  MetadataValueTransformKind::capitalize_first},
            },
    };
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->cells.empty());
    CHECK(preview->changed_item_count == 0U);
    CHECK(preview->unchanged_present_cell_count == 3U);
    CHECK(preview->unchanged_missing_cell_count == 1U);
}

void keepFirstCharactersUsesUnicodeAndRetainsShortValues() {
    using namespace trackknife::metadata;
    auto selected = StagedMetadataSelection::create({
        StagedMetadataSource{
            .raw_path = "/music/date-one.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("DATE", {"2024-08-30", "ééééé"})},
                    .unsupported_native_objects = {},
                },
        },
        StagedMetadataSource{
            .raw_path = "/music/date-two.flac",
            .source_revision = std::nullopt,
            .baseline =
                MetadataDocument{
                    .fields = {field("DATE", {"1999"})},
                    .unsupported_native_objects = {},
                },
        },
    });
    CHECK(selected.has_value());
    if (!selected) {
        return;
    }
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Keep year",
        .actions = {MetadataKeepFirstCharactersAction{
            .target_field = "Date",
            .character_count = 4U,
        }},
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(*selected, draft, items, chain);
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->changed_item_count == 1U);
    CHECK(preview->cells.size() == 1U);
    CHECK(preview->cells.front().before ==
          (std::optional<std::vector<std::string>>{{"2024-08-30", "ééééé"}}));
    CHECK(preview->cells.front().after ==
          (std::optional<std::vector<std::string>>{{"2024", "éééé"}}));
    CHECK(preview->unchanged_present_cell_count == 1U);
}

void exactMatchingAndSelectionNumberingComposeInOrder() {
    using namespace trackknife::metadata;
    const auto baseline = selection();
    const StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};
    const MetadataTransformationChain chain{
        .schema_version = 1U,
        .name = "Match and number",
        .actions =
            {
                MetadataRemoveMatchingValuesAction{.target_field = "Artist", .match = "alpha"},
                MetadataRemoveMatchingValuesAction{.target_field = "Artist", .match = "Alpha"},
                MetadataRemoveMatchingValuesAction{.target_field = "Comment", .match = "Remove me"},
                MetadataReplaceMatchingValuesAction{.target_field = "Title",
                                                    .match = "  One  ",
                                                    .replacement_values = {"First", "Alternate"}},
                MetadataNumberSelectedItemsAction{
                    .target_field = "Track Number", .start = 7U, .padding = 2U},
                MetadataFormatValueAction{
                    .target_field = "Summary",
                    .dialect = {},
                    .source = "%track number%: $join(artist, / ) — $join(title, / )",
                },
            },
    };
    CHECK(validate_metadata_transformation_chain(chain).has_value());
    const auto preview = plan_metadata_transformation(baseline, draft, items, chain);
    if (!preview) {
        std::cerr << preview.error().message << '\n';
    }
    CHECK(preview.has_value());
    if (!preview) {
        return;
    }
    CHECK(preview->changed_item_count == 2U);
    CHECK(preview->cells.size() == 8U);
    CHECK(preview->cells[0].canonical_field == "artist");
    CHECK(preview->cells[0].after == std::optional<std::vector<std::string>>{{"Beta"}});
    CHECK(preview->cells[0].last_action_index == 1U);
    CHECK(preview->cells[1].canonical_field == "comment");
    CHECK(preview->cells[1].after == std::nullopt);
    CHECK(preview->cells[2].canonical_field == "title");
    CHECK(preview->cells[2].after ==
          (std::optional<std::vector<std::string>>{{"First", "Alternate"}}));
    CHECK(preview->cells[3].canonical_field == "tracknumber");
    CHECK(preview->cells[3].after == std::optional<std::vector<std::string>>{{"07"}});
    CHECK(preview->cells[4].canonical_field == "summary");
    CHECK(preview->cells[4].after ==
          std::optional<std::vector<std::string>>{{"07: Beta — First / Alternate"}});
    CHECK(preview->cells[5].canonical_field == "artist");
    CHECK(preview->cells[5].after == std::optional<std::vector<std::string>>{{"Beta"}});
    CHECK(preview->cells[6].canonical_field == "tracknumber");
    CHECK(preview->cells[6].after == std::optional<std::vector<std::string>>{{"08"}});
    CHECK(preview->cells[7].canonical_field == "summary");
    CHECK(preview->cells[7].after ==
          std::optional<std::vector<std::string>>{{"08: Beta —   Two  "}});
}

void plansRejectInvalidDialectInputLimitsAndCancellation() {
    using namespace trackknife;
    const auto baseline = selection();
    const metadata::StagedMetadataPatchSet draft;
    const std::array items{std::size_t{0U}, std::size_t{1U}};

    const auto empty = metadata::plan_metadata_transformation(
        baseline, draft, items,
        metadata::MetadataTransformationChain{.schema_version = 1U, .name = {}, .actions = {}});
    CHECK(!empty);
    CHECK(empty.error().code == core::ErrorCode::invalid_argument);

    auto invalid_expression = metadata::MetadataTransformationChain{
        .schema_version = 1U,
        .name = {},
        .actions = {metadata::MetadataFormatValueAction{
            .target_field = "Title", .dialect = {}, .source = "$unknown(%title%)"}},
    };
    const auto invalid =
        metadata::plan_metadata_transformation(baseline, draft, items, invalid_expression);
    CHECK(!invalid);
    CHECK(invalid.error().message.find("unknown format function") != std::string::npos);

    auto* format_action =
        std::get_if<metadata::MetadataFormatValueAction>(&invalid_expression.actions.front());
    CHECK(format_action != nullptr);
    if (format_action == nullptr) {
        return;
    }
    format_action->dialect.dialect_version = 2U;
    const auto dialect =
        metadata::plan_metadata_transformation(baseline, draft, items, invalid_expression);
    CHECK(!dialect);
    CHECK(dialect.error().message.find("unsupported dialect") != std::string::npos);

    const auto invalid_split =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid split",
            .actions = {metadata::MetadataSplitValuesAction{.target_field = "Genre",
                                                            .separator = {}}},
        });
    CHECK(!invalid_split);
    CHECK(invalid_split.error().message.find("non-empty separator") != std::string::npos);

    const auto invalid_copy =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid copy",
            .actions = {metadata::MetadataCopyFieldAction{.target_field = "Genre",
                                                          .source_field = {}}},
        });
    CHECK(!invalid_copy);

    const auto invalid_replacement =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid replacement",
            .actions = {metadata::MetadataReplaceMatchingValuesAction{
                .target_field = "Genre", .match = "Rock", .replacement_values = {}}},
        });
    CHECK(!invalid_replacement);

    const auto invalid_number_start =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid number start",
            .actions = {metadata::MetadataNumberSelectedItemsAction{
                .target_field = "Track Number", .start = 0U, .padding = 2U}},
        });
    CHECK(!invalid_number_start);
    const auto invalid_number_padding =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid number padding",
            .actions = {metadata::MetadataNumberSelectedItemsAction{
                .target_field = "Track Number", .start = 1U, .padding = 33U}},
        });
    CHECK(!invalid_number_padding);

    const auto invalid_keep_count =
        metadata::validate_metadata_transformation_chain(metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = "Invalid keep count",
            .actions = {metadata::MetadataKeepFirstCharactersAction{.target_field = "Date",
                                                                    .character_count = 0U}},
        });
    CHECK(!invalid_keep_count);

    const metadata::MetadataTransformationChain bounded{
        .schema_version = 1U,
        .name = {},
        .actions = {metadata::MetadataSetValuesAction{.target_field = "Genre", .values = {"Rock"}}},
    };
    const auto limited = metadata::plan_metadata_transformation(
        baseline, draft, items, bounded, {},
        metadata::MetadataTransformationLimits{.items = 2U,
                                               .actions = 1U,
                                               .addressed_cells = 1U,
                                               .values_per_cell = 1U,
                                               .total_preview_text_bytes = 64U,
                                               .field_name_bytes = 64U,
                                               .chain_name_bytes = 64U,
                                               .action_text_bytes = 64U});
    CHECK(!limited);
    CHECK(limited.error().code == core::ErrorCode::limit_exceeded);

    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = metadata::plan_metadata_transformation(baseline, draft, items, bounded,
                                                                  cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == core::ErrorCode::cancelled);
}

} // namespace

int main() {
    orderedChainsSeeEarlierActionsAndCurrentDraft();
    exactAddCopySplitAndJoinPreserveOrderedState();
    capitalizationNoOpCountsPresentAndMissingTargets();
    keepFirstCharactersUsesUnicodeAndRetainsShortValues();
    exactMatchingAndSelectionNumberingComposeInOrder();
    plansRejectInvalidDialectInputLimitsAndCancellation();
    return failures == 0 ? 0 : 1;
}
