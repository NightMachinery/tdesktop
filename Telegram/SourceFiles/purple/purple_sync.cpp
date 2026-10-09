/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync.h"
#include "purple/purple_ui_witness.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_file_click_handler.h"
#include "data/data_groups.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_list_widget.h"
#include "info/info_controller.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_session_settings.h"
#include "storage/localimageloader.h"
#include "storage/storage_shared_media.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QRegion>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include "styles/style_menu_icons.h"

namespace Purple {

struct ImportSettingsContext {
	base::weak_ptr<Main::Session> session;
	base::weak_ptr<Window::SessionController> asking;
	std::shared_ptr<Ui::Show> show;
	FullMsgId itemId;
	const HistoryItem *itemIdentity = nullptr;
	DocumentData *document = nullptr;
	TimeId date = 0;
	QString account;
};

struct ImportSettingsTarget {
	enum class Input : char {
		Mouse,
		Keyboard,
		Files,
	};

	ImportSettingsContext context;
	ImportSettingsViewState native;
	HistoryView::Element *view = nullptr;
	Overview::Layout::ItemBase *layout = nullptr;
	FullMsgId containerId;
	const HistoryItem *containerIdentity = nullptr;
	GlobalMsgId globalId;
	QPoint rawPoint;
	Input input = Input::Mouse;
};

namespace {

#ifdef Q_OS_MAC

class HistoryContextMenuShortcutFilter final : public QObject {
public:
	HistoryContextMenuShortcutFilter(
		not_null<QWidget*> receiver,
		Qt::KeyboardModifiers modifiers);

