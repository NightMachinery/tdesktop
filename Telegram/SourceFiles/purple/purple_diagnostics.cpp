#include "purple/purple_diagnostics.h"

#include <QtCore/QStringList>

#include <deque>
#include <mutex>
#include <optional>

namespace Purple::Diagnostics {
namespace {

constexpr auto kMaxEntries = 256;
constexpr auto kNoticeEntries = 18;
using enum Subsystem;

struct Entry {
	Subsystem subsystem;
	QString text;
};

struct Journal {
	std::mutex mutex;
	std::deque<Entry> entries;
};

Journal &Buffer() {
	static const auto result = new Journal();
	return *result;
}

struct Rule {
	QStringView prefix;
	Subsystem subsystem;
	QStringView event;
};

constexpr Rule kLogRules[] = {
	{ u"Purple Archive: stale capture", Archive, u"Stale archive capture skipped." },
	{ u"Purple Archive:", Archive, u"Archive operation logged." },
	{ u"Purple Recording: could not write", Recording, u"Recording storage write failed." },
	{ u"Purple Recording: recovered", Recording, u"Recording crash recovery finished." },
	{ u"Purple Recording:", Recording, u"Recording operation logged." },
	{ u"Purple diag:", Sync, u"Settings sync diagnostic logged." },
	{ u"Purple: settings import dropped:", Sync, u"Incoming settings import dropped." },
	{ u"Purple Error: Settings history", Sync, u"Settings history reported a problem." },
	{ u"Purple Error: Last Seen Peek restore", Privacy, u"Last Seen Peek restoration needs attention." },
	{ u"Purple Error: Last Seen Peek", Privacy, u"Last Seen Peek reported an error." },
	{ u"Purple: Last Seen Peek", Privacy, u"Last Seen Peek operation logged." },
	{ u"Purple: screen time log pruned", Privacy, u"Screen Time pruned its event history." },
	{ u"Purple: not offering translation", Translation, u"Translation offer omitted." },
	{ u"Purple Error: Could not read ", Settings, u"A settings or state read failed." },
	{ u"Purple Error: Could not write ", Settings, u"A settings or state write failed." },
	{ u"Purple Error: Could not commit ", Settings, u"A settings or state commit failed." },
	{ u"Purple Error: settings.toml did not read back", Settings, u"The settings write did not verify." },
};

constexpr Rule kNoticeRules[] = {
	{ u"archive:discard_failed:", Archive, u"The local archive could not discard a capture." },
	{ u"archive:gap:", Archive, u"The local archive reported a capture gap." },
	{ u"archive:removal_pending", Archive, u"Local archive removal is pending." },
	{ u"archive:unavailable:", Archive, u"The local archive is unavailable." },
	{ u"config:reload:", Settings, u"Settings were reloaded." },
	{ u"sync:download_fail:", Sync, u"Settings download failed." },
	{ u"sync:send_unconfirmed:", Sync, u"Settings upload could not be confirmed." },
	{ u"sync_magic:problem:", Sync, u"Settings sync needs attention." },
	{ u"translation:excluded:", Translation, u"Translation was turned off for a chat." },
	{ u"diagnostics:export_failed", General, u"A diagnostic bundle could not be saved." },
};

Subsystem Scope(QStringView scope) {
	if (scope == u"Local archive") return Archive;
	if (scope.contains(u"record", Qt::CaseInsensitive)) return Recording;
	if (scope.contains(u"music", Qt::CaseInsensitive) || scope.contains(u"playlist", Qt::CaseInsensitive)) return Music;
	if (scope.contains(u"sync", Qt::CaseInsensitive)) return Sync;
	if (scope.contains(u"peek", Qt::CaseInsensitive) || scope.compare(u"Screen Time", Qt::CaseInsensitive) == 0) return Privacy;
	if (scope.contains(u"translat", Qt::CaseInsensitive)) return Translation;
	if (scope.contains(u"settings", Qt::CaseInsensitive) || scope.contains(u"list", Qt::CaseInsensitive)) return Settings;
	return General;
}

QStringView Name(Subsystem subsystem) {
	switch (subsystem) {
	case General: return u"General";
	case Settings: return u"Settings";
	case Sync: return u"Settings sync";
	case Archive: return u"Local archive";
	case Recording: return u"Media recording";
	case Music: return u"Music";
	case Privacy: return u"Privacy";
	case Translation: return u"Translation";
	}
	return u"General";
}

void Append(Subsystem subsystem, QStringView event) {
	auto &buffer = Buffer();
	const auto lock = std::lock_guard(buffer.mutex);
	if (buffer.entries.size() == kMaxEntries) buffer.entries.pop_front();
	buffer.entries.push_back({ subsystem, event.toString() });
}

Entry Notice(QStringView scope, QStringView key, bool error) {
	for (const auto &rule : kNoticeRules) {
		if (key.startsWith(rule.prefix)) return { rule.subsystem, rule.event.toString() };
	}
	// Only generated labels leave the app. Notice text can contain arbitrary chat or config data.
	return { Scope(scope), error ? QStringLiteral("An operation failed.")
		: QStringLiteral("An operation reported a notice.") };
}

QStringList Recent(std::optional<Subsystem> scope, int limit) {
	auto &buffer = Buffer();
	const auto lock = std::lock_guard(buffer.mutex);
	auto lines = QStringList();
	for (auto i = buffer.entries.rbegin(); i != buffer.entries.rend() && lines.size() < limit; ++i) {
		if (!scope || i->subsystem == scope) {
			lines.prepend(Name(i->subsystem).toString() + QStringLiteral(": ") + i->text);
		}
	}
	return lines;
}

} // namespace

void CaptureMainLog(QStringView line) {
	if (!line.startsWith(u"Purple")) return;
	for (const auto &rule : kLogRules) {
		if (line.startsWith(rule.prefix)) {
			Append(rule.subsystem, rule.event);
			return;
		}
	}
}

void RecordNotice(QStringView scope, QStringView key, bool warning) {
	const auto entry = Notice(scope, key, warning);
	Append(entry.subsystem, entry.text);
}

QString ForNotice(QStringView scope, QStringView key, bool error) {
	const auto notice = Notice(scope, key, error);
	auto recent = Recent(notice.subsystem, kNoticeEntries);
	if (recent.isEmpty()) recent.push_back(QStringLiteral("No recent diagnostics in this session."));
	return QStringLiteral("%1 notice\n%2\n\nRecent diagnostics\n%3\n")
		.arg(Name(notice.subsystem), notice.text, recent.join(u'\n'));
}

QString ExportText() {
	const auto lines = Recent(std::nullopt, kMaxEntries);
	return lines.isEmpty() ? QStringLiteral("No recent diagnostics in this session.\n")
		: lines.join(u'\n') + u'\n';
}

void Clear() {
	auto &buffer = Buffer();
	const auto lock = std::lock_guard(buffer.mutex);
	buffer.entries.clear();
}

} // namespace Purple::Diagnostics
