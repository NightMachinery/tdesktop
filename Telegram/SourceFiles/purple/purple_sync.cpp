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
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "storage/localimageloader.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>

#include "styles/style_menu_icons.h"

namespace Purple {
namespace {

constexpr auto kFileName = "settings.toml";

// A settings.toml is a few kilobytes of TOML, and this fetches one into memory
// rather than onto disk. Anything of a size that could not be the file is not
// the file, and offering to import it would be offering to import something
// the download path cannot even hold.
constexpr auto kMaxSize = 4 * 1024 * 1024;

[[nodiscard]] QString SettingsFileName() {
	return QString::fromLatin1(kFileName);
}

[[nodiscard]] QString BackupFilePath() {
	return SettingsFilePath() + u".bak"_q;
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

void WriteImported(
		const QString &text,
		const std::shared_ptr<Ui::Show> &show) {
	const auto path = SettingsFilePath();
	if (QFile::exists(path)) {
		// One backup, overwritten. A numbered series would accumulate in a
		// directory the user reads by hand, and the file it protects against
		// losing is one Saved Messages is already keeping every version of.
		QFile::remove(BackupFilePath());
		if (!QFile::copy(path, BackupFilePath())) {
			LOG(("Purple Error: Could not back up %1.").arg(path));
		}
	}
	if (!WriteConfigFile(path, text)) {
		show->showBox(Ui::MakeInformBox(
			u"Could not write %1. See the log."_q.arg(path)));
		return;
	}
	// Nothing else to do: the directory watcher sees the write and reloads,
	// exactly as it would for an edit made in a text editor.
	show->showToast(u"Settings imported"_q);
}

void ConfirmAndImport(
		const QByteArray &content,
		TimeId date,
		const std::shared_ptr<Ui::Show> &show) {
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
	lines.push_back(u"Import the settings sent %1?"_q.arg(
		FormatMoment(base::unixtime::parse(date))));
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
		"kept as %1."_q.arg(SettingsFileName() + u".bak"_q));

	const auto keep = show;
	show->showBox(Ui::MakeConfirmBox({
		.text = lines.join('\n'),
		.confirmed = [=](Fn<void()> close) {
			close();
			WriteImported(text, keep);
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
		ConfirmAndImport(content, date, show);
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
		ConfirmAndImport(content, date, show);
		base::take(lifetime)->destroy();
	}, *lifetime);
}

void Upload(
		not_null<Main::Session*> session,
		const QByteArray &content,
		int version) {
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
		action);
}

} // namespace

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
	show->showBox(Ui::MakeConfirmBox({
		.text = u"This posts your settings.toml to your Saved Messages, "
			"where any Purple Telegram can import it."_q,
		.confirmed = [=](Fn<void()> close) {
			close();
			Upload(session, content, version);
			show->showToast(u"Sent to Saved Messages"_q);
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

} // namespace Purple
