// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/query/tkq.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using trackknife::query::compile_tkq;
using trackknife::query::TkqComparison;
using trackknife::query::TkqNodeKind;
using trackknife::query::TkqOperandKind;
using trackknife::query::TkqSortDirection;

void simpleWordsBecomeAnAllWordSearch() {
    const auto compiled = compile_tkq("miles DAVIS Kind\tof blue");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    CHECK(!compiled->match_all);
    CHECK(compiled->predicates.size() == 1U);
    const auto& predicate = compiled->predicates.front();
    CHECK(predicate.operand == TkqOperandKind::any_field);
    CHECK(predicate.comparison == TkqComparison::has);
    CHECK(predicate.words == (std::vector<std::string>{"miles", "davis", "kind", "of", "blue"}));
    CHECK(compiled->source == "miles DAVIS Kind\tof blue");
    CHECK(compiled->dialect == "tkq");
    CHECK(compiled->dialect_version == 1);

    // Lowercase keyword spellings are ordinary words, not structure.
    const auto lowercase = compile_tkq("black and white");
    CHECK(lowercase.has_value());
    CHECK(lowercase && lowercase->predicates.size() == 1U &&
          lowercase->predicates.front().words ==
              (std::vector<std::string>{"black", "and", "white"}));
}

void structuredQueriesParseWithPrecedence() {
    const auto compiled =
        compile_tkq("genre HAS jazz AND date GREATER 1990 OR NOT artist IS \"Miles Davis\"");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    const auto& root = compiled->nodes[compiled->root];
    CHECK(root.kind == TkqNodeKind::or_node);
    CHECK(root.children.size() == 2U);
    CHECK(compiled->nodes[root.children.front()].kind == TkqNodeKind::and_node);
    CHECK(compiled->nodes[root.children.back()].kind == TkqNodeKind::not_node);
    CHECK(compiled->predicates.size() == 3U);
    CHECK(compiled->predicates[0].field == "genre");
    CHECK(compiled->predicates[0].comparison == TkqComparison::has);
    CHECK(compiled->predicates[1].field == "date");
    CHECK(compiled->predicates[1].comparison == TkqComparison::greater);
    CHECK(compiled->predicates[1].number == 1990);
    CHECK(compiled->predicates[2].comparison == TkqComparison::is);
    CHECK(compiled->predicates[2].text == "Miles Davis");
    CHECK(compiled->predicates[2].normalized == "miles davis");
    CHECK(compiled->field_dependencies() == (std::vector<std::string>{"genre", "date", "artist"}));

    // Parentheses override precedence.
    const auto grouped = compile_tkq("genre HAS jazz AND (date GREATER 1990 OR date MISSING)");
    CHECK(grouped.has_value());
    CHECK(grouped && grouped->nodes[grouped->root].kind == TkqNodeKind::and_node);
}

void quotingEscapesAndKeywordsInsideStrings() {
    const auto compiled = compile_tkq("title IS \"AND \"\"quoted\"\" OR\"");
    CHECK(compiled.has_value());
    CHECK(compiled && compiled->predicates.front().text == "AND \"quoted\" OR");

    const auto unterminated = compile_tkq("title IS \"open");
    CHECK(!unterminated.has_value());

    // A quoted operand is a field name even if it carries spaces.
    const auto spaced_field = compile_tkq("\"my field\" PRESENT");
    CHECK(spaced_field.has_value());
    CHECK(spaced_field && spaced_field->predicates.front().field == "my field");
}

void expressionOperandsCompileAsFormatPredicates() {
    const auto compiled = compile_tkq("\"%artist% - %title%\" HAS blue");
    CHECK(compiled.has_value());
    if (compiled) {
        CHECK(compiled->predicates.front().operand == TkqOperandKind::expression);
        CHECK(compiled->programs.size() == 1U);
    }
    const auto invalid = compile_tkq("\"$if(%a%\" HAS x");
    CHECK(!invalid.has_value());
}

void sortClauseSplitsFromTheExpression() {
    const auto compiled = compile_tkq("genre IS jazz SORT DESCENDING BY %album% - %title%");
    CHECK(compiled.has_value());
    if (!compiled) {
        return;
    }
    CHECK(compiled->sort.has_value());
    CHECK(compiled->sort && compiled->sort->direction == TkqSortDirection::descending);
    CHECK(compiled->sort && compiled->sort->source == "%album% - %title%");
    CHECK(compiled->predicates.size() == 1U);

    const auto plain = compile_tkq("ALL SORT BY %album%");
    CHECK(plain.has_value());
    CHECK(plain && plain->match_all && plain->sort.has_value() &&
          plain->sort->direction == TkqSortDirection::ascending);

    CHECK(!compile_tkq("genre IS jazz SORT BY").has_value());
    CHECK(!compile_tkq("genre IS jazz SORT %album%").has_value());
}

void strictErrorsNeverDegradeToWordSearch() {
    // A reserved keyword makes the query structured; malformed structure
    // is a hard error rather than a silent word search.
    CHECK(!compile_tkq("jazz AND").has_value());
    CHECK(!compile_tkq("genre HAS").has_value());
    CHECK(!compile_tkq("genre GREATER abc").has_value());
    CHECK(!compile_tkq("(genre IS jazz").has_value());
    CHECK(!compile_tkq("AND genre IS jazz").has_value());
    CHECK(!compile_tkq("* IS jazz").has_value());
    CHECK(!compile_tkq("* PRESENT").has_value());
    CHECK(!compile_tkq("ALL genre IS jazz").has_value());
    CHECK(!compile_tkq("").has_value());
    CHECK(!compile_tkq("   ").has_value());
    CHECK(!compile_tkq("genre IS jazz extra").has_value());

    // Diagnostics carry the offending span.
    const auto failed = compile_tkq("genre GREATER abc");
    CHECK(!failed.has_value());
    if (!failed) {
        bool has_span = false;
        for (const auto& entry : failed.error().context) {
            has_span = has_span || entry.key == "span_begin";
        }
        CHECK(has_span);
    }
}

void boundsFailClosed() {
    trackknife::query::TkqLimits limits;
    limits.maximum_source_bytes = 16U;
    CHECK(!compile_tkq("genre IS jazz AND artist IS someone", limits).has_value());

    limits = {};
    limits.maximum_nodes = 2U;
    CHECK(!compile_tkq("a IS b AND c IS d AND e IS f", limits).has_value());

    limits = {};
    limits.maximum_words = 2U;
    CHECK(!compile_tkq("one two three", limits).has_value());
}

} // namespace

int main() {
    simpleWordsBecomeAnAllWordSearch();
    structuredQueriesParseWithPrecedence();
    quotingEscapesAndKeywordsInsideStrings();
    expressionOperandsCompileAsFormatPredicates();
    sortClauseSplitsFromTheExpression();
    strictErrorsNeverDegradeToWordSearch();
    boundsFailClosed();
    return failures == 0 ? 0 : 1;
}
