// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    trackknife::titleformat::ParseOptions options;
    options.mode = trackknife::titleformat::ParseMode::editor;
    options.maximum_source_bytes = 64U * 1024U;
    options.maximum_nesting_depth = 64U;
    const auto output = trackknife::titleformat::parse(
        std::string{reinterpret_cast<const char*>(data), size}, options);

    for (std::size_t index = 0; index < output.tree.nodeCount(); ++index) {
        const auto& node = output.tree.node(static_cast<trackknife::titleformat::NodeId>(index));
        static_cast<void>(output.tree.sourceText(node.span));
    }
    return 0;
}
