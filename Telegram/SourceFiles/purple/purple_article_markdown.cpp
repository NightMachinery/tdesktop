#include "purple/purple_article_markdown.h"

#include "iv/markdown/iv_markdown_prepare_serialize.h"
#include "purple/purple_copy.h"
#include "ui/text/text_html_tags.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <algorithm>

namespace Purple {
namespace {

using Page = Iv::RichPage;
using Kind = Page::BlockKind;

QString Escape(const QString &text) {
	return TextUtilities::EscapeForHtml(text);
}

QString ExternalUrl(const QString &text, bool inlineLink = false) {
	const auto url = QUrl(text, QUrl::StrictMode);
	if (inlineLink && text.startsWith('#') && url.isValid()) {
		return url.toString(QUrl::FullyEncoded);
	}
	const auto scheme = url.scheme().toLower();
	const auto web = (scheme == u"http"_q || scheme == u"https"_q)
		&& !url.host().isEmpty();
	const auto other = inlineLink && (scheme == u"mailto"_q
		|| scheme == u"tel"_q || scheme == u"tg"_q);
	return (web || other) && url.isValid()
		? url.toString(QUrl::FullyEncoded)
		: QString();
}

QString OrderedListType(const QString &type) {
	if (type == u"a"_q
		|| type.compare(u"lower-alpha"_q, Qt::CaseInsensitive) == 0
		|| type.compare(u"lower-latin"_q, Qt::CaseInsensitive) == 0) {
		return u"a"_q;
	} else if (type == u"A"_q
		|| type.compare(u"upper-alpha"_q, Qt::CaseInsensitive) == 0
		|| type.compare(u"upper-latin"_q, Qt::CaseInsensitive) == 0) {
		return u"A"_q;
	} else if (type == u"i"_q
		|| type.compare(u"lower-roman"_q, Qt::CaseInsensitive) == 0) {
		return u"i"_q;
	} else if (type == u"I"_q
		|| type.compare(u"upper-roman"_q, Qt::CaseInsensitive) == 0) {
		return u"I"_q;
	}
	return u"1"_q;
}

QString Date(TimeId date) {
	return date ? QDateTime::fromSecsSinceEpoch(date, Qt::UTC).toString(Qt::ISODate)
		: QString();
}

QString ParagraphEndRemoved(QString text, const QString &source) {
	auto expected = 0;
	for (auto i = source.size(); i > 0 && source[i - 1] == '\n'; --i) ++expected;
	auto actual = 0;
	for (auto i = text.size(); i > 0 && text[i - 1] == '\n'; --i) ++actual;
	if (actual > expected) text.chop(actual - expected);
	return text;
}

bool NeedsAnchoredHtml(const Page::Block &value, int depth) {
	const auto rich = [](const Page::RichText &text) {
		return !text.anchorId.isEmpty() || !text.anchorIds.empty();
	};
	if (!value.anchorId.isEmpty() || rich(value.text) || rich(value.caption)) return true;
	if (depth >= 64) return true;
	for (const auto &child : value.blocks) {
		if (NeedsAnchoredHtml(child, depth + 1)) return true;
	}
	for (const auto &item : value.listItems) {
		if (!item.anchorId.isEmpty() || rich(item.text)) return true;
		for (const auto &child : item.blocks) {
			if (NeedsAnchoredHtml(child, depth + 1)) return true;
		}
	}
	for (const auto &row : value.tableRows) {
		for (const auto &cell : row.cells) if (rich(cell.text)) return true;
	}
	return false;
}

class Writer final {
public:
	explicit Writer(QString source) : _source(ExternalUrl(source)) {
	}

