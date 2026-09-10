// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/artwork_writers.hpp"

#include "container_preservation_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <attachedpictureframe.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4item.h>
#include <mp4tag.h>
#include <mpegfile.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

using text_writer_detail::PreparedPathGuard;
using text_writer_detail::system_error;

// One qualified container: the artwork adapter, its text-adapter sibling,
// how a change is applied to the prepared copy, and the container
// preservation proof. The APIC writer keeps types and descriptions; the
// covr writer produces untyped entries (ADR-0137).
struct ContainerArtworkFormat {
    std::string_view label;
    std::string_view artwork_adapter;
    std::string_view text_adapter;
    // Pictures surface as unsupported native objects in the text document
    // ("covr", "APIC[:description]"); the text-unchanged check must ignore
    // exactly these while every other native object stays pinned.
    std::string_view artwork_object_prefix;
    bool supports_descriptions{false};
    std::function<core::Result<void>(const ArtworkWritePlanSource&, const std::string&,
                                     const std::vector<unsigned char>&)>
        apply;
    std::function<core::Result<void>(const std::string&, const std::string&,
                                     const core::CancellationToken&)>
        verify_preservation;
};

[[nodiscard]] core::Error writer_error(const core::ErrorCode code, std::string message,
                                       const std::string& source_raw_path,
                                       const std::string& prepared_raw_path) {
    return text_writer_detail::writer_error(code, std::move(message), source_raw_path,
                                            prepared_raw_path);
}

[[nodiscard]] core::Error cancelled(const ContainerArtworkFormat& format,
                                    const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(format.label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<LocalArtworkInventory>
read_embedded_inventory(const std::string& raw_path, const core::CancellationToken& cancellation) {
    auto policy = default_artwork_inventory_policy();
    policy.external_patterns.clear();
    return read_local_artwork_inventory(raw_path, policy, cancellation);
}

[[nodiscard]] std::vector<TagLib::ID3v2::AttachedPictureFrame*>
apic_frames(TagLib::ID3v2::Tag* tag) {
    std::vector<TagLib::ID3v2::AttachedPictureFrame*> frames;
    if (tag == nullptr) {
        return frames;
    }
    for (auto* frame : tag->frameListMap()["APIC"]) {
        frames.push_back(dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame));
    }
    return frames;
}

[[nodiscard]] TagLib::ByteVector to_byte_vector(const std::vector<unsigned char>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned int>(bytes.size())};
}