	void bindTo(not_null<Ui::PopupMenu*> popup);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QPointer<Ui::PopupMenu> _popup;
	Qt::KeyboardModifiers _modifiers;
	bool _bound = false;

};

[[nodiscard]] bool IsHistoryContextMenuShortcut(
		not_null<QKeyEvent*> event) {
	const auto control = QCoreApplication::testAttribute(
		Qt::AA_MacDontSwapCtrlAndMeta)
		? Qt::ControlModifier
		: Qt::MetaModifier;
	return event->type() == QEvent::KeyPress
		&& event->key() == Qt::Key_Return
		&& event->modifiers() == control;
}

HistoryContextMenuShortcutFilter::HistoryContextMenuShortcutFilter(
		not_null<QWidget*> receiver,
		Qt::KeyboardModifiers modifiers)
: QObject(receiver)
, _modifiers(modifiers) {
	qApp->installEventFilter(this);
}

void HistoryContextMenuShortcutFilter::bindTo(
		not_null<Ui::PopupMenu*> popup) {
	_popup = popup;
	_bound = true;
	setParent(popup);
}

bool HistoryContextMenuShortcutFilter::eventFilter(
		QObject *watched,
		QEvent *event) {
	if (event->type() != QEvent::KeyPress) {
		return false;
	}
	const auto key = static_cast<QKeyEvent*>(event);
	if (!key->isAutoRepeat()
		|| key->key() != Qt::Key_Return
		|| key->modifiers() != _modifiers) {
		return false;
	}
	auto receiver = qobject_cast<QWidget*>(parent());
	if (_bound) {
		if (!_popup || QApplication::activePopupWidget() != _popup) {
			return false;
		}
		receiver = _popup.data();
	} else {
		if (!receiver) {
			return false;
		}
		if (const auto active = QApplication::activePopupWidget()) {
			const auto popup = dynamic_cast<Ui::PopupMenu*>(active);
			if (!popup || popup->parentWidget() != receiver) {
				return false;
			}
			receiver = popup;
		} else if (QApplication::focusWidget() != receiver) {
			return false;
		}
	}
	const auto widget = qobject_cast<QWidget*>(watched);
	const auto window = qobject_cast<QWindow*>(watched);
	const auto widgetMatches = widget
		&& (receiver->isWindow()
			? widget->window() == receiver
			: widget == receiver);
	if (!widgetMatches
		&& (!window || window != receiver->window()->windowHandle())) {
		return false;
	}
	key->accept();
	return true;
}

#endif

constexpr auto kFileName = "settings.toml";

// A settings.toml is a few kilobytes of TOML, and this fetches one into memory
// rather than onto disk. Anything of a size that could not be the file is not
// the file, and offering to import it would be offering to import something
// the download path cannot even hold.
constexpr auto kMaxSize = 4 * 1024 * 1024;
constexpr auto kSearchLimit = 100;

[[nodiscard]] QString SettingsFileName() {
	return QString::fromLatin1(kFileName);
}

[[nodiscard]] QString PlatformName() {
	return Platform::IsWindows()
		? u"Windows"_q
		: Platform::IsMac()
		? u"macOS"_q
		: u"Linux"_q;
}

[[nodiscard]] QString FormatMoment(const QDateTime &when) {
	return when.toString(u"yyyy-MM-dd HH:mm"_q);
}

// What the file is, when it was written and where it came from - the three
// things you need to pick one message out of a Saved Messages chat holding a
// year of them. The schema version is first because it is the only one that
// can stop an import from making sense.
[[nodiscard]] QString Caption(int version) {
	const auto separator = u" · "_q;
	return u"Purple settings"_q
		+ separator + u"schema v%1"_q.arg(version)
		+ separator + FormatMoment(QDateTime::currentDateTime())
		+ separator + PlatformName();
}

[[nodiscard]] QByteArray LocalContent(
		const std::shared_ptr<Data::DocumentMedia> &media,
		not_null<DocumentData*> document) {
	if (const auto bytes = media->bytes(); !bytes.isEmpty()) {
		return bytes;
	}
	const auto path = document->filepath(true);
	if (path.isEmpty()) {
		return QByteArray();
	}
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

[[nodiscard]] bool IsActiveSession(not_null<Main::Session*> session) {
	auto &domain = Core::App().domain();
	return domain.started() && (domain.active().maybeSession() == session);
}

[[nodiscard]] QString AccountLabel(not_null<Main::Session*> session) {
	const auto user = session->user();
	return ImportAccountLabel(user->name(), user->username());
}

[[nodiscard]] bool AskingWindowSwitched(
		const std::shared_ptr<Ui::Show> &show) {
	return !show || !(*show);
}

[[nodiscard]] DocumentData *QualifyingImportDocument(
		not_null<Main::Session*> session,
		HistoryItem *item,
		bool allowLoading = false) {
	if (!item
		|| !item->isRegular()
		|| !item->isHistoryEntry()
		|| &item->history()->session() != session
		|| session->data().message(item->fullId()) != item
		|| item->history()->peer != session->user()
		|| !item->history()->peer->isSelf()
		|| item->isSending()
		|| item->isUploading()) {
		return nullptr;
	}
	const auto media = item->media();
	if (!media || !media->sharedMediaTypes().test(Storage::SharedMediaType::File)) {
		return nullptr;
	}
	const auto document = media->document();
	if (!document
		|| &document->session() != session
		|| document->size <= 0
		|| document->size > kMaxSize
		|| document->uploading()
		|| (!allowLoading && document->loading())
		|| document->filename().compare(
			SettingsFileName(),
			Qt::CaseInsensitive) != 0) {
		return nullptr;
	}
	return document;
}

[[nodiscard]] bool DocumentHandlerAgrees(
		const HistoryView::TextState &state,
		not_null<DocumentData*> document) {
	const auto handler = dynamic_cast<DocumentClickHandler*>(state.link.get());
	return handler
		&& handler->document() == document
		&& handler->context() == state.itemId;
}

[[nodiscard]] bool ContentHitAgrees(
		const HistoryView::TextState &state,
		not_null<DocumentData*> document) {
	using Origin = HistoryView::ContentOrigin;
	if (!state.itemId || state.contentOwner != state.itemId) {
		return false;
	}
	switch (state.contentOrigin) {
	case Origin::MessageBody:
	case Origin::DocumentCaption:
		return state.cursor == HistoryView::CursorState::Text;
	case Origin::DocumentCard:
		return DocumentHandlerAgrees(state, document);
	case Origin::None:
		return false;
	}
	return false;
}

[[nodiscard]] bool ContainerOwnsItem(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> container,
		not_null<HistoryItem*> item) {
	if (&container->history()->session() != session
		|| session->data().message(container->fullId()) != container) {
		return false;
	} else if (container == item) {
		return true;
	}
	const auto group = session->data().groups().find(container);
	return group
		&& !group->items.empty()
		&& group->items.front() == container
		&& session->data().groups().find(item) == group
		&& ranges::contains(group->items, item);
}

[[nodiscard]] bool ReceivingContentReady(
		const ImportSettingsViewState &native) {
	return native.ready
		&& native.receiver
		&& native.receiver->isVisible()
		&& native.receiver->window()->isActiveWindow()
		&& !native.viewport.isEmpty()
		&& !native.geometry.isEmpty()
		&& native.receiver->rect().intersects(native.viewport)
		&& native.viewport.intersects(native.geometry);
}

[[nodiscard]] bool ReceivingPointReady(
		const ImportSettingsViewState &native,
		QPoint rawPoint) {
	return ReceivingContentReady(native)
		&& native.receiver->rect().contains(rawPoint)
		&& native.viewport.contains(rawPoint)
		&& native.geometry.contains(rawPoint)
		&& native.receiver->visibleRegion().contains(rawPoint);
}

[[nodiscard]] ImportSettingsContext ImportContextFor(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	return {
		.session = base::make_weak(&controller->session()),
		.asking = base::make_weak(controller.get()),
		.show = controller->uiShow(),
		.itemId = item->fullId(),
		.itemIdentity = item.get(),
		.document = document.get(),
		.date = item->date(),
		.account = AccountLabel(&controller->session()),
	};
}

[[nodiscard]] DocumentData *ImportContextDocument(
		const ImportSettingsContext &context,
		bool allowLoading) {
	if (AskingWindowSwitched(context.show)) {
		return nullptr;
	}
	const auto asking = context.asking.get();
	const auto session = context.session.get();
	if (!asking || !session || &asking->session() != session) {
		return nullptr;
	}
	const auto item = session->data().message(context.itemId);
	if (!item || item != context.itemIdentity || item->date() != context.date) {
		return nullptr;
	}
	const auto document = QualifyingImportDocument(session, item, allowLoading);
	return (document == context.document) ? document : nullptr;
}

[[nodiscard]] bool HistoryContainerReady(
		not_null<Window::SessionController*> controller,
		HistoryView::Element *view,
		const ImportSettingsViewState &native) {
	return ReceivingContentReady(native)
		&& view
		&& !view->isHidden()
		&& !view->pendingResize()
		&& &view->data()->history()->session() == &controller->session();
}

[[nodiscard]] bool FilesControllerAgrees(
		not_null<Info::AbstractController*> controller,
		HistoryItem *row,
		GlobalMsgId globalId) {
	const auto parent = controller->parentController();
	const auto section = controller->section();
	const auto peer = controller->key().peer();
	const auto session = &controller->session();
	return &parent->session() == session
		&& section.type() == Info::Section::Type::Media
		&& section.mediaType() == Storage::SharedMediaType::File
		&& !controller->isDownloads()
		&& !controller->isGlobalMedia()
		&& !controller->storiesPeer()
		&& !controller->musicPeer()
		&& peer
		&& peer->isSelf()
		&& &peer->session() == session
		&& row
		&& row->history()->peer == peer
		&& &row->history()->session() == session
		&& globalId == row->globalId()
		&& globalId.sessionUniqueId == session->uniqueId()
		&& session->data().message(globalId.itemId) == row;
}

void RefuseSwitchedImport(const QString &account) {
	LOG(("Purple: settings import dropped: "
		"the window that asked was closed or shows another account now."));
	if (const auto window = Core::App().activeWindow()) {
		window->uiShow()->showBox(Ui::MakeInformBox(
			ImportWindowSwitchedText(account)));
	}
}

void WriteImported(
		const QString &account,
		const QString &text,
		const std::shared_ptr<Ui::Show> &show) {
	if (AskingWindowSwitched(show)) {
		RefuseSwitchedImport(account);
		return;
	}
	const auto path = SettingsFilePath();
	const auto failed = WriteImportedSettings(path, [&] {
		return WriteConfigFile(path, text);
	});
	if (failed == path) {
		show->showBox(Ui::MakeInformBox(
			u"Could not write %1. See the log."_q.arg(path)));
		return;
	} else if (!failed.isEmpty()) {
		LOG(("Purple Error: Could not write %1, so nothing was imported."
			).arg(failed));
		show->showBox(Ui::MakeInformBox(
			u"Could not write %1, so nothing was imported. See the log."_q
				.arg(failed)));
		return;
	}
	// The bytes as they landed on disk, which is what the automatic send will
	// fingerprint the next time anything writes this file. Recording them here
	// is what stops this machine from offering back to the other one exactly
	// what it just received. See ShouldAutoSend().
	NoteSettingsImported(text.toUtf8());
	// Nothing else to do: the directory watcher sees the write and reloads,
	// exactly as it would for an edit made in a text editor.
	show->showToast(u"Settings imported"_q);
}

void ConfirmAndImport(
		not_null<Main::Session*> session,
		const QByteArray &content,
		TimeId date,
		const std::shared_ptr<Ui::Show> &show,
		Fn<bool()> stillValid) {
	const auto account = AccountLabel(session);
	if (stillValid && !stillValid()) {
		if (AskingWindowSwitched(show)) {
			RefuseSwitchedImport(account);
		}
		return;
	}
	if (AskingWindowSwitched(show)) {
		RefuseSwitchedImport(account);
		return;
	}
	const auto text = QString::fromUtf8(content);
	const auto parsed = ParseSettings(text, SettingsFilePath());
	if (!parsed.ok()) {
		show->showBox(Ui::MakeInformBox(
			u"That %1 is not valid TOML, so nothing was changed:\n\n%2"_q.arg(
				SettingsFileName(),
				parsed.error)));
		return;
	}
	const auto version = parsed.settings.version;
	const auto warnings = int(parsed.warnings.size());
	auto lines = QStringList();
	lines.push_back(
		u"Import the settings sent %1 to Saved Messages of %2?"_q.arg(
			FormatMoment(base::unixtime::parse(date)),
			account));
	lines.push_back(QString());
	lines.push_back(u"Schema v%1."_q.arg(version));
	if (version > kSettingsVersion) {
		lines.push_back(u"That is newer than this build understands (v%1), "
			"so keys it has never heard of will be ignored."_q.arg(
				kSettingsVersion));
	}
	lines.push_back(!warnings
		? u"No parser warnings."_q
		: (warnings == 1)
		? u"1 parser warning."_q
		: u"%1 parser warnings."_q.arg(warnings));
	lines.push_back(QString());
	lines.push_back(u"Replaces your current settings; the previous file is "
		"kept as %1."_q.arg(SettingsFileName() + u".import.bak"_q));

	const auto keep = show;
	const auto weakSession = base::make_weak(session.get());
	show->showBox(Ui::MakeConfirmBox({
		.text = lines.join('\n'),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (!weakSession.get() || (stillValid && !stillValid())) {
				if (AskingWindowSwitched(keep)) {
					RefuseSwitchedImport(account);
				}
				return;
			}
			WriteImported(account, text, keep);
		},
		.confirmText = u"Import"_q,
	}));
}

// The bytes are the whole problem here. A document that is only in the cloud
// has to be fetched, and the loader hands what it fetched to whatever media
// view is active at the moment it finishes - so the view has to be held for
// the length of the download, and the subscription that is waiting on it has
// to hold itself. Both live in the lambda below and die with it.
void ResolveAndImport(
		not_null<DocumentData*> document,
		FullMsgId itemId,
		TimeId date,
		std::shared_ptr<Ui::Show> show,
		Fn<bool()> stillValid) {
	if (AskingWindowSwitched(show) || (stillValid && !stillValid())) {
		return;
	}
	const auto weakSession = base::make_weak(&document->session());
	const auto account = AccountLabel(&document->session());
	const auto media = document->createMediaView();
	if (const auto content = LocalContent(media, document);
		!content.isEmpty()) {
		ConfirmAndImport(&document->session(), content, date, show, stillValid);
		return;
	}
	document->save(itemId, QString());
	if (!document->loading()) {
		show->showBox(Ui::MakeInformBox(
			u"Could not download %1."_q.arg(SettingsFileName())));
		return;
	}
	show->showToast(u"Downloading %1..."_q.arg(SettingsFileName()));

	auto lifetime = std::make_shared<rpl::lifetime>();
	document->session().data().sessionDataAboutToBeCleared(
	) | rpl::on_next([weak = std::weak_ptr<rpl::lifetime>(lifetime)] {
		if (const auto live = weak.lock()) {
			live->destroy();
		}
	}, *lifetime);
	document->session().downloaderTaskFinished(
	) | rpl::on_next([=]() mutable {
		const auto session = weakSession.get();
		if (!session
			|| AskingWindowSwitched(show)
			|| (stillValid && !stillValid())) {
			if (AskingWindowSwitched(show)) {
				RefuseSwitchedImport(account);
			}
			base::take(lifetime)->destroy();
			return;
		}
		const auto content = LocalContent(media, document);
		if (content.isEmpty()) {
			if (!document->loading()) {
				show->showToast(
					u"Could not download %1."_q.arg(SettingsFileName()));
				base::take(lifetime)->destroy();
			}
			return;
		}
		ConfirmAndImport(&document->session(), content, date, show, stillValid);
		base::take(lifetime)->destroy();
	}, *lifetime);
}

void AddQualifiedImportAction(
		not_null<Ui::PopupMenu*> menu,
		const ImportSettingsContext &context) {
	const auto action = menu->addAction(u"Import Purple settings"_q, [=] {
		const auto document = ImportContextDocument(context, false);
		if (!document) {
			if (AskingWindowSwitched(context.show)) {
				RefuseSwitchedImport(context.account);
			}
			return;
		}
		ResolveAndImport(
			document,
			context.itemId,
			context.date,
			context.show,
			[=] { return ImportContextDocument(context, true) != nullptr; });
	}, &st::menuIconDownload);
	if (UiWitnessEnabled()) {
		if (const auto session = context.session.get()) {
			RecordUiWitnessImportAction(menu, context.asking.get(),
				session->data().message(context.itemId), context.document, action);
		}
	}
}

struct ImportCandidate {
	not_null<DocumentData*> document;
	FullMsgId itemId;
	TimeId date = 0;
};

[[nodiscard]] std::optional<ImportCandidate> FindImportCandidate(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result) {
	auto messages = (const QVector<MTPMessage>*)nullptr;
	result.match([](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		messages = &data.vmessages().v;
	});
	if (!messages) {
		return std::nullopt;
	}
	session->data().processMessages(*messages, NewMessageType::Existing);

	auto candidate = std::optional<ImportCandidate>();
	for (const auto &message : *messages) {
		const auto itemId = FullMsgId(
			PeerFromMessage(message),
			IdFromMessage(message));
		const auto item = session->data().message(itemId);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		if (!document
			|| (document->size > kMaxSize)
			|| (document->filename().compare(
				SettingsFileName(),
				Qt::CaseInsensitive) != 0)) {
			continue;
		}
		if (!candidate || (itemId.msg > candidate->itemId.msg)) {
			candidate = ImportCandidate{ document, itemId, item->date() };
		}
	}
	return candidate;
}

void OfferImportCandidate(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const ImportCandidate &candidate) {
	if (!show || !(*show)) {
		ReleaseSettingsOfferStart(session);
		return;
	}
	const auto verdict = JudgeImportOffer(
		IsActiveSession(session),
		candidate.itemId.msg.bare,
		SettingsOfferMessageId(session).bare,
		candidate.date,
		QFileInfo(SettingsFilePath()).lastModified().toSecsSinceEpoch());
	if (verdict == ImportOfferVerdict::InactiveAccount) {
		LOG(("Purple: Saved Messages settings offer dropped: "
			"the account is no longer the active one."));
		ReleaseSettingsOfferStart(session);
		return;
	} else if (verdict == ImportOfferVerdict::AlreadyOffered) {
		return;
	}
	SetSettingsOfferMessageId(session, candidate.itemId.msg);
	if (verdict == ImportOfferVerdict::NotNewer) {
		return;
	}
	const auto account = AccountLabel(session);
	const auto weakSession = base::make_weak(session.get());
	const auto expectedItem = session->data().message(candidate.itemId);
	const auto stillValid = [=] {
		const auto live = weakSession.get();
		if (!live || AskingWindowSwitched(show)) {
			return false;
		}
		const auto item = live->data().message(candidate.itemId);
		return item
			&& item == expectedItem
			&& item->date() == candidate.date
			&& item->media()
			&& item->media()->document() == candidate.document;
	};
	show->showBox(Ui::MakeConfirmBox({
		.text = u"A newer Work Mode settings file is in Saved Messages"
			"\n\nAccount %1, sent %2"_q.arg(
				account,
				FormatMoment(base::unixtime::parse(candidate.date))),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (AskingWindowSwitched(show)) {
				RefuseSwitchedImport(account);
				return;
			}
			if (!stillValid()) {
				return;
			}
			ResolveAndImport(
				candidate.document,
				candidate.itemId,
				candidate.date,
				show,
				stillValid);
		},
		.confirmText = u"Import"_q,
	}));
}

[[nodiscard]] auto SettingsSearchRequest(
		not_null<Main::Session*> session,
		const QString &query) {
	return MTPmessages_Search(
		MTP_flags(0),
		session->user()->input(),
		MTP_string(query),
		MTP_inputPeerEmpty(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTPint(),
		MTP_inputMessagesFilterDocument(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kSearchLimit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0));
}

void SearchForSettingsOffer(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		bool fallback) {
	const auto query = fallback ? QString() : SettingsFileName();
	session->api().request(SettingsSearchRequest(session, query)).done(
		crl::guard(session, [=](const MTPmessages_Messages &result) {
			if (const auto candidate = FindImportCandidate(session, result)) {
				OfferImportCandidate(session, show, *candidate);
			} else if (!fallback) {
				SearchForSettingsOffer(session, show, true);
			}
		})
	).fail(crl::guard(session, [=](const MTP::Error &error) {
		LOG(("Purple Error: Saved Messages settings search failed: %1."
			).arg(error.type()));
	})
	).send();
}

[[nodiscard]] Main::Session *AutoSendSession() {
	auto &domain = Core::App().domain();
	if (!domain.started()) {
		LOG(("Purple: nothing to send settings.toml to, not sending."));
		return nullptr;
	}
	auto signedIn = std::vector<Main::Account*>();
	for (const auto &account : domain.orderedAccounts()) {
		if (account->sessionExists()) {
			signedIn.push_back(account);
		}
	}
	const auto window = Core::App().activeWindow();
	const auto front = window ? window->maybeSession() : nullptr;
	Main::Account *frontAccount = front ? &front->account() : nullptr;
	switch (JudgeAutoSendTarget(signedIn, frontAccount)) {
	case AutoSendTarget::NoAccount:
		LOG(("Purple: nothing to send settings.toml to, not sending."));
		return nullptr;
	case AutoSendTarget::NotFirstAccount:
		LOG(("Purple: settings.toml not sent after this save: "
			"the account in front is not the first account."));
		return nullptr;
	case AutoSendTarget::Post:
		break;
	}
	return front;
}

void UploadTo(
		not_null<Main::Session*> session,
		const QByteArray &content,
		int version,
		Fn<void(std::optional<MsgId>)> finished) {
	// Built from the bytes rather than from the path, so the message carries
	// exactly what was read and validated a moment ago, and the name on it is
	// ours rather than whatever the file happens to be called on disk.
	auto file = Ui::PreparedFile(QString());
	file.content = content;
	file.displayName = SettingsFileName();
	file.size = content.size();
	file.caption = { Caption(version) };
	file.information = std::make_unique<Ui::PreparedFileInformation>();
	file.information->filemime = u"text/plain"_q;

	auto list = Ui::PreparedList();
	list.files.push_back(std::move(file));

	const auto history = session->data().history(session->user());
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		action,
		std::move(finished));
}

void NoteConfirmedSend(
		not_null<Main::Session*> session,
		const QByteArray &content,
		MsgId messageId) {
	NoteSettingsSent(content);
	if (messageId > SettingsOfferMessageId(session)) {
		SetSettingsOfferMessageId(session, messageId);
	}
}

} // namespace

