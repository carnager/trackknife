// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/panel_layout.hpp"

#include <QtTest>

#include <vector>

namespace trackknife::ui {

class PanelLayoutTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsNestedComposition();
    void rejectsInvalidComposition_data();
    void rejectsInvalidComposition();
};

void PanelLayoutTest::roundTripsNestedComposition() {
    std::vector<PanelLayoutNode> split_children;
    split_children.push_back(panelLayoutPanel(QStringLiteral("lists")));
    split_children.push_back(panelLayoutPanel(QStringLiteral("details")));
    std::vector<PanelLayoutNode> tab_children;
    tab_children.push_back(panelLayoutPanel(QStringLiteral("folders")));
    tab_children.push_back(panelLayoutSplit(Qt::Vertical, std::move(split_children), {3, 1}));
    const PanelLayout expected{
        .schema_version = panel_layout_schema_version,
        .root = panelLayoutTabs(std::move(tab_children), 1),
    };

    const auto encoded = serializePanelLayout(expected);
    QString error;
    const auto decoded = deserializePanelLayout(
        encoded, {QStringLiteral("folders"), QStringLiteral("lists"), QStringLiteral("details")},
        &error);
    QVERIFY2(decoded.has_value(), qPrintable(error));
    QCOMPARE(*decoded, expected);
    QCOMPARE(serializePanelLayout(*decoded), encoded);
}

void PanelLayoutTest::rejectsInvalidComposition_data() {
    QTest::addColumn<QByteArray>("encoded");
    QTest::addColumn<QStringList>("registered");

    QTest::newRow("malformed") << QByteArray{"{"} << QStringList{QStringLiteral("folders")};
    QTest::newRow("future-schema")
        << QByteArray{R"({"schema":2,"root":{"kind":"panel","panel":"folders"}})"}
        << QStringList{QStringLiteral("folders")};
    QTest::newRow("unknown-panel")
        << QByteArray{R"({"schema":1,"root":{"kind":"panel","panel":"unknown"}})"}
        << QStringList{QStringLiteral("folders")};
    QTest::newRow("duplicate-panel")
        << QByteArray{R"({"schema":1,"root":{"kind":"split","orientation":"horizontal","weights":[1,1],"children":[{"kind":"panel","panel":"folders"},{"kind":"panel","panel":"folders"}]}})"}
        << QStringList{QStringLiteral("folders")};
    QTest::newRow("incomplete")
        << QByteArray{R"({"schema":1,"root":{"kind":"panel","panel":"folders"}})"}
        << QStringList{QStringLiteral("folders"), QStringLiteral("lists")};
    QTest::newRow("bad-weight")
        << QByteArray{R"({"schema":1,"root":{"kind":"split","orientation":"horizontal","weights":[1,0],"children":[{"kind":"panel","panel":"folders"},{"kind":"panel","panel":"lists"}]}})"}
        << QStringList{QStringLiteral("folders"), QStringLiteral("lists")};
    QTest::newRow("bad-active-tab")
        << QByteArray{R"({"schema":1,"root":{"kind":"tabs","active":2,"children":[{"kind":"panel","panel":"folders"}]}})"}
        << QStringList{QStringLiteral("folders")};

    auto too_deep = panelLayoutPanel(QStringLiteral("folders"));
    for (int depth = 0; depth < 18; ++depth) {
        std::vector<PanelLayoutNode> child;
        child.push_back(std::move(too_deep));
        too_deep = panelLayoutTabs(std::move(child));
    }
    QTest::newRow("too-deep") << serializePanelLayout(
                                     PanelLayout{.schema_version = panel_layout_schema_version,
                                                 .root = std::move(too_deep)})
                              << QStringList{QStringLiteral("folders")};
}

void PanelLayoutTest::rejectsInvalidComposition() {
    QFETCH(QByteArray, encoded);
    QFETCH(QStringList, registered);
    QString error;
    QVERIFY(!deserializePanelLayout(encoded, registered, &error).has_value());
    QVERIFY(!error.isEmpty());
}

} // namespace trackknife::ui

QTEST_APPLESS_MAIN(trackknife::ui::PanelLayoutTest)

#include "panel_layout_test.moc"
