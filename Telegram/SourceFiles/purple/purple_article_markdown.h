#pragma once

#include "iv/iv_rich_page.h"

namespace Purple {

struct ArticleMarkdown {
	QString text;
	QString error;
};

[[nodiscard]] std::shared_ptr<const Iv::RichPage> SelectCompleteArticlePage(
	const std::shared_ptr<const Iv::RichPage> &original,
	const std::shared_ptr<const Iv::RichPage> &full,
	const std::shared_ptr<const Iv::RichPage> &translation,
	bool translationUsed);

[[nodiscard]] ArticleMarkdown RenderArticleMarkdown(
	const Iv::RichPage &page,
	const QString &sourcePermalink = QString());

} // namespace Purple