DocumentData *UiWitnessQualifyingDocument(Main::Session *session, HistoryItem *item) {
	return session && item ? QualifyingImportDocument(session, item) : nullptr;
}

bool Upload(
		const QByteArray &content,
		int version,
		Fn<void(std::optional<MsgId>)> finished) {
	const auto session = AutoSendSession();
	if (!session) {
		return false;
	}
	const auto weak = base::make_weak(session);
	UploadTo(session, content, version, [=](std::optional<MsgId> messageId) {
		if (messageId) {
			if (const auto live = weak.get()) {
				NoteConfirmedSend(live, content, *messageId);
			}
		}
		if (finished) {
			finished(messageId);
		}
	});
	return true;
}

bool IsPurplePost(const FilePrepareResult &file) {
	return file.to.receipt != nullptr;
}

void SendSettingsToSavedMessages(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show) {
	const auto path = SettingsFilePath();
	auto file = QFile(path);
	if (!file.exists()) {
		show->showBox(Ui::MakeInformBox(u"No settings.toml yet"_q));
		return;
	} else if (!file.open(QIODevice::ReadOnly)) {
		show->showBox(Ui::MakeInformBox(
			u"Could not read %1. See the log."_q.arg(path)));
		return;
	}
	const auto content = file.readAll();
	const auto parsed = ParseSettings(QString::fromUtf8(content), path);
	if (!parsed.ok()) {
		// Refused rather than sent, because the caption would have to claim a
		// schema version the file does not have, and the machine importing it
		// would only find out that it is broken after replacing its own.
		show->showBox(Ui::MakeInformBox(
			u"%1 is not valid TOML, so there is nothing worth sending:"
			"\n\n%2"_q.arg(path, parsed.error)));
		return;
	}
	const auto version = parsed.settings.version;
	const auto weak = base::make_weak(session);
	show->showBox(Ui::MakeConfirmBox({
		.text = u"This posts your settings.toml to your Saved Messages, "
			"where any Purple Telegram can import it."_q,
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto live = weak.get();
			if (!live) {
				return;
			}
			UploadTo(live, content, version, crl::guard(live, [=](
					std::optional<MsgId> messageId) {
				if (messageId) {
					NoteConfirmedSend(live, content, *messageId);
					if (show && *show) {
						show->showToast(u"Sent to Saved Messages"_q);
					}
				} else if (show && *show) {
					show->showToast(u"Could not confirm settings send"_q);
				}
			}));
		},
		.confirmText = u"Send"_q,
	}));
}

