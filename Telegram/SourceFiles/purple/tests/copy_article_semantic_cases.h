#pragma once

#include "purple/purple_article_markdown.h"

#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QXmlStreamReader>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtGui/QTextList>

#include <vector>

namespace {

struct SemanticXmlNode {
	QString name;
	QXmlStreamAttributes attributes;
	QString text;
	std::vector<SemanticXmlNode> children;
};

SemanticXmlNode ReadSemanticXmlNode(QXmlStreamReader &reader, int depth) {
	auto result = SemanticXmlNode{ reader.name().toString(), reader.attributes() };
	if (depth > 64) {
		reader.raiseError(QString::fromUtf8("Fixture XML nesting limit"));
		return result;
	}
	while (!reader.atEnd()) {
		const auto token = reader.readNext();
		if (token == QXmlStreamReader::StartElement) {
			auto child = ReadSemanticXmlNode(reader, depth + 1);
			result.text += child.text;
			result.children.push_back(std::move(child));
		} else if (token == QXmlStreamReader::Characters) {
			result.text += reader.text();
		} else if (token == QXmlStreamReader::EndElement) {
			break;
		}
	}
	return result;
}

struct SemanticListSpec {
	QString text = QString::fromUtf8("Anchored item");
	QString listAnchor = QString::fromUtf8("list-anchor");
	QString itemAnchor = QString::fromUtf8("item-anchor");
};

struct SemanticArticleSpec {
	QString code = QString::fromUtf8("literal code");
	QString language;
	std::vector<SemanticListSpec> lists = { SemanticListSpec() };
};

bool StrictSemanticArticleCode(
		const QString &markdown,
		const SemanticArticleSpec &expected = {},
		QString *diagnostic = nullptr) {
	const auto q = [](const char *value) { return QString::fromUtf8(value); };
	auto imported = QTextDocument();
	imported.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
	const auto reject = [&](const QString &message) {
		if (diagnostic) *diagnostic = message;
		return false;
	};
	auto reader = QXmlStreamReader(markdown);
	if (!reader.readNextStartElement()) return reject(q("missing balanced source island"));
	const auto root = ReadSemanticXmlNode(reader, 0);
	while (!reader.atEnd()) reader.readNext();
	if (reader.hasError() || root.name != q("div")) return reject(q("source island is not balanced XML"));
	if (root.children.size() != 2 * (expected.lists.size() + 3)) return reject(q("source sibling count changed"));
	auto idCounts = QMap<QString, int>();
	auto nameCounts = QMap<QString, int>();
	const auto visit = [&](const auto &self, const SemanticXmlNode &node) -> void {
		if (node.attributes.hasAttribute(q("id"))) ++idCounts[node.attributes.value(q("id")).toString()];
		if (node.attributes.hasAttribute(q("name"))) ++nameCounts[node.attributes.value(q("name")).toString()];
		for (const auto &child : node.children) self(self, child);
	};
	visit(visit, root);
	auto requiredIds = QSet<QString>{ q("heading-anchor"), q("code-anchor"), q("quote-anchor") };
	for (const auto &list : expected.lists) {
		requiredIds.insert(list.listAnchor);
		requiredIds.insert(list.itemAnchor);
	}
	if (idCounts.size() != requiredIds.size() || nameCounts.size() != requiredIds.size()) return reject(q("anchor cardinality changed"));
	for (const auto &id : requiredIds) {
		if (idCounts.value(id) != 1 || nameCounts.value(id) != 1) return reject(q("missing or duplicate source anchor"));
	}
	const auto anchor = [&](const SemanticXmlNode &node, const QString &id) {
		return node.name == q("a") && node.text.isEmpty() && node.children.empty()
			&& node.attributes.value(q("id")) == id && node.attributes.value(q("name")) == id;
	};
	if (!anchor(root.children[0], q("heading-anchor")) || root.children[1].name != q("h3")
		|| root.children[1].text != q("Anchored heading")) return reject(q("heading anchor or content changed"));
	auto sourceIndex = size_t(2);
	for (const auto &list : expected.lists) {
		const auto &container = root.children[sourceIndex + 1];
		if (!anchor(root.children[sourceIndex], list.listAnchor) || container.name != q("ul")
			|| container.children.size() != 1 || container.children.front().name != q("li")) return reject(q("list sibling or item cardinality changed"));
		const auto &item = container.children.front();
		if (item.children.size() != 2 || !anchor(item.children[0], list.itemAnchor)
			|| item.children[1].name != q("p") || item.text != list.text) return reject(q("list anchor/content association changed"));
		sourceIndex += 2;
	}
	const auto &pre = root.children[sourceIndex + 1];
	if (!anchor(root.children[sourceIndex], q("code-anchor")) || pre.name != q("pre")
		|| pre.children.size() != 1 || pre.children[0].name != q("code")
		|| !pre.children[0].children.empty() || pre.text != expected.code
		|| pre.children[0].text != expected.code
		|| pre.children[0].attributes.value(q("class")) != q("language-") + expected.language) return reject(q("semantic code source/text/language/anchor changed"));
	const auto &quote = root.children[sourceIndex + 3];
	if (!anchor(root.children[sourceIndex + 2], q("quote-anchor")) || quote.name != q("blockquote")
		|| quote.children.size() != 1 || quote.children[0].name != q("p")
		|| quote.text != q("Anchored quote")) return reject(q("quote node/content/anchor changed"));

	auto positions = QMap<QString, QSet<int>>();
	for (auto block = imported.begin(); block.isValid(); block = block.next()) {
		auto names = QSet<QString>();
		for (const auto &name : block.charFormat().anchorNames()) names.insert(name);
		for (auto it = block.begin(); !it.atEnd(); ++it) {
			const auto fragment = it.fragment();
			if (!fragment.isValid()) continue;
			for (const auto &name : fragment.charFormat().anchorNames()) names.insert(name);
		}
		for (const auto &name : names) positions[name].insert(block.blockNumber());
	}
	if (positions.size() != requiredIds.size()) return reject(q("imported anchor cardinality changed"));
	const auto at = [&](const QString &id, int index) {
		return positions.value(id) == QSet<int>{ index };
	};
	auto block = imported.begin();
	if (!block.isValid() || block.text() != q("Anchored heading") || block.blockFormat().headingLevel() != 3
		|| block.textList() || !at(q("heading-anchor"), block.blockNumber())) return reject(q("imported heading/anchor changed"));
	block = block.next();
	auto listIdentities = QSet<const QTextList*>();
	for (const auto &list : expected.lists) {
		if (!block.isValid() || block.text() != list.text || !block.textList()
			|| block.textList()->count() != 1 || block.textList()->format().style() != QTextListFormat::ListDisc
			|| listIdentities.contains(block.textList()) || !at(list.listAnchor, block.blockNumber())
			|| !at(list.itemAnchor, block.blockNumber())) return reject(q("imported list order/separation/content/anchors changed"));
		listIdentities.insert(block.textList());
		block = block.next();
	}
	const auto codeStart = block.blockNumber();
	auto codeParts = QStringList();
	while (block.isValid() && block.blockFormat().nonBreakableLines()) {
		if (block.textList() || block.blockFormat().headingLevel()
			|| block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)) return reject(q("code absorbed into list/quote/heading"));
		const auto language = block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
		if (!language.isEmpty() && language != expected.language) return reject(q("imported language changed"));
		for (auto it = block.begin(); !it.atEnd(); ++it) {
			const auto fragment = it.fragment();
			if (fragment.isValid() && !fragment.text().isEmpty()
				&& !fragment.charFormat().fontFamilies().toStringList().contains(q("monospace"))) return reject(q("code presentation lost"));
		}
		codeParts.push_back(block.text());
		block = block.next();
	}
	auto code = codeParts.join('\n');
	code.replace(QChar::ParagraphSeparator, QChar('\n'));
	if (codeParts.empty() || code != expected.code || !at(q("code-anchor"), codeStart)) return reject(q("imported exact code range/start anchor changed: expected=%1 actual=%2 start=%3 positions=%4").arg(QString::fromLatin1(expected.code.toUtf8().toHex()), QString::fromLatin1(code.toUtf8().toHex())).arg(codeStart).arg(positions.value(q("code-anchor")).size()));
	if (!block.isValid() || block.text() != q("Anchored quote") || block.textList()
		|| block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) != 1
		|| !at(q("quote-anchor"), block.blockNumber()) || block.next().isValid()) return reject(q("imported quote/sibling/anchor changed"));
	return true;
}

Iv::RichPage SemanticArticleFixture(const SemanticArticleSpec &spec) {
	using Page = Iv::RichPage;
	const auto q = [](const char *value) { return QString::fromUtf8(value); };
	auto page = Page();
	auto heading = Page::Block(); heading.kind = Page::BlockKind::Heading; heading.headingLevel = 3;
	heading.anchorId = q("heading-anchor"); heading.text.text.text = q("Anchored heading"); page.blocks.push_back(heading);
	for (const auto &expected : spec.lists) {
		auto list = Page::Block(); list.kind = Page::BlockKind::List; list.anchorId = expected.listAnchor;
		auto item = Page::ListItem(); item.anchorId = expected.itemAnchor; item.text.text.text = expected.text;
		list.listItems.push_back(item); page.blocks.push_back(list);
	}
	auto code = Page::Block(); code.kind = Page::BlockKind::Code; code.anchorId = q("code-anchor");
	code.language = spec.language; code.text.text.text = spec.code; page.blocks.push_back(code);
	auto quote = Page::Block(); quote.kind = Page::BlockKind::Quote; quote.anchorId = q("quote-anchor");
	quote.text.text.text = q("Anchored quote"); page.blocks.push_back(quote);
	return page;
}

} // namespace
