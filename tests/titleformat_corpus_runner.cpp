// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using trackknife::titleformat::FormatContextKind;

[[nodiscard]] std::string asciiLower(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] FormatContextKind contextKind(const std::string_view value) {
    if (value == "track") {
        return FormatContextKind::track_display;
    }
    if (value == "list") {
        return FormatContextKind::list_item;
    }
    if (value == "queue") {
        return FormatContextKind::queue;
    }
    if (value == "now_playing") {
        return FormatContextKind::now_playing;
    }
    if (value == "path") {
        return FormatContextKind::path_generation;
    }
    if (value == "tree") {
        return FormatContextKind::tree_level;
    }
    if (value == "group") {
        return FormatContextKind::grouping;
    }
    if (value == "copy") {
        return FormatContextKind::copy_export;
    }
    if (value == "sort") {
        return FormatContextKind::sort;
    }
    throw std::runtime_error("unknown format-expression host: " + std::string{value});
}

class CorpusContext final : public trackknife::titleformat::EvaluationContext {
  public:
    CorpusContext(const FormatContextKind kind, const Json& fields, const Json& information)
        : kind_(kind) {
        for (const auto& [raw_name, raw_value] : fields.items()) {
            MetadataValues values;
            if (raw_value.is_string()) {
                values.push_back(raw_value.get<std::string>());
            } else if (raw_value.is_array()) {
                values = raw_value.get<MetadataValues>();
            } else {
                throw std::runtime_error("field values must be strings or string arrays");
            }
            fields_.emplace_back(asciiLower(raw_name), std::move(values));
        }
        for (const auto& [raw_name, raw_value] : information.items()) {
            information_.emplace_back(asciiLower(raw_name), raw_value.get<std::string>());
        }
    }

    [[nodiscard]] FormatContextKind kind() const noexcept override { return kind_; }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto values = resolveMetadata(name);
        if (!values || values->empty()) {
            return std::nullopt;
        }
        return values->front();
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match = std::ranges::find(fields_, normalized, &Field::first);
        return match == fields_.end() ? std::nullopt : std::optional<MetadataValues>{match->second};
    }

    [[nodiscard]] std::optional<std::string>
    resolveTechnicalInfo(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match = std::ranges::find(information_, normalized, &Information::first);
        return match == information_.end() ? std::nullopt
                                           : std::optional<std::string>{match->second};
    }

  private:
    using Field = std::pair<std::string, MetadataValues>;
    using Information = std::pair<std::string, std::string>;
    FormatContextKind kind_;
    std::vector<Field> fields_;
    std::vector<Information> information_;
};

struct Counts {
    std::size_t files{0};
    std::size_t cases{0};
    std::size_t failures{0};
};

void runCase(const Json& test_case, Counts& counts) {
    const auto id = test_case.at("id").get<std::string>();
    const auto source = test_case.at("source").get<std::string>();
    const auto host = contextKind(test_case.at("host").get<std::string>());
    const auto& fields = test_case.at("fields");
    const auto information = test_case.value("information", Json::object());

    trackknife::titleformat::CompileOptions options;
    options.context = host;
    const auto compiled = trackknife::titleformat::compile(source, options);
    if (!compiled.program) {
        std::cerr << id << ": did not compile\n";
        ++counts.failures;
        ++counts.cases;
        return;
    }

    const CorpusContext context(host, fields, information);
    const auto actual = trackknife::titleformat::evaluate(*compiled.program, context);
    const auto expected = test_case.at("expected").get<std::string>();
    if (!actual) {
        std::cerr << id << ": evaluation error: " << actual.error().message << '\n';
        ++counts.failures;
    } else if (actual->text != expected) {
        std::cerr << id << ": expected " << Json(expected).dump() << ", actual "
                  << Json(actual->text).dump() << '\n';
        ++counts.failures;
    }
    ++counts.cases;
}

void runFile(const std::filesystem::path& path, Counts& counts) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error("cannot open corpus file: " + path.string());
    }
    const auto document = Json::parse(input);
    if (document.at("schema_version").get<unsigned int>() != 2U ||
        document.at("dialect").at("name") != "tkfmt" ||
        document.at("dialect").at("version").get<unsigned int>() != 1U) {
        throw std::runtime_error("unsupported corpus dialect/schema in " + path.string());
    }
    for (const auto& test_case : document.at("cases")) {
        runCase(test_case, counts);
    }
    ++counts.files;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: trackknife_titleformat_corpus_runner CORPUS_DIRECTORY\n";
        return 2;
    }

    Counts counts;
    try {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator{argv[1]}) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        if (files.empty()) {
            throw std::runtime_error("format-expression corpus contains no JSON case files");
        }
        for (const auto& file : files) {
            runFile(file, counts);
        }
    } catch (const std::exception& exception) {
        std::cerr << "corpus error: " << exception.what() << '\n';
        return 2;
    }

    std::cout << "corpus_files=" << counts.files << " cases=" << counts.cases
              << " failures=" << counts.failures << '\n';
    return counts.failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