bool ImportSettingsHit::keyboard() const {
	return target && target->input == ImportSettingsTarget::Input::Keyboard;
}

FullMsgId ImportSettingsHit::containerId() const {
	return target ? target->containerId : FullMsgId();
}

void CaptureImportSettingsHit(
		ImportSettingsHit *hit,
		not_null<Window::SessionController*> controller,
		HistoryView::Element *view,
		const HistoryView::TextState &state,
		HistoryView::PointState pointState,
		QPoint rawPoint,
		const ImportSettingsViewState &native,
		bool contentOwned) {
	if (!hit) {
		return;
	}
	*hit = {};
	if (!contentOwned
		|| pointState == HistoryView::PointState::Outside
		|| !HistoryContainerReady(controller, view, native)
		|| !ReceivingPointReady(native, rawPoint)) {
		return;
	}
	const auto session = &controller->session();
	const auto item = session->data().message(state.itemId);
	const auto document = QualifyingImportDocument(session, item);
	if (!document
		|| !ContentHitAgrees(state, document)
		|| !ContainerOwnsItem(session, view->data(), item)) {
		return;
	}
	auto target = std::make_shared<ImportSettingsTarget>();
	target->context = ImportContextFor(controller, item, document);
	target->native = native;
	target->view = view;
	target->containerId = view->data()->fullId();
	target->containerIdentity = view->data();
	target->rawPoint = rawPoint;
	target->input = ImportSettingsTarget::Input::Mouse;
	hit->target = std::move(target);
}

