/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "purple/purple_config.h"
#include "storage/localimageloader.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include "styles/style_menu_icons.h"

namespace Purple {
namespace {

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
		const std::shared_ptr<Ui::Show> &show) {
	const auto account = AccountLabel(session);
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
	show->showBox(Ui::MakeConfirmBox({
		.text = lines.join('\n'),
		.confirmed = [=](Fn<void()> close) {
			close();
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
		std::shared_ptr<Ui::Show> show) {
	const auto media = document->createMediaView();
	if (const auto content = LocalContent(media, document);
		!content.isEmpty()) {
		ConfirmAndImport(&document->session(), content, date, show);
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
	document->session().downloaderTaskFinished(
	) | rpl::on_next([=]() mutable {
		const auto content = LocalContent(media, document);
		if (content.isEmpty()) {
			if (!document->loading()) {
				show->showToast(
					u"Could not download %1."_q.arg(SettingsFileName()));
				base::take(lifetime)->destroy();
			}
			return;
		}
		ConfirmAndImport(&document->session(), content, date, show);
		base::take(lifetime)->destroy();
	}, *lifetime);
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
	auto &settings = session->settings();
	if (!show || !(*show)) {
		settings.releasePurpleSettingsOfferStart();
		return;
	}
	const auto verdict = JudgeImportOffer(
		IsActiveSession(session),
		candidate.itemId.msg.bare,
		settings.purpleSettingsOfferMessageId().bare,
		candidate.date,
		QFileInfo(SettingsFilePath()).lastModified().toSecsSinceEpoch());
	if (verdict == ImportOfferVerdict::InactiveAccount) {
		LOG(("Purple: Saved Messages settings offer dropped: "
			"the account is no longer the active one."));
		settings.releasePurpleSettingsOfferStart();
		return;
	} else if (verdict == ImportOfferVerdict::AlreadyOffered) {
		return;
	}
	settings.setPurpleSettingsOfferMessageId(candidate.itemId.msg);
	session->saveSettings();
	if (verdict == ImportOfferVerdict::NotNewer) {
		return;
	}
	const auto account = AccountLabel(session);
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
			ResolveAndImport(
				candidate.document,
				candidate.itemId,
				candidate.date,
				show);
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
	auto &settings = session->settings();
	if (messageId > settings.purpleSettingsOfferMessageId()) {
		settings.setPurpleSettingsOfferMessageId(messageId);
		session->saveSettings();
	}
}

} // namespace

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

void AddImportSettingsAction(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		not_null<DocumentData*> document,
		std::shared_ptr<Ui::Show> show) {
	if (!item || !item->history()->peer->isSelf()) {
		return;
	} else if (document->size > kMaxSize) {
		return;
	} else if (document->filename().compare(
			SettingsFileName(),
			Qt::CaseInsensitive) != 0) {
		return;
	}
	const auto itemId = item->fullId();
	const auto date = item->date();
	menu->addAction(u"Import Purple settings"_q, [=] {
		ResolveAndImport(document, itemId, date, show);
	}, &st::menuIconDownload);
}

void OfferNewerSettingsFromSavedMessages(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show) {
	if (!show || !(*show)
		|| !IsActiveSession(session)
		|| !session->settings().takePurpleSettingsOfferStart()) {
		return;
	}
	SearchForSettingsOffer(session, std::move(show), false);
}

} // namespace Purple
