// SPDX-License-Identifier: GPL-3.0-only

#include "ui/format_sandbox.hpp"

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::ui {
namespace {

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

struct ParsedFields {
    using Values = std::vector<std::string>;
    std::vector<std::pair<std::string, Values>> metadata;
    std::vector<std::pair<std::string, std::string>> information;
    std::optional<QString> error;
};

void parseLines(const QString& text, const bool multi_value, ParsedFields& output) {
    const auto lines = text.split(QLatin1Char('\n'));
    for (qsizetype line_index = 0; line_index < lines.size(); ++line_index) {
        const auto& line = lines.at(line_index);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const auto separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            output.error = QStringLiteral("Line %1 must use name=value").arg(line_index + 1);
            return;
        }
        const auto name = asciiLower(line.left(separator).trimmed().toStdString());
        const auto value = line.mid(separator + 1).toStdString();
        if (multi_value) {
            const auto existing = std::ranges::find(
                output.metadata, name,
                [](const auto& item) -> const std::string& { return item.first; });
            if (existing == output.metadata.end()) {
                output.metadata.emplace_back(name, ParsedFields::Values{value});
            } else {
                existing->second.push_back(value);
            }
        } else {
            output.information.emplace_back(name, value);
        }
    }
}

class SandboxContext final : public trackknife::titleformat::EvaluationContext {
  public:
    SandboxContext(const FormatContextKind kind, ParsedFields values)
        : kind_(kind), values_(std::move(values)) {}

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
        const auto match =
            std::ranges::find(values_.metadata, normalized,
                              [](const auto& item) -> const std::string& { return item.first; });
        return match == values_.metadata.end() ? std::nullopt
                                               : std::optional<MetadataValues>{match->second};
    }

    [[nodiscard]] std::optional<std::string>
    resolveTechnicalInfo(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match =
            std::ranges::find(values_.information, normalized,
                              [](const auto& item) -> const std::string& { return item.first; });
        return match == values_.information.end() ? std::nullopt
                                                  : std::optional<std::string>{match->second};
    }

  private:
    FormatContextKind kind_;
    ParsedFields values_;
};

[[nodiscard]] QString dependencyLine(const QString& label,
                                     const std::vector<std::string>& dependencies) {
    QStringList values;
    for (const auto& dependency : dependencies) {
        values.push_back(QString::fromStdString(dependency));
    }
    return label + values.join(QStringLiteral(", "));
}

} // namespace