bool IsHistoryContextMenuKeyboard(
		not_null<QContextMenuEvent*> event) {
	return event->reason() == QContextMenuEvent::Keyboard;
}

std::optional<HistoryContextSelection> HistoryContextMenuSelection(
		not_null<QContextMenuEvent*> event,
		HistoryItem *focused,
		bool hasSelectedItems,
		bool focusedSelected,
		HistoryItem *selectedTextItem,
		bool hasSelectedText) {
	if (!IsHistoryContextMenuKeyboard(event)) {
		return std::nullopt;
	}
	auto result = HistoryContextSelection();
	if (hasSelectedItems) {
		result.has = 2;
		result.upon = (focused && focusedSelected) ? 2 : -2;
	} else if (focused && focused == selectedTextItem && hasSelectedText) {
		result.has = 1;
		result.upon = 1;
	}
	return result;
}

HistoryItem *HistoryContextMenuItem(
		not_null<QContextMenuEvent*> event,
		HistoryItem *focused,
		Fn<HistoryItem*()> mouse) {
	return IsHistoryContextMenuKeyboard(event) ? focused : mouse();
}

HistoryItem *HistoryContextMenuSponsored(
		not_null<QContextMenuEvent*> event,
		HistoryItem *focused,
		Fn<HistoryItem*()> mouse) {
	if (IsHistoryContextMenuKeyboard(event)) {
		return (focused && focused->isSponsored()) ? focused : nullptr;
	}
	return mouse();
}

