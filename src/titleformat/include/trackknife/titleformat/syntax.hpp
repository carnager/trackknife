// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace trackknife::titleformat {

struct SourceSpan {
    std::size_t begin{0};
    std::size_t end{0};

    [[nodiscard]] constexpr std::size_t length() const noexcept { return end - begin; }
    [[nodiscard]] constexpr bool empty() const noexcept { return begin == end; }
    friend constexpr bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

using NodeId = std::uint32_t;

struct SequenceNode {
    std::vector<NodeId> children;
};

struct LiteralNode {
    std::string value;
};

struct FieldNode {
    std::string name;
    SourceSpan name_span;
    bool terminated{true};
};

struct CallNode {
    std::string name;
    SourceSpan name_span;
    std::vector<NodeId> arguments;
    bool terminated{true};
};

struct ErrorNode {
    std::string recovered_text;
};

using NodeData = std::variant<SequenceNode, LiteralNode, FieldNode, CallNode, ErrorNode>;

struct SyntaxNode {
    SourceSpan span;
    NodeData data;
};

class SyntaxTree final {
  public:
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] NodeId root() const noexcept { return root_; }
    [[nodiscard]] const SyntaxNode& node(NodeId id) const;
    [[nodiscard]] std::string_view sourceText(SourceSpan span) const;
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

  private:
    std::string source_;
    std::vector<SyntaxNode> nodes_;
    NodeId root_{0};

    friend class Parser;
};

} // namespace trackknife::titleformat