[[nodiscard]] core::Result<void> apply_apic_change(const ArtworkWritePlanSource& source_plan,
                                                   const std::string& prepared_raw_path,
                                                   const std::vector<unsigned char>& replacement) {
    TagLib::MPEG::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP3 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto* tag = file.ID3v2Tag(true);
    auto frames = apic_frames(tag);
    const auto ordinal = source_plan.change.target_ordinal;
    const auto& change = source_plan.change;
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::remove:
        if (ordinal >= frames.size() || frames[ordinal] == nullptr) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted APIC frame no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        tag->removeFrame(frames[ordinal]);
        break;
    case ArtworkWritePlanIntentKind::replace:
        if (ordinal >= frames.size() || frames[ordinal] == nullptr) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted APIC frame no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        // In-place mutation keeps the frame's position, type, and
        // description.
        frames[ordinal]->setMimeType(
            TagLib::String{change.replacement->mime_type, TagLib::String::UTF8});
        frames[ordinal]->setPicture(to_byte_vector(replacement));
        break;
    case ArtworkWritePlanIntentKind::add: {
        auto* frame = new TagLib::ID3v2::AttachedPictureFrame();
        frame->setType(static_cast<TagLib::ID3v2::AttachedPictureFrame::Type>(
            artwork_detail::canonical_picture_type(change.added_role)));
        frame->setMimeType(TagLib::String{change.replacement->mime_type, TagLib::String::UTF8});
        frame->setDescription(TagLib::String{change.added_description, TagLib::String::UTF8});
        frame->setPicture(to_byte_vector(replacement));
        tag->addFrame(frame);
        break;
    }
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib could not save the prepared MP3 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_covr_change(const ArtworkWritePlanSource& source_plan,
                                                   const std::string& prepared_raw_path,
                                                   const std::vector<unsigned char>& replacement) {
    TagLib::MP4::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid() || file.tag() == nullptr) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP4 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto* tag = file.tag();
    auto covers =
        tag->contains("covr") ? tag->item("covr").toCoverArtList() : TagLib::MP4::CoverArtList{};
    const auto ordinal = source_plan.change.target_ordinal;
    const auto& change = source_plan.change;
    const auto replacement_cover = [&] {
        const auto format = change.replacement->mime_type == "image/png"
                                ? TagLib::MP4::CoverArt::PNG
                                : TagLib::MP4::CoverArt::JPEG;
        return TagLib::MP4::CoverArt{format, to_byte_vector(replacement)};
    };
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::remove:
    case ArtworkWritePlanIntentKind::replace: {
        if (ordinal >= covers.size()) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted covr entry no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        TagLib::MP4::CoverArtList rebuilt;
        for (unsigned int index = 0U; index < covers.size(); ++index) {
            if (index == ordinal) {
                if (change.kind == ArtworkWritePlanIntentKind::replace) {
                    rebuilt.append(replacement_cover());
                }
                continue;
            }
            rebuilt.append(covers[index]);
        }
        covers = std::move(rebuilt);
        break;
    }
    case ArtworkWritePlanIntentKind::add:
        covers.append(replacement_cover());
        break;
    }
    if (covers.isEmpty()) {
        tag->removeItem("covr");
    } else {
        tag->setItem("covr", TagLib::MP4::Item{covers});
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib could not save the prepared MP4 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] const ContainerArtworkFormat& mp3_format() {
    static const ContainerArtworkFormat format{
        .label = "MP3",
        .artwork_adapter = "taglib-id3v2-apic-v1",
        .text_adapter = "taglib-mpeg-v1",
        .artwork_object_prefix = "APIC",
        .supports_descriptions = true,
        .apply = apply_apic_change,
        .verify_preservation = preservation_detail::verify_mp3_binary_preservation,
    };
    return format;
}

[[nodiscard]] const ContainerArtworkFormat& mp4_format() {
    static const ContainerArtworkFormat format{
        .label = "MP4",
        .artwork_adapter = "taglib-mp4-covr-v1",
        .text_adapter = "taglib-mp4-v1",
        .artwork_object_prefix = "covr",
        .supports_descriptions = false,
        .apply = apply_covr_change,
        .verify_preservation = preservation_detail::verify_mp4_box_preservation,
    };
    return format;
}

// Predicts one resulting item's identity attributes so the reread
// inventory can be compared exactly.
struct ExpectedItem {
    core::ContentFingerprint fingerprint;
    std::string native_type;
    std::string mime_type;
    std::string description;
    ArtworkRole role{ArtworkRole::other};
};

[[nodiscard]] ExpectedItem expected_from(const ArtworkInventoryItem& item) {
    return ExpectedItem{
        .fingerprint = item.content_fingerprint,
        .native_type = item.native_type,
        .mime_type = item.mime_type,
        .description = item.description,
        .role = item.role,
    };
}