bool PrepareHistoryContextMenuKeyboard(
		not_null<QContextMenuEvent*> event,
		not_null<HistoryView::ContextMenuRequest*> request,
		HistoryItem *focused,
		HistoryItem *selectedTextItem) {
	if (!IsHistoryContextMenuKeyboard(event)) {
		return false;
	}
	const auto item = focused;
	const auto current = item
		&& request->item == item
		&& request->view
		&& request->view->data() == item;
	request->item = current ? item : nullptr;
	request->view = current ? request->view : nullptr;
	request->pointState = current
		? HistoryView::PointState::Inside
		: HistoryView::PointState::Outside;
	const auto selectedText = current
		&& selectedTextItem == item
		&& !request->selectedText.empty();
	if (!selectedText) {
		request->selectedText = {};
		request->quote = {};
	}
	request->overSelection = false;
	if (current && !request->selectedItems.empty()) {
		for (const auto &selected : request->selectedItems) {
			if (selected.msgId == item->fullId()) {
				request->overSelection = true;
				break;
			}
		}
	} else {
		request->overSelection = selectedText;
	}
	return true;
}

bool HandleHistoryContextMenuShortcut(
		not_null<QKeyEvent*> event,
		Fn<HistoryContextMenuTarget()> current) {
#ifdef Q_OS_MAC
	if (!IsHistoryContextMenuShortcut(event)) {
		return false;
	}
	const auto target = current();
	const auto &native = target.native;
	if (!target.controller
		|| !target.focused
		|| !target.view
		|| target.view->data() != target.focused
		|| target.view->isHidden()
		|| target.view->pendingResize()
		|| &target.focused->history()->session() != &target.controller->session()
		|| target.controller->session().data().message(
			target.focused->fullId()) != target.focused
		|| !native.receiver
		|| QApplication::focusWidget() != native.receiver
		|| !native.receiver->isVisible()
		|| !native.receiver->window()->isActiveWindow()
		|| QApplication::activePopupWidget()
		|| native.geometry.top() < 0) {
		return false;
	}
	const auto visible = native.receiver->rect()
		.intersected(native.viewport)
		.intersected(native.geometry);
	if (visible.isEmpty()
		|| !native.receiver->visibleRegion().contains(visible.center())) {
		return false;
	}
	event->accept();
	if (event->isAutoRepeat()) {
		return true;
	}
	const auto receiver = QPointer<QWidget>(native.receiver);
	const auto filter = QPointer<HistoryContextMenuShortcutFilter>(
		new HistoryContextMenuShortcutFilter(
			receiver.data(),
			event->modifiers()));
	auto context = QContextMenuEvent(
		QContextMenuEvent::Keyboard,
		visible.center(),
		receiver->mapToGlobal(visible.center()),
		event->modifiers());
	QCoreApplication::sendEvent(receiver, &context);
	const auto popup = dynamic_cast<Ui::PopupMenu*>(
		QApplication::activePopupWidget());
	if (filter) {
		if (receiver && popup && popup->parentWidget() == receiver) {
			filter->bindTo(popup);
		} else {
			delete filter.data();
		}
	}
	return true;
#else
	Q_UNUSED(event);
	Q_UNUSED(current);
	return false;
#endif
}

