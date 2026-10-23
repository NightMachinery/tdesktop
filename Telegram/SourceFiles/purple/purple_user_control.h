#pragma once

#include "base/basic_types.h"
#include "crl/crl_time.h"
#include "scheme.h"

#include <memory>
#include <optional>

class DocumentData;
class HistoryItem;
namespace Ui::Menu { struct MenuCallback; }
namespace Data { class Media; }
namespace HistoryView {
struct ContextMenuRequest;
}
namespace Ui { class PopupMenu; }

namespace Purple {

class SelfDestructSaveSource final {
public:
	explicit SelfDestructSaveSource(const void *identity);
	[[nodiscard]] bool active(
		const void *current,
		std::optional<crl::time> destructAt,
		crl::time now) const;
	void consume();

private:
	const void *_identity = nullptr;
	bool _consumed = false;

};

[[nodiscard]] inline bool SaveRestrictionsDisabled() {
	return true;
}

[[nodiscard]] bool CanSaveSelfDestructMedia(
	const MTPMessageMedia &media,
	bool unread);
[[nodiscard]] bool SelfDestructHasServicePlaceholder(
	const MTPMessageMedia *media);
[[nodiscard]] QString SelfDestructDocumentSaveExtension(
	bool voice, const QString &mime);
[[nodiscard]] bool AddSelfDestructDocumentSaveAction(
	const Ui::Menu::MenuCallback &addAction,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document);
[[nodiscard]] std::unique_ptr<Data::Media> SelfDestructMediaForSaving(
	not_null<HistoryItem*> item,
	const MTPMessageMedia &media);
void ExpireSelfDestructMedia(
	not_null<HistoryItem*> item,
	bool consumed = false);
void AddSelfDestructSaveAction(
	not_null<Ui::PopupMenu*> menu,
	const HistoryView::ContextMenuRequest &request);
void AddSelfDestructSaveAction(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item);

}