[[nodiscard]] core::Result<void> verify_inventory_result(const ContainerArtworkFormat& format,
                                                         const LocalArtworkInventory& before,
                                                         const LocalArtworkInventory& after,
                                                         const ArtworkWritePlanSource& source_plan,
                                                         const std::string& prepared_raw_path) {
    const auto& change = source_plan.change;
    std::vector<ExpectedItem> expected;
    expected.reserve(before.items.size() + 1U);
    for (const auto& item : before.items) {
        expected.push_back(expected_from(item));
    }
    const auto covr = format.artwork_adapter == "taglib-mp4-covr-v1";
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::remove:
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(change.target_ordinal));
        break;
    case ArtworkWritePlanIntentKind::replace: {
        auto& target = expected[change.target_ordinal];
        target.fingerprint = change.replacement->content_fingerprint;
        target.mime_type = change.replacement->mime_type;
        break;
    }
    case ArtworkWritePlanIntentKind::add: {
        const auto type = artwork_detail::canonical_picture_type(change.added_role);
        expected.push_back(ExpectedItem{
            .fingerprint = change.replacement->content_fingerprint,
            .native_type =
                covr ? std::string{} : TagLib::FLAC::Picture::typeToString(type).to8Bit(true),
            .mime_type = change.replacement->mime_type,
            .description = covr ? std::string{} : change.added_description,
            .role = covr ? ArtworkRole::front : change.added_role,
        });
        break;
    }
    }
    const auto mismatch = [&] {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "prepared artwork inventory differs from the planned result",
            source_plan.raw_media_path, prepared_raw_path));
    };
    if (after.items.size() != expected.size()) {
        return mismatch();
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const auto& item = after.items[index];
        const auto& want = expected[index];
        if (item.provenance != ArtworkProvenance::embedded || item.source_ordinal != index ||
            item.content_fingerprint != want.fingerprint || item.native_type != want.native_type ||
            item.mime_type != want.mime_type || item.description != want.description ||
            item.role != want.role) {
            return mismatch();
        }
    }
    return {};
}

