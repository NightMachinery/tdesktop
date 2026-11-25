#include "purple/purple_diagnostics_ui.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/version.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_diagnostics.h"
#include "purple/purple_notices.h"
#include "purple/purple_privacy_screen.h"
#include "purple/purple_version.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

namespace Purple::Diagnostics {
namespace {

auto CurrentWindow(not_null<Window::Controller*> window) {
	const auto weak = base::make_weak(window.get());
	const auto session = base::make_weak(window->maybeSession());
	return [=]() -> Window::Controller* {
		const auto host = weak.get();
		const auto live = session.get();
		return live && CanManage(host) && host->maybeSession() == live ? host : nullptr;
	};
}

QString CanonicalOrAbsolute(const QFileInfo &info) {
	const auto canonical = info.canonicalFilePath();
	return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool InDirectory(const QString &path, const QString &directory) {
	return path.compare(directory, Qt::CaseInsensitive) == 0
		|| path.startsWith(directory + u'/', Qt::CaseInsensitive);
}

bool IsAppDataPath(const QString &selected) {
	const auto info = QFileInfo(selected);
	const auto directory = CanonicalOrAbsolute(QFileInfo(info.absolutePath()));
	const auto own = CanonicalOrAbsolute(QFileInfo(QDir(cWorkingDir()).absolutePath()));
	const auto config = CanonicalOrAbsolute(QFileInfo(QDir(ConfigDirectory()).absolutePath()));
	const auto proposed = directory + u'/' + info.fileName();
	for (const auto &path : { proposed, CanonicalOrAbsolute(info) }) {
		if (InDirectory(path, config)
			|| InDirectory(path, own + u"/DebugLogs"_q)
			|| InDirectory(path, own + u"/tdata"_q)) {
			return true;
		}
		const auto file = QFileInfo(path);
		if (file.absolutePath().compare(own, Qt::CaseInsensitive) == 0) {
			const auto name = file.fileName().toLower();
			if (name == u"log.txt" || (name.startsWith(u"log_start") && name.endsWith(u".txt"))) {
				return true;
			}
		}
	}
	return false;
}

void ExportFailure(not_null<Window::Controller*> window, QString summary) {
	ShowPurpleNotice({
		.session = window->maybeSession(),
		.window = window.get(),
		.scope = u"Diagnostics"_q,
		.summary = std::move(summary),
		.coalesceKey = u"diagnostics:export_failed"_q,
		.severity = ModalNotices::NoticeSeverity::Error,
	});
}

void Acknowledge(not_null<Window::Controller*> window, QString text) {
	ShowPurpleAck({
		.session = window->maybeSession(),
		.window = window.get(),
		.scope = u"Diagnostics"_q,
		.text = std::move(text),
	});
}

} // namespace

bool CanManage(Window::Controller *window) {
	const auto session = window ? window->maybeSession() : nullptr;
	return session && !session->account().loggingOut()
		&& Core::App().activeWindow() == window
		&& !window->locked()
		&& !Core::App().passcodeLocked()
		&& !Core::App().screenIsLocked()
		&& !PrivacyScreenActiveAny();
}

void ShowExport(not_null<Window::Controller*> window) {
	if (!CanManage(window)) {
		return;
	}
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		u"Export Purple diagnostics"_q,
		u"Text files (*.txt)"_q,
		u"purple-diagnostics.txt"_q,
		[current = CurrentWindow(window)](QString &&path) {
			const auto host = current();
			if (!host || path.isEmpty()) {
				return;
			}
			if (IsAppDataPath(path)) {
				ExportFailure(host, u"Choose a file outside the app's settings, data, and log paths."_q);
				return;
			}
			const auto bytes = (u"Purple diagnostics\n%1\nTelegram Desktop %2\nQt %3\n\n%4"_q
				.arg(VersionText(), QString::fromLatin1(AppVersionStr),
					QString::fromLatin1(qVersion()), ExportText())).toUtf8();
			auto file = QSaveFile(path);
			if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
				|| !file.commit()) {
				ExportFailure(host, u"Could not save the diagnostic bundle."_q);
				return;
			}
			Acknowledge(host, u"Purple diagnostics exported."_q);
		});
}

void ShowClear(not_null<Window::Controller*> window) {
	if (!CanManage(window)) {
		return;
	}
	window->show(Ui::MakeConfirmBox({
		.text = rpl::single(u"Clear the recent Purple diagnostics used by Copy logs and "
			"Export logs? Telegram's log files and Screen Time history are kept."_q),
		.confirmed = [current = CurrentWindow(window)](Fn<void()> close) {
			close();
			if (const auto host = current()) {
				Clear();
				Acknowledge(host, u"Purple diagnostics cleared."_q);
			}
		},
		.confirmText = u"Clear logs"_q,
	}));
}

} // namespace Purple::Diagnostics