FormatSandboxDialog::FormatSandboxDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("format-sandbox"));
    setWindowTitle(QStringLiteral("Trackknife Format Expression Sandbox"));
    resize(900, 680);

    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        QStringLiteral("Live tkfmt-1 preview. Repeat a metadata name for ordered multi-values. "
                       "Expressions are read-only; path safety is applied later by the operation "
                       "planner."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* form = new QFormLayout;
    host_ = new QComboBox(this);
    host_->setObjectName(QStringLiteral("format-host"));
    host_->addItem(QStringLiteral("Track / view column"),
                   static_cast<int>(FormatContextKind::track_display));
    host_->addItem(QStringLiteral("Queue column"), static_cast<int>(FormatContextKind::queue));
    host_->addItem(QStringLiteral("Library tree level"),
                   static_cast<int>(FormatContextKind::tree_level));
    host_->addItem(QStringLiteral("Conversion relative path"),
                   static_cast<int>(FormatContextKind::path_generation));
    form->addRow(QStringLiteral("Host:"), host_);

    source_ = new QPlainTextEdit(this);
    source_->setObjectName(QStringLiteral("format-source"));
    source_->setMaximumHeight(100);
    source_->setPlainText(QStringLiteral(
        "$if2(%albumartist%,%artist%)/%album%/$num(%tracknumber%,2) - %title%.flac"));
    form->addRow(QStringLiteral("Expression:"), source_);
    layout->addLayout(form);

    auto* inputs = new QSplitter(Qt::Horizontal, this);
    auto* metadata_group = new QGroupBox(QStringLiteral("Metadata (name=value)"), inputs);
    auto* metadata_layout = new QVBoxLayout(metadata_group);
    fields_ = new QPlainTextEdit(metadata_group);
    fields_->setObjectName(QStringLiteral("format-fields"));
    fields_->setPlaceholderText(QStringLiteral("name=value; repeat names for multi-values"));
    fields_->setPlainText(QStringLiteral("artist=Talk Talk\n"
                                         "albumartist=Talk Talk\n"
                                         "album=Spirit of Eden\n"
                                         "tracknumber=2\n"
                                         "title=Eden\n"
                                         "genre=Art Rock\n"
                                         "genre=Post-Rock\n"
                                         "queue_position=3"));
    metadata_layout->addWidget(fields_);

    auto* information_group =
        new QGroupBox(QStringLiteral("Technical information (name=value)"), inputs);
    auto* information_layout = new QVBoxLayout(information_group);
    information_ = new QPlainTextEdit(information_group);
    information_->setObjectName(QStringLiteral("format-information"));
    information_->setPlaceholderText(QStringLiteral("technical_name=value"));
    information_->setPlainText(QStringLiteral("codec=FLAC\nsample_rate=44100"));
    information_layout->addWidget(information_);
    inputs->addWidget(metadata_group);
    inputs->addWidget(information_group);
    layout->addWidget(inputs, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("format-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    preview_ = new QPlainTextEdit(this);
    preview_->setObjectName(QStringLiteral("format-preview"));
    preview_->setReadOnly(true);
    layout->addWidget(preview_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    connect(host_, &QComboBox::currentIndexChanged, this, [this](const int) { updatePreview(); });
    connect(source_, &QPlainTextEdit::textChanged, this, &FormatSandboxDialog::updatePreview);
    connect(fields_, &QPlainTextEdit::textChanged, this, &FormatSandboxDialog::updatePreview);
    connect(information_, &QPlainTextEdit::textChanged, this, &FormatSandboxDialog::updatePreview);
    updatePreview();
}

void FormatSandboxDialog::updatePreview() {
    ParsedFields parsed;
    parseLines(fields_->toPlainText(), true, parsed);
    if (!parsed.error) {
        parseLines(information_->toPlainText(), false, parsed);
    }
    if (parsed.error) {
        status_->setText(*parsed.error);
        preview_->clear();
        return;
    }

    const auto host = static_cast<FormatContextKind>(host_->currentData().toInt());
    trackknife::titleformat::CompileOptions options;
    options.context = host;
    const auto compiled =
        trackknife::titleformat::compile(source_->toPlainText().toStdString(), options);
    if (!compiled.program) {
        QStringList diagnostics;
        for (const auto& diagnostic : compiled.parse_diagnostics) {
            diagnostics.push_back(QStringLiteral("[%1,%2) %3")
                                      .arg(diagnostic.span.begin)
                                      .arg(diagnostic.span.end)
                                      .arg(QString::fromStdString(diagnostic.message)));
        }
        for (const auto& diagnostic : compiled.diagnostics) {
            diagnostics.push_back(QStringLiteral("[%1,%2) %3")
                                      .arg(diagnostic.span.begin)
                                      .arg(diagnostic.span.end)
                                      .arg(QString::fromStdString(diagnostic.message)));
        }
        status_->setText(QStringLiteral("Cannot evaluate"));
        preview_->setPlainText(diagnostics.join(QLatin1Char('\n')));
        return;
    }

    const SandboxContext context(host, std::move(parsed));
    QStringList output;
    if (compiled.program->hasExpansions()) {
        const auto values = trackknife::titleformat::evaluateExpanded(*compiled.program, context);
        if (!values) {
            status_->setText(QStringLiteral("Evaluation failed"));
            preview_->setPlainText(QString::fromStdString(values.error().message));
            return;
        }
        for (std::size_t index = 0; index < values->size(); ++index) {
            output.push_back(QStringLiteral("[%1] %2").arg(index).arg(
                QString::fromStdString(values->at(index).text)));
        }
    } else {
        const auto value = trackknife::titleformat::evaluate(*compiled.program, context);
        if (!value) {
            status_->setText(QStringLiteral("Evaluation failed"));
            preview_->setPlainText(QString::fromStdString(value.error().message));
            return;
        }
        output.push_back(QString::fromStdString(value->text));
    }

    status_->setText(QStringLiteral("Valid tkfmt-1 expression — %1 result(s)").arg(output.size()));
    output.push_back(QString{});
    output.push_back(
        dependencyLine(QStringLiteral("Fields: "), compiled.program->fieldDependencies()));
    output.push_back(
        dependencyLine(QStringLiteral("Technical: "), compiled.program->technicalDependencies()));
    output.push_back(
        dependencyLine(QStringLiteral("Expanded: "), compiled.program->expansionDependencies()));
    preview_->setPlainText(output.join(QLatin1Char('\n')));
}

} // namespace trackknife::ui