[[nodiscard]] core::Result<PreparedArtworkWrite> prepare_container_artwork_write_copy(
    const ContainerArtworkFormat& format, const ArtworkWritePlanSource& source_plan,
    const std::string& prepared_raw_path, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }
    const auto replace = source_plan.change.kind == ArtworkWritePlanIntentKind::replace;
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    const auto uses_replacement = replace || add;
    const auto replacement_representable =
        !uses_replacement ||
        (source_plan.change.replacement && source_plan.change.replacement->width &&
         source_plan.change.replacement->height && source_plan.change.replacement->byte_size > 0U &&
         (source_plan.change.replacement->mime_type == "image/png" ||
          source_plan.change.replacement->mime_type == "image/jpeg"));
    if (source_plan.raw_media_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_media_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos ||
        source_plan.raw_media_path == prepared_raw_path || !source_plan.ready() ||
        source_plan.adapter_name != format.artwork_adapter ||
        !source_plan.expected_media_revision || !source_plan.observed_media_revision ||
        *source_plan.expected_media_revision != *source_plan.observed_media_revision ||
        source_plan.occurrence_indexes.empty() ||
        uses_replacement != source_plan.change.replacement.has_value() ||
        !replacement_representable || (add && source_plan.change.original) ||
        (!add &&
         (!source_plan.change.original ||
          source_plan.change.original->provenance != ArtworkProvenance::embedded ||
          source_plan.change.original->raw_source_path != source_plan.raw_media_path ||
          source_plan.change.original->source_revision != *source_plan.observed_media_revision ||
          source_plan.change.original->source_ordinal != source_plan.change.target_ordinal ||
          source_plan.change.original->content_fingerprint !=
              source_plan.change.expected_target_fingerprint))) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared artwork write requires one complete ready source plan",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (add && !format.supports_descriptions && !source_plan.change.added_description.empty()) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "MP4 covr entries cannot carry a picture description",
                                            source_plan.raw_media_path, prepared_raw_path));
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_media_revision) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "the source changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }
    auto before_inventory = read_embedded_inventory(source_plan.raw_media_path, cancellation);
    if (!before_inventory) {
        return std::unexpected(std::move(before_inventory.error()));
    }
    if (before_inventory->media_revision != *source_plan.observed_media_revision ||
        before_inventory->embedded_adapter_name != format.artwork_adapter) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the artwork source no longer matches the previewed adapter and revision",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (add) {
        if (source_plan.change.target_ordinal != before_inventory->items.size()) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "the fresh artwork count differs from the previewed insertion ordinal",
                             source_plan.raw_media_path, prepared_raw_path));
        }
        const auto duplicate = std::ranges::find_if(before_inventory->items, [&](const auto& item) {
            return item.content_fingerprint == source_plan.change.replacement->content_fingerprint;
        });
        if (duplicate != before_inventory->items.end()) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the source already contains the added image",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
    } else {
        const auto target = std::ranges::find_if(before_inventory->items, [&](const auto& item) {
            return item.provenance == ArtworkProvenance::embedded &&
                   item.source_ordinal == source_plan.change.target_ordinal;
        });
        if (target == before_inventory->items.end() || *target != *source_plan.change.original ||
            target->content_fingerprint != source_plan.change.expected_target_fingerprint) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "the fresh artwork differs from the previewed target picture",
                             source_plan.raw_media_path, prepared_raw_path));
        }
    }

    std::vector<unsigned char> replacement_bytes;
    if (uses_replacement) {
        auto loaded =
            read_artwork_image_bytes(*source_plan.change.replacement,
                                     source_plan.change.replacement->byte_size, cancellation);
        if (!loaded) {
            auto error = std::move(loaded.error());
            error.context.push_back(
                {.key = "target", .value = core::escape_raw_path(source_plan.raw_media_path)});
            return std::unexpected(std::move(error));
        }
        replacement_bytes = std::move(*loaded);
    }
    auto before_document = read_local_metadata(source_plan.raw_media_path, cancellation);
    if (!before_document) {
        return std::unexpected(std::move(before_document.error()));
    }
    if (before_document->source_revision != *source_plan.observed_media_revision ||
        before_document->adapter_name != format.text_adapter) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "the metadata changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        format.label, source_plan.raw_media_path, *source_plan.observed_media_revision,
        prepared_raw_path, cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = format.apply(source_plan, prepared_raw_path, replacement_bytes);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared artwork permissions failed", errno,
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }

    auto after_document = read_local_metadata(prepared_raw_path, cancellation);
    if (!after_document) {
        return std::unexpected(std::move(after_document.error()));
    }
    const auto non_artwork_objects = [&format](const MetadataDocument& document) {
        auto objects = document.unsupported_native_objects;
        std::erase_if(objects, [&format](const NativeObjectIdentity& object) {
            return std::string_view{object.identity}.starts_with(format.artwork_object_prefix);
        });
        return objects;
    };
    if (after_document->document.fields != before_document->document.fields ||
        non_artwork_objects(after_document->document) !=
            non_artwork_objects(before_document->document)) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the prepared artwork write changed the text metadata document",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    auto after_inventory = read_embedded_inventory(prepared_raw_path, cancellation);
    if (!after_inventory) {
        return std::unexpected(std::move(after_inventory.error()));
    }
    if (after_inventory->media_revision != after_document->source_revision ||
        after_inventory->embedded_adapter_name != format.artwork_adapter) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "the prepared artwork and metadata rereads disagree",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto inventory_verified = verify_inventory_result(format, *before_inventory, *after_inventory,
                                                      source_plan, prepared_raw_path);
    if (!inventory_verified) {
        return std::unexpected(std::move(inventory_verified.error()));
    }
    auto preserved =
        format.verify_preservation(source_plan.raw_media_path, prepared_raw_path, cancellation);
    if (!preserved) {
        return std::unexpected(std::move(preserved.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_media_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the source changed while the prepared artwork copy was being verified",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedArtworkWrite{
        .source_raw_path = source_plan.raw_media_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after_document->source_revision,
        .document = std::move(after_document->document),
        .inventory = std::move(*after_inventory),
        .kind = source_plan.change.kind,
        .target_ordinal = source_plan.change.target_ordinal,
    };
}

} // namespace

core::Result<PreparedArtworkWrite>
prepare_mp3_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation) {
    return prepare_container_artwork_write_copy(mp3_format(), source_plan, prepared_raw_path,
                                                cancellation);
}

core::Result<PreparedArtworkWrite>
prepare_mp4_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation) {
    return prepare_container_artwork_write_copy(mp4_format(), source_plan, prepared_raw_path,
                                                cancellation);
}

core::Result<PreparedArtworkWrite>
prepare_qualified_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                                     const std::string& prepared_raw_path,
                                     const core::CancellationToken& cancellation) {
    if (source_plan.adapter_name == "taglib-id3v2-apic-v1") {
        return prepare_mp3_artwork_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    if (source_plan.adapter_name == "taglib-mp4-covr-v1") {
        return prepare_mp4_artwork_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    return prepare_flac_artwork_write_copy(source_plan, prepared_raw_path, cancellation);
}

} // namespace trackknife::metadata
