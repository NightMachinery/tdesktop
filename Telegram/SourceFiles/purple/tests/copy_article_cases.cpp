#include "purple/purple_article_markdown.h"
#include "purple/purple_copy.h"
#include "iv/markdown/iv_markdown_prepare_serialize.h"
#include "ui/text/text_entity.h"
#include "purple/tests/copy_article_semantic_cases.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QTextDocument>
#include <QtGui/QTextBlock>
#include <QtGui/QTextFragment>
#include <QtGui/QTextList>

#include <limits>

#include <iostream>

int main(int argc, char **argv) {
	auto app = QGuiApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	const auto q = [](const char *value) { return QString::fromUtf8(value); };
	const auto ordinary = [&](const QString &source) {
		const auto markdown = Purple::CopyTextAsMarkdown(TextForMimeData::Simple(source));
		auto roundtrip = QTextDocument();
		roundtrip.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
		if (roundtrip.toPlainText() != source) {
			std::cerr << "source=" << source.toUtf8().toHex().constData()
				<< " markdown=" << markdown.toUtf8().toHex().constData()
				<< " roundtrip=" << roundtrip.toPlainText().toUtf8().toHex().constData() << '\n';
		}
		expect(roundtrip.toPlainText() == source, "ordinary whitespace and punctuation roundtrip");
		return markdown;
	};
	expect(ordinary(q("alpha  beta")).contains(q("alpha  beta")), "repeated literal spaces");
	ordinary(q("  leading spaces"));
	expect(ordinary(q("alpha\tbeta")).contains('\t'), "literal tab");
	ordinary(q("first\n\n\nlast"));
	ordinary(q("# heading-like * literal [brackets] <xml> & \\"));
	const auto styled = TextForMimeData::Rich({ q("one  two and code`tick"), {
		EntityInText(EntityType::Bold, 0, 8),
		EntityInText(EntityType::Code, 13, 9),
	} });
	auto styledDoc = QTextDocument();
	styledDoc.setMarkdown(Purple::CopyTextAsMarkdown(styled), QTextDocument::MarkdownDialectGitHub);
	expect(styledDoc.toPlainText() == styled.rich.text, "mixed real entity whitespace roundtrip");
	using Page = Iv::RichPage;
	using Kind = Page::BlockKind;
	const auto rich = [&](const char *text) { auto result = Page::RichText(); result.text.text = q(text); return result; };
	const auto paragraph = [&](const char *text) { auto result = Page::Block(); result.kind = Kind::Paragraph; result.text = rich(text); return result; };
	auto page = Page();
	for (auto level = 1; level != 7; ++level) {
		auto heading = paragraph("Heading"); heading.kind = Kind::Heading; heading.headingLevel = level;
		page.blocks.push_back(heading);
	}
	auto quote = paragraph("quotation"); quote.kind = Kind::Quote; quote.caption = rich("credit"); quote.blocks.push_back(paragraph("quote child"));
	page.blocks.push_back(quote);
	auto list = Page::Block(); list.kind = Kind::List; list.listKind = Page::ListKind::Ordered;
	list.orderedList.start = 7; list.orderedList.reversed = true; list.orderedList.type = q("i");
	auto item = Page::ListItem(); item.number.num = q("vii<&"); item.taskState = Page::TaskState::Checked;
	item.text = rich("list text"); item.blocks = { paragraph("first child"), paragraph("second child") }; list.listItems.push_back(item);
	page.blocks.push_back(list);
	auto table = Page::Block(); table.kind = Kind::Table; table.text = rich("Table title");
	auto cell = Page::TableCell(); cell.header = true; cell.colspan = 2; cell.rowspan = 3;
	cell.alignment = Page::TableAlignment::Right; cell.text = rich("a|b\nc"); table.tableRows.push_back({ { cell } });
	page.blocks.push_back(table);
	auto details = paragraph("closed details"); details.kind = Kind::Details; details.open = false; details.blocks = { paragraph("hidden child"), table };
	page.blocks.push_back(details);
	auto code = paragraph("```\n````\n<&>"); code.kind = Kind::Code; code.language = q("cpp"); page.blocks.push_back(code);
	auto math = Page::Block(); math.kind = Kind::Math; math.formula = q("x^2 + y^2"); page.blocks.push_back(math);
	auto photo = Page::Block(); photo.kind = Kind::Photo; photo.photoId = 123; photo.caption = rich("Photo caption and credit"); page.blocks.push_back(photo);
	auto group = Page::Block(); group.kind = Kind::GroupedMedia;
	group.mediaItems.push_back({ .kind = Kind::Photo, .photoId = 42 });
	group.mediaItems.push_back({ .kind = Kind::Video, .documentId = 43 }); group.caption = rich("gallery caption"); page.blocks.push_back(group);
	auto embed = Page::Block(); embed.kind = Kind::Embed; embed.html = QByteArray("<span>raw embed</span>"); embed.caption = rich("embed caption"); page.blocks.push_back(embed);
	auto inlineMath = paragraph("a");
	const auto object = Iv::Markdown::SerializeInlineTextObjectEntity({
		.kind = Iv::Markdown::InlineTextObjectKind::Formula,
		.data = Iv::Markdown::InlineTextObjectFormulaData{ q("$z$"), q("z") },
	});
	inlineMath.text.text.text = QString(QChar::ObjectReplacementCharacter);
	inlineMath.text.text.entities.push_back(EntityInText(EntityType::CustomEmoji, 0, 1, object)); page.blocks.push_back(inlineMath);
	auto footer = paragraph("footer body"); footer.kind = Kind::Footer; page.blocks.push_back(footer);
	auto byline = paragraph("article author"); byline.kind = Kind::AuthorDate; byline.date = 1; page.blocks.push_back(byline);
	auto thinking = paragraph("thinking body"); thinking.kind = Kind::Thinking; thinking.blocks = { paragraph("thinking child") }; page.blocks.push_back(thinking);
	page.blocks.push_back({ .kind = Kind::Divider });
	auto anchorBlock = Page::Block(); anchorBlock.kind = Kind::Anchor; anchorBlock.anchorId = q("article-start"); page.blocks.push_back(anchorBlock);
	auto bullet = Page::Block(); bullet.kind = Kind::List;
	auto bulletItem = Page::ListItem(); bulletItem.text = rich("bullet parent");
	auto nested = Page::Block(); nested.kind = Kind::List; nested.listKind = Page::ListKind::Ordered; nested.orderedList.start = 4;
	auto nestedItem = Page::ListItem(); nestedItem.text = rich("nested ordered child"); nested.listItems.push_back(nestedItem);
	bulletItem.blocks = { paragraph("bullet second paragraph"), nested }; bullet.listItems.push_back(bulletItem); page.blocks.push_back(bullet);
	auto video = Page::Block(); video.kind = Kind::Video; video.caption = rich("video caption"); page.blocks.push_back(video);
	auto audio = Page::Block(); audio.kind = Kind::Audio; audio.audioTitle = q("audio title"); audio.audioPerformer = q("performer");
	audio.audioFileName = q("example.ogg"); audio.audioDuration = 77; audio.caption = rich("audio credit"); page.blocks.push_back(audio);
	auto post = paragraph("embedded post"); post.kind = Kind::EmbedPost; post.author = q("embedded author"); post.date = 1;
	post.photoId = 7; post.blocks = { paragraph("embedded child") }; post.caption = rich("embedded credit"); page.blocks.push_back(post);
	auto channel = Page::Block(); channel.kind = Kind::Channel; channel.channelTitle = q("channel title"); page.blocks.push_back(channel);
	auto related = paragraph("Related"); related.kind = Kind::RelatedArticles;
	related.relatedArticles.push_back({ .url = q("https://example.org/related"), .photoId = 77,
		.title = q("related title"), .description = q("related description"), .author = q("related author"), .publishedDate = 1 }); page.blocks.push_back(related);
	auto map = Page::Block(); map.kind = Kind::Map; map.latitude = 12.5; map.longitude = 45.25; map.caption = rich("map caption"); page.blocks.push_back(map);
	auto linkText = paragraph("documentation"); linkText.text.text.entities.push_back(EntityInText(EntityType::CustomUrl,
		0, 13, Iv::EncodeRichPageLinkUrl(q("https://example.org/a_(b)"), 42))); page.blocks.push_back(linkText);
	auto inlineImage = paragraph("placeholder"); inlineImage.text.text.text = QString(QChar::ObjectReplacementCharacter);
	inlineImage.text.text.entities.push_back(EntityInText(EntityType::CustomEmoji, 0, 1,
		Iv::Markdown::SerializeInlineTextObjectEntity({ .kind = Iv::Markdown::InlineTextObjectKind::IvImage,
			.data = Iv::Markdown::InlineTextObjectIvImageData{ 99, 10, 10, q("inline image") } })));
	page.blocks.push_back(inlineImage);
	const auto output = Purple::RenderArticleMarkdown(page, q("https://example.org/source"));
	expect(output.error.isEmpty(), "complete supported article");
	expect(output.text.contains(q("###### Heading")), "all heading levels");
	expect(output.text.contains(q("> quote child")) && output.text.contains(q("> credit")), "quote children and attribution");
	expect(output.text.contains(q("start=\"7\"")) && output.text.contains(q(" reversed")), "ordered metadata");
	expect(output.text.contains(q("vii&lt;&amp;")) && output.text.contains(q("second child")), "arbitrary numbering and all children");
	expect(output.text.contains(q("[x]")), "task state");
	expect(output.text.contains(q("colspan=\"2\"")) && output.text.contains(q("rowspan=\"3\"")), "merged table spans");
	expect(output.text.contains(q("<th ")) && output.text.contains(q("Table title")), "header identity and table title");
	expect(output.text.contains(q("<details><summary>")) && output.text.contains(q("hidden child")), "closed details full body");
	expect(output.text.contains(q("`````cpp")), "safe code fence");
	expect(output.text.contains(q("x^2 + y^2")) && output.text.contains(q("$z$")), "block and inline math source");
	expect(output.text.contains(q("Photo caption and credit")) && output.text.contains(q("gallery caption")), "media captions");
	expect(!output.text.contains(q("tg://photo?id=")) && !output.text.contains(q("file://")), "no synthetic media URLs");
	expect(!output.text.contains(q("<span>raw embed</span>")) && output.text.contains(q("embed caption")), "raw embed is not injected");
	expect(output.text.contains(q("https://example.org/source")), "real supplied source permalink");
	expect(output.text.contains(q("<footer>")) && output.text.contains(q("footer body")), "footer role and body");
	expect(output.text.contains(q("article author")) && output.text.contains(q("1970-01-01T00:00:01")), "author and date");
	expect(output.text.contains(q("thinking child")) && output.text.contains(q("embedded child")), "thinking and embedded post children");
	expect(output.text.contains(q("id=\"article-start\"")), "anchor identity");
	expect(output.text.contains(q("bullet second paragraph")) && output.text.contains(q("4. nested ordered child")), "simple list paragraphs and nesting");
	expect(output.text.contains(q("video caption")) && output.text.contains(q("audio credit")) && output.text.contains(q("example.ogg")), "audio/video metadata and credit");
	expect(output.text.contains(q("related description")) && output.text.contains(q("https://example.org/related")), "related article structure");
	expect(output.text.contains(q("12.5")) && output.text.contains(q("map caption")), "map coordinates and caption");
	expect(!output.text.contains(q("internal:wrapped")) && output.text.contains(q("documentation")), "actual wrapped link codec");
	expect(output.text.contains(q("inline image")), "inline image safe placeholder");
	page.rtl = true; expect(Purple::RenderArticleMarkdown(page).text.contains(q("<article dir=\"rtl\">")), "RTL article direction");
	page.rtl = false;
	page.part = true; expect(!Purple::RenderArticleMarkdown(page).error.isEmpty(), "partial page refuses");
	page.part = false; page.blocks.push_back({ .kind = Kind::Unsupported });
	expect(!Purple::RenderArticleMarkdown(page).error.isEmpty(), "unsupported block refuses whole action");
	const auto firstCopy = Purple::BeginMarkdownCopy();
	expect(Purple::IsCurrentMarkdownCopy(firstCopy), "new production copy token is current");
	const auto ordinaryCopy = Purple::BeginMarkdownCopy();
	expect(!Purple::IsCurrentMarkdownCopy(firstCopy) && Purple::IsCurrentMarkdownCopy(ordinaryCopy),
		"later ordinary action invalidates pending article token");
	const auto otherWindowCopy = Purple::BeginMarkdownCopy();
	expect(!Purple::IsCurrentMarkdownCopy(ordinaryCopy) && !Purple::IsCurrentMarkdownCopy(firstCopy)
		&& Purple::IsCurrentMarkdownCopy(otherWindowCopy), "shared production generation spans contexts");
	expect(!Purple::IsCurrentMarkdownCopy({}), "empty generation never owns copy");
	auto originalPage = std::make_shared<Page>(); originalPage->blocks = { paragraph("Original heading") };
	auto translatedPage = std::make_shared<Page>(); translatedPage->blocks = { paragraph("Translated heading") };
	auto partialTranslation = std::make_shared<Page>(*translatedPage); partialTranslation->part = true;
	expect(Purple::SelectCompleteArticlePage(originalPage, originalPage, translatedPage, true) == translatedPage,
		"production selector chooses displayed translation");
	expect(!Purple::SelectCompleteArticlePage(originalPage, originalPage, partialTranslation, true),
		"partial displayed translation refuses original fallback");
	expect(!Purple::SelectCompleteArticlePage(originalPage, originalPage, {}, true),
		"missing displayed translation refuses original fallback");
	expect(Purple::SelectCompleteArticlePage(originalPage, originalPage, translatedPage, false) == originalPage,
		"unused translation preserves original tree");
	expect(Purple::RenderArticleMarkdown(*Purple::SelectCompleteArticlePage(
		originalPage, originalPage, translatedPage, true)).text.contains(q("Translated heading")),
		"selected translated tree exports translated content");
	const auto parsedAnchor = [&](const QTextDocument &doc, const QString &name) {
		for (auto block = doc.begin(); block.isValid(); block = block.next()) {
			if (block.charFormat().anchorNames().contains(name)) return true;
			for (auto it = block.begin(); !it.atEnd(); ++it) {
				const auto fragment = it.fragment();
				if (fragment.isValid() && fragment.charFormat().anchorNames().contains(name)) return true;
			}
		}
		return false;
	};
	auto anchored = Page();
	auto heading = paragraph("Anchored heading"); heading.kind = Kind::Heading; heading.headingLevel = 3;
	heading.anchorId = q("heading-anchor"); anchored.blocks.push_back(heading);
	auto anchoredList = Page::Block(); anchoredList.kind = Kind::List;
	auto anchoredItem = Page::ListItem(); anchoredItem.anchorId = q("item-anchor"); anchoredItem.text = rich("Anchored item");
	anchoredList.listItems.push_back(anchoredItem); anchoredList.anchorId = q("list-anchor"); anchored.blocks.push_back(anchoredList);
	auto anchoredCode = paragraph("literal code"); anchoredCode.kind = Kind::Code; anchoredCode.anchorId = q("code-anchor"); anchored.blocks.push_back(anchoredCode);
	auto anchoredQuote = paragraph("Anchored quote"); anchoredQuote.kind = Kind::Quote; anchoredQuote.anchorId = q("quote-anchor"); anchored.blocks.push_back(anchoredQuote);
	const auto anchoredOutput = Purple::RenderArticleMarkdown(anchored);
	auto anchorDoc = QTextDocument(); anchorDoc.setMarkdown(anchoredOutput.text, QTextDocument::MarkdownDialectGitHub);
	auto parsedHeading = false; auto parsedList = false; auto parsedCode = false; auto parsedQuote = false;
	for (auto block = anchorDoc.begin(); block.isValid(); block = block.next()) {
		if (block.text() == q("Anchored heading")) parsedHeading = block.blockFormat().headingLevel() == 3;
		if (block.text() == q("Anchored item")) parsedList = block.textList() != nullptr;
		if (block.text() == q("literal code")) parsedCode = block.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
		if (block.text() == q("Anchored quote")) parsedQuote = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0;
	}
	expect(anchoredOutput.error.isEmpty() && parsedHeading, "anchored heading retains parsed ATX structure");
	expect(parsedList, "anchored list item retains parsed list structure");
	std::cerr << "anchor-markdown=" << anchoredOutput.text.toUtf8().toHex().constData() << '\n';
	std::cerr << "anchor-document=" << anchorDoc.toHtml().toUtf8().toHex().constData() << '\n';
	auto codeDiagnostic = QString();
	const auto semanticCode = StrictSemanticArticleCode(anchoredOutput.text, {}, &codeDiagnostic);
	if (!semanticCode) std::cerr << "strict-code=" << codeDiagnostic.toUtf8().constData() << '\n';
	expect(semanticCode, "anchored code retains exact semantic structure content and start anchor");
	expect(parsedQuote, "anchored quote retains parsed quote structure");
	for (const auto &name : { q("heading-anchor"), q("list-anchor"), q("item-anchor"), q("code-anchor"), q("quote-anchor") }) {
		expect(parsedAnchor(anchorDoc, name), "real Markdown/HTML importer retains anchor identity");
	}
	const auto numbered = [&](int start, int count, const QString &type = {}) {
		auto result = Page(); auto block = Page::Block(); block.kind = Kind::List; block.listKind = Page::ListKind::Ordered;
		block.orderedList.start = start; if (!type.isEmpty()) block.orderedList.type = type;
		for (auto i = 0; i != count; ++i) { auto item = Page::ListItem(); item.text = rich("Numbered item"); block.listItems.push_back(item); }
		result.blocks.push_back(block); return result;
	};
	for (const auto start : { std::numeric_limits<int>::max(), -1, 1000000000 }) {
		const auto result = Purple::RenderArticleMarkdown(numbered(start, 1));
		expect(result.error.isEmpty() && result.text.contains(q("<ol start=\"%1\"").arg(start))
			&& !result.text.contains(q("%1. Numbered item").arg(start)), "out-of-domain ordered start uses faithful HTML");
	}
	const auto crossing = Purple::RenderArticleMarkdown(numbered(999999999, 2));
	expect(crossing.error.isEmpty() && crossing.text.contains(q("<ol start=\"999999999\"")),
		"nine-to-ten-digit sequence uses HTML before increment");
	const auto maximum = Purple::RenderArticleMarkdown(numbered(999999999, 1));
	expect(maximum.error.isEmpty() && maximum.text.contains(q("999999999. Numbered item")), "last valid nine-digit marker stays Markdown");
	const auto zero = Purple::RenderArticleMarkdown(numbered(0, 1));
	expect(zero.error.isEmpty() && zero.text.contains(q("0. Numbered item")), "zero marker is valid Markdown");
	for (const auto &pair : { std::pair{ q("lower-alpha"), q("a") }, std::pair{ q("lower-latin"), q("a") },
		std::pair{ q("upper-alpha"), q("A") }, std::pair{ q("upper-latin"), q("A") },
		std::pair{ q("lower-roman"), q("i") }, std::pair{ q("upper-roman"), q("I") } }) {
		auto typed = numbered(7, 1, pair.first); typed.blocks.front().orderedList.reversed = true;
		typed.blocks.front().listItems.front().number.type = pair.first;
		typed.blocks.front().listItems.front().number.value = 9;
		const auto result = Purple::RenderArticleMarkdown(typed);
		expect(result.error.isEmpty() && result.text.count(q("type=\"%1\"").arg(pair.second)) == 2
			&& result.text.contains(q("start=\"7\"")) && result.text.contains(q("value=\"9\""))
			&& result.text.contains(q(" reversed")), "native list/item aliases normalize and preserve numbering metadata");
	}

	const auto strictCase = [&](const SemanticArticleSpec &spec, const char *name) {
		const auto rendered = Purple::RenderArticleMarkdown(SemanticArticleFixture(spec));
		auto diagnostic = QString();
		const auto valid = rendered.error.isEmpty() && StrictSemanticArticleCode(rendered.text, spec, &diagnostic);
		if (!valid) std::cerr << "semantic-case=" << name << " reason=" << diagnostic.toUtf8().constData() << '\n';
		expect(valid, name);
		return rendered.text;
	};
	const auto semanticBase = strictCase({}, "strict semantic baseline with node-specific anchors");
	auto whitespaceSpec = SemanticArticleSpec(); whitespaceSpec.language = q("cpp");
	whitespaceSpec.code = q("  first\t  value\n\n    final");
	const auto whitespaceOutput = strictCase(whitespaceSpec, "exact leading repeated spaces tab and blank code lines");
	auto finalLineSpec = whitespaceSpec; finalLineSpec.code += '\n';
	strictCase(finalLineSpec, "exact final code line terminator");
	auto twoListSpec = SemanticArticleSpec();
	twoListSpec.lists.push_back({ q("Second item"), q("second-list-anchor"), q("second-item-anchor") });
	const auto twoListOutput = strictCase(twoListSpec, "distinct adjacent lists preserve cardinality and order");
	const auto rejectMutation = [&](QString changed, const QString &original,
			const SemanticArticleSpec &spec, const char *name) {
		expect(changed != original && !StrictSemanticArticleCode(changed, spec), name);
	};
	auto flattened = semanticBase;
	flattened.replace(q("<h3>"), q("<p>")); flattened.replace(q("</h3>"), q("</p>"));
	rejectMutation(flattened, semanticBase, {}, "reject flattened heading prose");
	auto absorption = semanticBase;
	absorption.replace(q("</li>&#10;</ul>"), QString());
	absorption.replace(q("</div>"), q("</li></ul></div>"));
	rejectMutation(absorption, semanticBase, {}, "reject balanced code and quote absorbed into list item");
	const auto oldFragments = q("<div><a id=\"heading-anchor\" name=\"heading-anchor\"></a><h3><span style=\"white-space:pre-wrap\">Anchored heading</span></h3></div>\n\n"
		"<div><a id=\"list-anchor\" name=\"list-anchor\"></a><ul>\n<li><a id=\"item-anchor\" name=\"item-anchor\"></a><span style=\"white-space:pre-wrap\">Anchored item</span></li>\n</ul></div>\n\n"
		"<div><a id=\"code-anchor\" name=\"code-anchor\"></a><pre><code class=\"language-\">literal code</code></pre></div>\n\n"
		"<div><a id=\"quote-anchor\" name=\"quote-anchor\"></a><blockquote><span style=\"white-space:pre-wrap\">Anchored quote</span></blockquote></div>");
	expect(!StrictSemanticArticleCode(oldFragments), "reject exact prior separately imported fragment regression");
	auto mergedLists = twoListOutput;
	mergedLists.replace(q("</ul><a id=\"second-list-anchor\" name=\"second-list-anchor\"></a><ul>"), QString());
	rejectMutation(mergedLists, twoListOutput, twoListSpec, "reject accidentally merged adjacent lists");
	auto reversedLists = twoListOutput;
	reversedLists.replace(q("Anchored item"), q("TEMP_ITEM"));
	reversedLists.replace(q("Second item"), q("Anchored item"));
	reversedLists.replace(q("TEMP_ITEM"), q("Second item"));
	rejectMutation(reversedLists, twoListOutput, twoListSpec, "reject reordered list item content");
	auto leadingSpace = whitespaceOutput; leadingSpace.replace(q("  first"), q(" first"));
	rejectMutation(leadingSpace, whitespaceOutput, whitespaceSpec, "reject one lost leading code space");
	auto tabChanged = whitespaceOutput; tabChanged.replace('\t', ' ');
	rejectMutation(tabChanged, whitespaceOutput, whitespaceSpec, "reject altered code tab");
	auto blankChanged = whitespaceOutput; blankChanged.replace(q("&#10;&#10;"), q("&#10;"));
	rejectMutation(blankChanged, whitespaceOutput, whitespaceSpec, "reject dropped blank code line");
	auto finalChanged = whitespaceOutput; finalChanged.replace(q("final"), q("different"));
	rejectMutation(finalChanged, whitespaceOutput, whitespaceSpec, "reject altered final code line");
	auto languageChanged = whitespaceOutput; languageChanged.replace(q("language-cpp"), q("language-python"));
	rejectMutation(languageChanged, whitespaceOutput, whitespaceSpec, "reject changed code language metadata");
	auto styledProse = semanticBase;
	styledProse.replace(q("<pre><code class=\"language-\">"), q("<p style=\"font-family:monospace;white-space:pre\">"));
	styledProse.replace(q("</code></pre>"), q("</p>"));
	rejectMutation(styledProse, semanticBase, {}, "reject styled monospace prose without semantic code node");
	auto falseQuote = semanticBase;
	falseQuote.replace(q("<blockquote>"), q("<div style=\"margin-left:40px;margin-right:40px\">"));
	falseQuote.replace(q("</blockquote>"), q("</div>"));
	rejectMutation(falseQuote, semanticBase, {}, "reject indented prose without quote node");
	const auto codeAnchor = q("<a id=\"code-anchor\" name=\"code-anchor\"></a>");
	auto droppedAnchor = semanticBase; droppedAnchor.replace(codeAnchor, QString());
	rejectMutation(droppedAnchor, semanticBase, {}, "reject missing code anchor");
	auto duplicatedAnchor = semanticBase; duplicatedAnchor.replace(codeAnchor, codeAnchor + codeAnchor);
	rejectMutation(duplicatedAnchor, semanticBase, {}, "reject duplicated code anchor");
	auto movedAnchor = semanticBase;
	movedAnchor.replace(q("code-anchor"), q("temporary-anchor"));
	movedAnchor.replace(q("quote-anchor"), q("code-anchor"));
	movedAnchor.replace(q("temporary-anchor"), q("quote-anchor"));
	rejectMutation(movedAnchor, semanticBase, {}, "reject code anchor moved onto quote");
	const auto preSource = q("<pre><code class=\"language-\">literal code</code></pre>");
	auto escapedCode = semanticBase; escapedCode.replace(preSource, q("<p>&lt;pre&gt;&lt;code&gt;literal code&lt;/code&gt;&lt;/pre&gt;</p>"));
	rejectMutation(escapedCode, semanticBase, {}, "reject escaped code tag decoy");
	auto commentCode = semanticBase; commentCode.replace(preSource, q("<!--") + preSource + q("--><p>literal code</p>"));
	rejectMutation(commentCode, semanticBase, {}, "reject comment-only code tag decoy");
	auto emptyDecoy = semanticBase; emptyDecoy.replace(preSource, q("<pre><code class=\"language-\"></code></pre><p>literal code</p>"));
	rejectMutation(emptyDecoy, semanticBase, {}, "reject empty code node beside styled or plain content");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