	ArticleMarkdown render(const Page &page) {
		if (page.part || page.blocks.empty()) {
			return { .error = u"The complete article is unavailable."_q };
		}
		const auto text = page.rtl
			? u"<article dir=\"rtl\">\n%1\n</article>"_q.arg(blocks(page.blocks, true, 0))
			: blocks(page.blocks, false, 0);
		return { text, _error };
	}

private:
	QString inlineText(const Page::RichText &rich, bool html);
	QString leaf(const TextWithEntities &text, bool html);
	QString blocks(const std::vector<Page::Block> &items, bool html, int depth);
	QString block(const Page::Block &value, bool html, int depth);
	QString list(const Page::Block &value, bool html, int depth);
	QString table(const Page::Block &value, int depth);
	QString media(const QString &label, const QString &url, bool html);
	QString link(const QString &label, const QString &url, bool html);
	QString anchor(const QString &id);
	void fail(const QString &error) {
		if (_error.isEmpty()) {
			_error = error;
		}
	}

	QString _source;
	QString _error;

};

QString Writer::anchor(const QString &id) {
	return id.isEmpty() ? QString() : u"<a id=\"%1\" name=\"%1\"></a>"_q.arg(Escape(id));
}

QString Writer::leaf(const TextWithEntities &text, bool html) {
	if (text.text.contains(QChar::ObjectReplacementCharacter)) {
		fail(u"An article contains an unresolved inline object."_q);
		return QString();
	}
	for (const auto &entity : text.entities) {
		if (entity.offset() < 0 || entity.length() < 0
			|| int64(entity.offset()) + entity.length() > text.text.size()) {
			fail(u"An article contains invalid text entities."_q);
			return QString();
		}
		if (entity.type() == EntityType::CustomUrl
			&& ExternalUrl(entity.data(), true).isEmpty()) {
			const auto decoded = Iv::DecodeRichPageLinkUrl(entity.data());
			if (!decoded || ExternalUrl(decoded->url, true).isEmpty()) {
				fail(u"An article contains an unsupported link target."_q);
				return QString();
			}
		}
	}
	auto normalized = text;
	auto preserveHtml = html;
	for (auto &entity : normalized.entities) {
		if (entity.type() == EntityType::Semibold) {
			entity = EntityInText(EntityType::Bold, entity.offset(), entity.length());
		}
		preserveHtml = preserveHtml || entity.type() == EntityType::Underline
			|| entity.type() == EntityType::Subscript || entity.type() == EntityType::Superscript
			|| entity.type() == EntityType::Marked || entity.type() == EntityType::Spoiler
			|| entity.type() == EntityType::FormattedDate;
		if (entity.type() == EntityType::CustomUrl) {
			if (const auto decoded = Iv::DecodeRichPageLinkUrl(entity.data())) {
				entity = EntityInText(entity.type(), entity.offset(), entity.length(), decoded->url);
			}
		}
	}
	const auto mime = TextForMimeData::Rich(std::move(normalized));
	if (!preserveHtml) {
		return ParagraphEndRemoved(CopyTextAsMarkdown(mime), text.text);
	}
	auto result = TextUtilities::TextForMimeDataToHtml(mime);
	const auto begin = result.indexOf(u"<body>"_q);
	const auto end = result.lastIndexOf(u"</body>"_q);
	if (begin < 0 || end < begin) {
		return Escape(text.text);
	}
	return u"<span style=\"white-space:pre-wrap\">%1</span>"_q.arg(
		result.mid(begin + 6, end - begin - 6));
}

QString Writer::inlineText(const Page::RichText &rich, bool html) {
	auto result = anchor(rich.anchorId);
	for (const auto &id : rich.anchorIds) {
		result += anchor(id);
	}
	auto objects = EntitiesInText();
	for (const auto &entity : rich.text.entities) {
		if (entity.type() == EntityType::CustomEmoji
			&& Iv::Markdown::ParseInlineTextObjectEntity(entity.data())) {
			objects.push_back(entity);
		}
	}
	std::sort(objects.begin(), objects.end(), [](const auto &a, const auto &b) {
		return a.offset() < b.offset();
	});
	auto offset = 0;
	for (const auto &entity : objects) {
		if (entity.offset() < offset || entity.offset() + entity.length() > rich.text.text.size()) {
			fail(u"An article contains an invalid inline object."_q);
			continue;
		}
		result += leaf(Ui::Text::Mid(rich.text, offset, entity.offset() - offset), html);
		const auto object = Iv::Markdown::ParseInlineTextObjectEntity(entity.data());
		if (const auto formula = std::get_if<Iv::Markdown::InlineTextObjectFormulaData>(&object->data)) {
			const auto source = formula->trimmedTex;
			result += (html || source.contains('$'))
				? u"<code class=\"math\">%1</code>"_q.arg(Escape(source))
				: u"$%1$"_q.arg(source);
		} else if (const auto image = std::get_if<Iv::Markdown::InlineTextObjectIvImageData>(&object->data)) {
			result += media(image->replacementText.isEmpty() ? u"Image"_q : image->replacementText, QString(), html);
		} else {
			fail(u"An article contains an unsupported inline object."_q);
		}
		offset = entity.offset() + entity.length();
	}
	result += leaf(Ui::Text::Mid(rich.text, offset), html);
	return result;
}

QString Writer::link(const QString &label, const QString &url, bool html) {
	const auto target = ExternalUrl(url, true);
	if (target.isEmpty()) {
		return html ? Escape(label) : leaf({ label }, false);
	}
	return html
		? u"<a href=\"%1\">%2</a>"_q.arg(Escape(target), Escape(label))
		: u"[%1](<%2>)"_q.arg(leaf({ label }, false), target);
}

QString Writer::media(const QString &label, const QString &url, bool html) {
	const auto target = ExternalUrl(url);
	const auto actual = target.isEmpty() ? _source : target;
	const auto description = u"[%1]"_q.arg(label);
	return actual.isEmpty()
		? (html ? Escape(description) : leaf({ description }, false))
		: link(description, actual, html);
}

QString Writer::blocks(const std::vector<Page::Block> &items, bool html, int depth) {
	if (depth > 64) {
		fail(u"The article nesting is unsupported."_q);
		return QString();
	}
	auto result = QStringList();
	for (auto index = size_t(0); index != items.size();) {
		if (!html && NeedsAnchoredHtml(items[index], depth)) {
			auto island = QString();
			do {
				island += block(items[index], true, depth);
				++index;
			} while (index != items.size() && NeedsAnchoredHtml(items[index], depth));
			island.replace(u"\n"_q, u"&#10;"_q);
			result.push_back(u"<div>%1</div>"_q.arg(island));
		} else {
			result.push_back(block(items[index++], html, depth));
		}
	}
	return result.join(html ? u"\n"_q : u"\n\n"_q);
}

QString Writer::list(const Page::Block &value, bool html, int depth) {
	const auto ordered = value.listKind == Page::ListKind::Ordered;
	constexpr auto kMaximumMarkdownMarker = int64(999999999);
	const auto start = int64(value.orderedList.start.value_or(1));
	const auto markdownRange = !ordered || (start >= 0
		&& start <= kMaximumMarkdownMarker
		&& (value.listItems.empty()
			|| value.listItems.size() - 1 <= uint64(kMaximumMarkdownMarker - start)));
	const auto complex = html || !markdownRange || value.orderedList.reversed
		|| value.orderedList.type.has_value()
		|| std::any_of(value.listItems.begin(), value.listItems.end(), [](const auto &item) {
			return !item.anchorId.isEmpty() || item.number.num.has_value() || item.number.value.has_value() || item.number.type.has_value();
		});
	if (!complex) {
		auto rows = QStringList();
		auto number = start;
		for (const auto &item : value.listItems) {
			auto body = inlineText(item.text, false);
			if (!item.blocks.empty()) {
				body += u"\n\n"_q + blocks(item.blocks, false, depth + 1);
			}
			const auto task = item.taskState == Page::TaskState::None ? QString()
				: item.taskState == Page::TaskState::Checked ? u"[x] "_q : u"[ ] "_q;
			body.replace(u"\n"_q, u"\n    "_q);
			const auto marker = ordered ? QString::number(number) + u". "_q : u"- "_q;
			rows.push_back(marker + task + anchor(item.anchorId) + body);
			if (ordered) ++number;
		}
		return rows.join('\n');
	}
	const auto tag = ordered ? u"ol"_q : u"ul"_q;
	auto attrs = QString();
	if (ordered) {
		if (value.orderedList.start) attrs += u" start=\"%1\""_q.arg(*value.orderedList.start);
		if (value.orderedList.type) attrs += u" type=\"%1\""_q.arg(OrderedListType(*value.orderedList.type));
		if (value.orderedList.reversed) attrs += u" reversed"_q;
	}
	auto result = u"<%1%2>\n"_q.arg(tag, attrs);
	for (const auto &item : value.listItems) {
		auto attributes = item.number.value ? u" value=\"%1\""_q.arg(*item.number.value) : QString();
		if (item.number.type) attributes += u" type=\"%1\""_q.arg(OrderedListType(*item.number.type));
		if (item.number.hasRawText()) attributes += u" style=\"list-style-type:none\""_q;
		result += u"<li%1>%2"_q.arg(attributes, anchor(item.anchorId));
		if (item.number.hasRawText()) result += u"<span class=\"list-marker\">%1</span> "_q.arg(Escape(item.number.rawText()));
		if (item.taskState != Page::TaskState::None) result += item.taskState == Page::TaskState::Checked ? u"[x] "_q : u"[ ] "_q;
		result += u"<p>%1</p>"_q.arg(inlineText(item.text, true))
			+ blocks(item.blocks, true, depth + 1) + u"</li>\n"_q;
	}
	return result + u"</%1>"_q.arg(tag);
}

QString Writer::table(const Page::Block &value, int depth) {
	(void)depth;
	auto result = u"<table>\n"_q;
	if (!value.text.text.text.isEmpty()) result += u"<caption>%1</caption>\n"_q.arg(inlineText(value.text, true));
	for (const auto &row : value.tableRows) {
		result += u"<tr>"_q;
		for (const auto &cell : row.cells) {
			const auto tag = cell.header ? u"th"_q : u"td"_q;
			const auto align = cell.alignment == Page::TableAlignment::Center ? u"center"_q
				: cell.alignment == Page::TableAlignment::Right ? u"right"_q : u"left"_q;
			const auto vertical = cell.verticalAlignment == Page::TableVerticalAlignment::Top ? u"top"_q
				: cell.verticalAlignment == Page::TableVerticalAlignment::Bottom ? u"bottom"_q : u"middle"_q;
			if (cell.colspan < 1 || cell.rowspan < 1) fail(u"The article contains invalid table spans."_q);
			result += u"<%1 colspan=\"%2\" rowspan=\"%3\" align=\"%4\" valign=\"%5\">%6</%1>"_q
				.arg(tag).arg(cell.colspan).arg(cell.rowspan).arg(align, vertical, inlineText(cell.text, true));
		}
		result += u"</tr>\n"_q;
	}
	return result + u"</table>"_q;
}

QString Writer::block(const Page::Block &value, bool html, int depth) {
	auto result = anchor(value.anchorId);
	if (!html && !result.isEmpty()) result += u"\n\n"_q;
	if (!value.blocks.empty() && value.kind != Kind::Quote
		&& value.kind != Kind::Thinking && value.kind != Kind::EmbedPost
		&& value.kind != Kind::Details && value.kind != Kind::GroupedMedia
		&& value.kind != Kind::Photo && value.kind != Kind::Video
		&& value.kind != Kind::Audio && value.kind != Kind::Embed
		&& value.kind != Kind::Channel && value.kind != Kind::Map
		&& value.kind != Kind::Unsupported) {
		fail(u"The article contains children outside their supported block context."_q);
	}
	const auto rich = [&] { return inlineText(value.text, html); };
	const auto paragraph = [&](const QString &text) { return html ? u"<p>%1</p>"_q.arg(text) : text; };
	const auto children = [&] { return blocks(value.blocks, html, depth + 1); };
	switch (value.kind) {
	case Kind::Heading: {
		if (value.headingLevel < 1 || value.headingLevel > 6) fail(u"The article contains an unsupported heading level."_q);
		const auto level = std::clamp(value.headingLevel, 1, 6);
		result += html ? u"<h%1>%2</h%1>"_q.arg(level).arg(rich())
			: QString(level, '#') + ' ' + rich();
	} break;
	case Kind::Paragraph:
		result += paragraph(rich());
		break;
	case Kind::Footer:
		result += u"<footer>%1</footer>"_q.arg(inlineText(value.text, true));
		break;
	case Kind::AuthorDate:
		result += paragraph(rich() + (value.date ? u" · "_q + Date(value.date) : QString()));
		break;
	case Kind::Code: {
		const auto source = value.text.text.text;
		if (html || value.language.contains('\n') || value.language.contains('`')) {
			result += u"<pre><code class=\"language-%1\">%2</code></pre>"_q.arg(Escape(value.language), Escape(source));
		} else {
			auto count = 3;
			auto runs = QRegularExpression(u"`+"_q).globalMatch(source);
			while (runs.hasNext()) count = std::max(count, int(runs.next().capturedLength()) + 1);
			const auto fence = QString(count, '`');
			result += fence + value.language + '\n' + source + (source.endsWith('\n') ? QString() : u"\n"_q) + fence;
		}
	} break;
	case Kind::Divider: result += html ? u"<hr>"_q : u"---"_q; break;
	case Kind::Anchor: break;
	case Kind::List: result += list(value, html, depth); break;
	case Kind::Thinking:
	case Kind::Quote:
	case Kind::EmbedPost: {
		auto body = rich();
		if (html && !body.isEmpty()) body = paragraph(body);
		const auto add = [&](const QString &text) {
			body += (body.isEmpty() || html ? QString() : u"\n\n"_q) + paragraph(text);
		};
		if (!value.author.isEmpty()) add(html ? Escape(value.author) : leaf({ value.author }, false));
		if (value.date) add(Date(value.date));
		if (!value.url.isEmpty()) add(link(value.url, value.url, html));
		if (value.photoId) add(media(u"Photo"_q, QString(), html));
		if (!value.blocks.empty()) body += (html ? QString() : u"\n\n"_q) + children();
		if (!value.caption.text.text.isEmpty()) body += (html ? u"<cite>"_q : u"\n\n"_q) + inlineText(value.caption, html) + (html ? u"</cite>"_q : QString());
		if (html) result += u"<blockquote>%1</blockquote>"_q.arg(body);
		else { body.replace(u"\n"_q, u"\n> "_q); result += u"> "_q + body; }
	} break;
	case Kind::Photo:
	case Kind::Video:
	case Kind::Audio:
	case Kind::Embed:
	case Kind::Channel:
	case Kind::Map: {
		auto label = value.kind == Kind::Photo ? u"Photo"_q : value.kind == Kind::Video ? u"Video"_q
			: value.kind == Kind::Audio ? u"Audio"_q : value.kind == Kind::Channel ? u"Channel"_q
			: value.kind == Kind::Map ? u"Map"_q : u"Embedded content"_q;
		for (const auto &part : { value.audioFileName, value.audioPerformer, value.audioTitle, value.channelTitle }) {
			if (!part.isEmpty()) label += u": "_q + part;
		}
		if (value.kind == Kind::Audio && value.audioDuration > 0) label += u": %1 seconds"_q.arg(value.audioDuration);
		if (value.kind == Kind::Map) label += u": %1, %2"_q.arg(QString::number(value.latitude), QString::number(value.longitude));
		const auto url = value.kind == Kind::Channel && !value.username.isEmpty()
			? u"https://t.me/"_q + value.username : value.url;
		result += paragraph(media(label, url, html));
		if (!value.text.text.text.isEmpty()) result += (html ? QString() : u"\n\n"_q) + paragraph(rich());
		if (!value.caption.text.text.isEmpty()) result += (html ? QString() : u"\n\n"_q) + paragraph(inlineText(value.caption, html));
		if (!value.blocks.empty()) result += (html ? QString() : u"\n\n"_q) + children();
	} break;
	case Kind::GroupedMedia: {
		result += paragraph(media(value.mediaIntent == Page::GroupedMediaIntent::Slideshow
			? u"Slideshow"_q : u"Gallery"_q, QString(), html)) + (html ? QString() : u"\n\n"_q);
		for (const auto &item : value.mediaItems) {
			if (item.kind != Kind::Photo && item.kind != Kind::Video && item.kind != Kind::Audio) {
				fail(u"The article contains an unsupported grouped media item."_q);
			}
			const auto label = item.kind == Kind::Photo ? u"Photo"_q
				: item.kind == Kind::Audio ? u"Audio"_q : item.kind == Kind::Video ? u"Video"_q : u"Media"_q;
			result += paragraph(media(label, QString(), html)) + (html ? QString() : u"\n\n"_q);
		}
		result += children();
		if (!value.caption.text.text.isEmpty()) result += paragraph(inlineText(value.caption, html));
	} break;
	case Kind::Math:
		result += html || value.formula.contains(u"$$"_q)
			? u"<pre class=\"math\">%1</pre>"_q.arg(Escape(value.formula))
			: u"$$\n%1\n$$"_q.arg(value.formula);
		break;
	case Kind::Table: result += table(value, depth); break;
	case Kind::Details:
		result += u"<details%1><summary>%2</summary>\n%3\n</details>"_q.arg(
			value.open ? u" open"_q : QString(), inlineText(value.text, true), blocks(value.blocks, true, depth + 1));
		break;
	case Kind::RelatedArticles:
		result += paragraph(rich());
		for (const auto &article : value.relatedArticles) {
			const auto add = [&](const QString &text) {
				result += (result.isEmpty() || html ? QString() : u"\n\n"_q) + paragraph(text);
			};
			add(link(article.title.isEmpty() ? article.url : article.title, article.url, html));
			if (article.photoId || article.photo) add(media(u"Related article photo"_q, article.url, html));
			if (!article.description.isEmpty()) add(html ? Escape(article.description) : leaf({ article.description }, false));
			if (!article.author.isEmpty()) add(html ? Escape(article.author) : leaf({ article.author }, false));
			if (article.publishedDate) add(Date(article.publishedDate));
			result += html ? u"\n"_q : u"\n\n"_q;
		}
		break;
	case Kind::Unsupported:
	default:
		fail(u"The article contains an unsupported block."_q);
		result += paragraph(u"[Unsupported article block]"_q) + rich() + children();
		break;
	}
	return result;
}

} // namespace

std::shared_ptr<const Iv::RichPage> SelectCompleteArticlePage(
		const std::shared_ptr<const Iv::RichPage> &original,
		const std::shared_ptr<const Iv::RichPage> &full,
		const std::shared_ptr<const Iv::RichPage> &translation,
		bool translationUsed) {
	const auto page = translationUsed ? translation : full ? full : original;
	return page && !page->part ? page : nullptr;
}

ArticleMarkdown RenderArticleMarkdown(const Iv::RichPage &page, const QString &sourcePermalink) {
	return Writer(sourcePermalink.isEmpty() ? page.url : sourcePermalink).render(page);
}

} // namespace Purple
