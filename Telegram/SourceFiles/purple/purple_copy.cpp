#include "purple/purple_copy.h"

#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/view/history_view_list_widget.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_text.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_html_tags.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/popup_menu.h"

#include <QtGui/QTextDocument>

#include "styles/style_menu_icons.h"

namespace Purple {
namespace {

MarkdownCopyToken CurrentMarkdownCopy;

} // namespace

MarkdownCopyToken BeginMarkdownCopy() {
	CurrentMarkdownCopy = std::make_shared<MarkdownCopyOperation>();
	return CurrentMarkdownCopy;
}

bool IsCurrentMarkdownCopy(const MarkdownCopyToken &token) {
	return token && token == CurrentMarkdownCopy;
}

QString CopyTextAsMarkdown(const TextForMimeData &text) {
	if (text.empty()) {
		return QString();
	}
	auto document = QTextDocument();
	document.setDefaultStyleSheet(u"body, p, li { white-space: pre-wrap; }"_q);
	auto html = TextUtilities::TextForMimeDataToHtml(text);
	html.replace(u"<body>"_q, u"<body style=\"white-space: pre-wrap;\">"_q);
	document.setHtml(html);
	const auto markdown = document.toMarkdown(QTextDocument::MarkdownDialectGitHub);
	auto roundtrip = QTextDocument();
	roundtrip.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
	if (roundtrip.toPlainText() == text.rich.text) {
		return markdown;
	}
	const auto body = html.indexOf(u"<body style=\"white-space: pre-wrap;\">"_q);
	const auto end = html.lastIndexOf(u"</body>"_q);
	const auto opening = u"<body style=\"white-space: pre-wrap;\">"_q;
	return body >= 0 && end >= body
		? u"<pre style=\"font-family:inherit;white-space:pre-wrap\">%1</pre>\n"_q.arg(
			html.mid(body + opening.size(), end - body - opening.size())
				.replace(u"<br>"_q, u"\n"_q))
		: markdown;
}

void AddCopyAsMarkdownAction(
		not_null<Ui::PopupMenu*> menu,
		Fn<TextForMimeData()> text) {
	menu->addAction(tr::lng_purple_copy_as_markdown(tr::now), [=] {
		BeginMarkdownCopy();
		const auto markdown = CopyTextAsMarkdown(text());
		if (!markdown.isEmpty()) {
			TextUtilities::SetClipboardText(TextForMimeData::Simple(markdown));
		}
	}, &st::menuIconCopy);
}

} // namespace Purple