void PrepareImportSettingsKeyboardHit(
		not_null<ImportSettingsHit*> hit,
		not_null<Window::SessionController*> controller,
		HistoryView::Element *view,
		HistoryItem *focused,
		const ImportSettingsViewState &native) {
	*hit = {};
	if (!HistoryContainerReady(controller, view, native)
		|| view->data() != focused) {
		return;
	}
	const auto document = QualifyingImportDocument(&controller->session(), focused);
	if (!document) {
		return;
	}
	auto target = std::make_shared<ImportSettingsTarget>();
	target->context = ImportContextFor(controller, focused, document);
	target->native = native;
	target->view = view;
	target->containerId = focused->fullId();
	target->containerIdentity = focused;
	target->input = ImportSettingsTarget::Input::Keyboard;
	hit->target = std::move(target);
}

void CaptureImportSettingsFileHit(
		ImportSettingsHit *hit,
		not_null<Info::AbstractController*> controller,
		Overview::Layout::ItemBase *layout,
		HistoryItem *row,
		GlobalMsgId globalId,
		const HistoryView::TextState &state,
		QPoint rawPoint,
		const ImportSettingsViewState &native,
		bool exact) {
	if (!hit) {
		return;
	}
	*hit = {};
	if (!exact
		|| !layout
		|| !ReceivingPointReady(native, rawPoint)
		|| !FilesControllerAgrees(controller, row, globalId)
		|| state.itemId != globalId.itemId
		|| state.contentOrigin != HistoryView::ContentOrigin::DocumentCard) {
		return;
	}
	const auto document = QualifyingImportDocument(&controller->session(), row);
	if (!document || !ContentHitAgrees(state, document)) {
		return;
	}
	auto target = std::make_shared<ImportSettingsTarget>();
	target->context = ImportContextFor(controller->parentController(), row, document);
	target->native = native;
	target->layout = layout;
	target->containerId = row->fullId();
	target->containerIdentity = row;
	target->globalId = globalId;
	target->rawPoint = rawPoint;
	target->input = ImportSettingsTarget::Input::Files;
	hit->target = std::move(target);
}

void AddImportSettingsAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<ImportSettingsHit*> hit,
		not_null<Window::SessionController*> controller,
		HistoryView::Element *container,
		HistoryItem *focused,
		const ImportSettingsViewState &native) {
	const auto target = hit->target;
	if (!target) {
		return;
	}
	const auto keyboard = target->input == ImportSettingsTarget::Input::Keyboard;
	const auto document = ImportContextDocument(target->context, false);
	const auto item = document
		? controller->session().data().message(target->context.itemId)
		: nullptr;
	if (target->input == ImportSettingsTarget::Input::Files
		|| target->context.asking.get() != controller
		|| !document
		|| !HistoryContainerReady(controller, container, native)
		|| target->view != container
		|| target->containerIdentity != container->data()
		|| !ContainerOwnsItem(&controller->session(), container->data(), item)
		|| native.receiver != target->native.receiver
		|| native.viewport != target->native.viewport
		|| native.geometry != target->native.geometry
		|| (keyboard && (focused != item || container->data() != focused))
		|| (!keyboard && !ReceivingPointReady(native, target->rawPoint))) {
		hit->target = nullptr;
		return;
	}
	AddQualifiedImportAction(menu, target->context);
}

void AddImportSettingsFileAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<ImportSettingsHit*> hit,
		not_null<Info::AbstractController*> controller,
		Overview::Layout::ItemBase *layout,
		HistoryItem *row,
		GlobalMsgId globalId,
		const ImportSettingsViewState &native) {
	const auto target = hit->target;
	if (!target) {
		return;
	}
	if (target->input != ImportSettingsTarget::Input::Files
		|| target->context.asking.get() != controller->parentController()
		|| !ImportContextDocument(target->context, false)
		|| !FilesControllerAgrees(controller, row, globalId)
		|| target->layout != layout
		|| target->containerIdentity != row
		|| target->globalId != globalId
		|| native.receiver != target->native.receiver
		|| native.viewport != target->native.viewport
		|| native.geometry != target->native.geometry
		|| !ReceivingPointReady(native, target->rawPoint)) {
		hit->target = nullptr;
		return;
	}
	AddQualifiedImportAction(menu, target->context);
}

void OfferNewerSettingsFromSavedMessages(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show) {
	if (!show || !(*show)
		|| !IsActiveSession(session)
		|| !TakeSettingsOfferStart(session)) {
		return;
	}
	SearchForSettingsOffer(session, std::move(show), false);
}

} // namespace Purple
