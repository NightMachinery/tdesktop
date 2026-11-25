#include "purple/purple_diagnostics.h"
#include "purple/purple_notice_state.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // Q_OS_UNIX

namespace Purple::Diagnostics {
namespace {

constexpr auto kNoticeLines = 60;
constexpr auto kExportNotices = 32;
constexpr auto kBackup = u".1";

Journal &Global() {
	static const auto result = new Journal();
	return *result;
}

QString Scope(QStringView scope) {
	if (scope.contains(u"archive", Qt::CaseInsensitive)) return QStringLiteral("Archive");
	if (scope.contains(u"record", Qt::CaseInsensitive)) return QStringLiteral("Recording");
	if (scope.contains(u"music", Qt::CaseInsensitive) || scope.contains(u"playlist", Qt::CaseInsensitive)
		|| scope.contains(u"songs", Qt::CaseInsensitive)) return QStringLiteral("Music");
	if (scope.contains(u"sync", Qt::CaseInsensitive)) return QStringLiteral("Sync");
	if (scope.contains(u"peek", Qt::CaseInsensitive) || scope.contains(u"screen time", Qt::CaseInsensitive)
		|| scope.contains(u"privacy", Qt::CaseInsensitive)) return QStringLiteral("Privacy");
	if (scope.contains(u"translat", Qt::CaseInsensitive)) return QStringLiteral("Translation");
	if (scope.contains(u"work mode", Qt::CaseInsensitive)) return QStringLiteral("Work Mode");
	if (scope.contains(u"settings", Qt::CaseInsensitive) || scope.contains(u"list", Qt::CaseInsensitive)) {
		return QStringLiteral("Settings");
	}
	return QStringLiteral("General");
}

bool Open(QFile &file, QIODevice::OpenMode mode) {
#ifdef Q_OS_UNIX
	const auto writing = bool(mode & QIODevice::WriteOnly);
	const auto flags = writing ? O_RDWR | O_CREAT | O_APPEND : O_RDONLY;
	const auto fd = ::open(QFile::encodeName(file.fileName()).constData(),
		flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return false;
	}
	struct stat info;
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
		|| info.st_uid != ::geteuid() || info.st_nlink != 1
		|| (writing && ::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
		|| !file.open(fd, mode, QFileDevice::AutoCloseHandle)) {
		::close(fd);
		return false;
	}
	return true;
#else
	if (QFileInfo(file).isSymLink() || !file.open(mode)) {
		return false;
	}
	return !(mode & QIODevice::WriteOnly)
		|| file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif // Q_OS_UNIX
}

template <typename Callback>
QString ReplaceMatches(
		QStringView text,
		const QRegularExpression &pattern,
		Callback replace) {
	auto result = QString();
	auto offset = qsizetype(0);
	for (const auto &match : pattern.globalMatchView(text)) {
		result += text.mid(offset, match.capturedStart() - offset);
		result += replace(match);
		offset = match.capturedEnd();
	}
	result += text.mid(offset);
	return result;
}

QString Mask(QString text) {
	static const auto home = QDir::toNativeSeparators(QDir::homePath());
	static const auto otherHome = QRegularExpression(QStringLiteral(
		R"((?:/Users/|/home/|[A-Za-z]:[/\\]Users[/\\])[^\s/\\'"<>]+)"));
	static const auto secrets = QRegularExpression(QStringLiteral(
		R"rx((\b(?:authorization|proxy[-_]authorization|(?:api|auth|access|refresh|bot|session|client|private|login)[-_ ]?(?:key(?:[-_ ]?id)?|hash|token|secret)|key(?:[-_ ]?id)?|token|password|passcode|secret|cookie|phone(?:[-_ ]?number)?)\s*(?:=|:)\s*)(?:Bearer\s+[^\s,;]+|"[^"\r\n]*"|'[^'\r\n]*'|[^\s,;]+))rx"),
		QRegularExpression::CaseInsensitiveOption);
	static const auto pem = QRegularExpression(QStringLiteral(
		R"(-----BEGIN [^-\r\n]*PRIVATE KEY-----[\s\S]*?(?:-----END [^-\r\n]*PRIVATE KEY-----|$))"));
	static const auto contact = QRegularExpression(QStringLiteral(
		R"((?:\bBearer\s+[A-Za-z0-9._~+/=-]{8,}|\b\d{5,15}:[A-Za-z0-9_-]{20,}|\b(?:AIza[A-Za-z0-9_-]{30,}|sk-[A-Za-z0-9_-]{20,})|[\w.%+-]+@[\w.-]+\.[A-Za-z]{2,}|(?<!\w)\+(?:\d[ ().-]?){6,14}\d(?!\w)|\b\d{3}[- ]\d{3}[- ]\d{4}\b|\(\d{3}\)[ -]\d{3}[- ]\d{4}\b))"),
		QRegularExpression::CaseInsensitiveOption);
	if (!home.isEmpty() && home != QStringLiteral("/")) {
		text.replace(home + QDir::separator(), QStringLiteral("~/"));
	}
	text.replace(otherHome, QStringLiteral("~"));
	text.replace(pem, QStringLiteral("[masked]"));
	text.replace(contact, QStringLiteral("[masked]"));
	text = ReplaceMatches(text, secrets, [](const auto &match) {
		return match.captured(1) + QStringLiteral("[masked]");
	});
	return text;
}

QString MarkId(QString number) {
	static const auto home = QDir::homePath().toUtf8();
	number.remove(u'_');
	if (number.startsWith(u'+')) {
		number.remove(0, 1);
	}
	const auto hash = QCryptographicHash::hash(
		home + '\0' + number.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
	return QStringLiteral("[[id:%1]]").arg(QString::fromLatin1(hash));
}

QString MarkIds(QStringView text) {
	static const auto ids = QRegularExpression(QStringLiteral(
		R"((\b(?:peer|chat|account|user|channel|message|msg|topic|thread|root)(?:[ _-]?id)?(?:\s*[:=#]\s*|\s+))([+-]?\d+)(?![\d\w]))"),
		QRegularExpression::CaseInsensitiveOption);
	return ReplaceMatches(text, ids, [](const auto &match) {
		return match.captured(1) + MarkId(match.captured(2));
	});
}

QString Share(QStringView text) {
	static const auto markers = QRegularExpression(QStringLiteral(R"(\[\[id:([0-9a-f]{16})\]\])"));
	auto aliases = QHash<QString, QString>();
	return ReplaceMatches(text, markers, [&](const auto &match) {
		const auto key = match.captured(1);
		const auto next = QStringLiteral("id-%1").arg(aliases.size() + 1);
		return aliases.tryEmplace(key, next).iterator.value();
	});
}

QString NoticeBody(ModalNotices::NoticeRecord record) {
	static const auto bareIds = QRegularExpression(QStringLiteral(
		R"((?<=[\[,])(\s*)([+-]?\d[\d_]*)(?=\s*[,\]]))"));
	for (auto &row : record.rows) {
		if (row.hasConfigPeerIds) {
			const auto mask = [](const QString &text) {
				return ReplaceMatches(text, bareIds, [](const auto &match) {
					return match.captured(1) + MarkId(match.captured(2));
				});
			};
			row.before = mask(row.before);
			row.after = mask(row.after);
		}
	}
	auto text = record.plainText();
	if (record.repeatCount > 1) {
		text += QStringLiteral("\nRepeated %1 times").arg(record.repeatCount);
	}
	return text;
}

} // namespace

Journal::Journal(qint64 segmentBytes) : _segmentBytes(segmentBytes) {
}

void Journal::start(const QString &directory) {
	const auto lock = std::lock_guard(_mutex);
	_path = directory.isEmpty() ? QString() : QDir(directory).filePath(QString::fromLatin1(kLogFile));
}

bool Journal::append(QStringView line) {
	if (!line.startsWith(u"Purple ") && !line.startsWith(u"Purple:")) {
		return false;
	}
	const auto colon = line.indexOf(u':');
	if (colon < 0 || colon > 40) {
		return false;
	}
	const auto subsystem = line.mid(6, colon - 6).trimmed();
	auto text = MarkIds(Mask(line.mid(colon + 1).trimmed().toString()));
	text.replace(u'\n', u' ');
	text.replace(u'\r', u' ');
	const auto bytes = QStringLiteral("%1 [%2] %3\n")
		.arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
			subsystem.isEmpty() ? QStringLiteral("General") : subsystem.toString(), text)
		.toUtf8();
	if (bytes.size() > std::min<qint64>(_segmentBytes, 16 * 1024)) {
		return false;
	}
	const auto lock = std::lock_guard(_mutex);
	if (_path.isEmpty()) {
		return false;
	}
	auto file = QFile(_path);
	if (!Open(file, QIODevice::ReadWrite | QIODevice::Append)) {
		return false;
	}
	if (file.size() + bytes.size() > _segmentBytes) {
		file.close();
		const auto backup = _path + kBackup;
		auto prior = QFile(backup);
		if ((QFileInfo(prior).exists() || QFileInfo(prior).isSymLink()) && !prior.remove()) {
			return false;
		}
		file.setFileName(_path);
		if (!QFile::rename(_path, backup)
			|| !Open(file, QIODevice::ReadWrite | QIODevice::Append)) {
			return false;
		}
	}
	const auto offset = file.size();
	if (file.write(bytes) == bytes.size() && file.flush()) {
		return true;
	}
	if (!file.resize(offset)) {
		file.close();
	}
	return false;
}

QString Journal::read() const {
	if (_path.isEmpty()) {
		return {};
	}
	auto result = QString();
	for (const auto &path : { _path + kBackup, _path }) {
		auto file = QFile(path);
		if (Open(file, QIODevice::ReadOnly)
			&& file.seek(std::max(qint64(0), file.size() - _segmentBytes))) {
			const auto bytes = file.read(_segmentBytes);
			const auto last = bytes.lastIndexOf('\n');
			if (last >= 0) {
				const auto first = file.size() > _segmentBytes ? bytes.indexOf('\n') + 1 : 0;
				result += QString::fromUtf8(bytes.mid(first, last + 1 - first));
			}
		}
	}
	return result;
}

QString Journal::copy(QStringView scope, QStringView notice) {
	const auto lock = std::lock_guard(_mutex);
	const auto marker = QStringLiteral(" [%1] ").arg(Scope(scope));
	const auto lines = read().split(u'\n', Qt::SkipEmptyParts);
	auto recent = QStringList();
	for (auto i = lines.rbegin(); i != lines.rend() && recent.size() < kNoticeLines; ++i) {
		const auto stampEnd = i->indexOf(u' ');
		if (stampEnd >= 0 && QStringView(*i).mid(stampEnd).startsWith(marker)) {
			recent.prepend(*i);
		}
	}
	return Share(MarkIds(Mask(notice.toString())) + QStringLiteral("\n\nRecent diagnostics\n")
		+ (recent.isEmpty() ? QStringLiteral("No recent lines for this subsystem.") : recent.join(u'\n'))
		+ u'\n');
}

QString Journal::exportText(QStringView notices) {
	const auto lock = std::lock_guard(_mutex);
	return Share(read() + u'\n' + MarkIds(Mask(notices.toString())));
}

bool Journal::clear() {
	const auto lock = std::lock_guard(_mutex);
	if (_path.isEmpty()) {
		return false;
	}
	auto file = QFile(_path);
	auto prior = QFile(_path + kBackup);
	const auto mode = QIODevice::ReadWrite | QIODevice::Append;
	return Open(file, mode) && Open(prior, mode) && prior.resize(0) && file.resize(0);
}

void Start(const QString &directory) {
	Global().start(directory);
}

void CaptureMainLog(QStringView line) {
	Global().append(line);
}

QString ForNotice(const ModalNotices::NoticeRecord &notice) {
	return Global().copy(notice.scope, NoticeBody(notice));
}

QString ExportText(QStringView notices) {
	return Global().exportText(notices);
}

QString ExportNotices(
		std::span<const ModalNotices::NoticeRecord> account,
		std::span<const ModalNotices::NoticeRecord> app) {
	struct Item {
		const ModalNotices::NoticeRecord *record;
		bool app;
	};
	auto items = std::vector<Item>();
	for (const auto &record : account) {
		items.push_back({ &record, false });
	}
	for (const auto &record : app) {
		items.push_back({ &record, true });
	}
	std::ranges::sort(items, {}, [](const Item &item) {
		return std::pair(item.record->timestampMs, item.record->id);
	});
	auto result = QStringLiteral("Recent notices (this account and the app)\n");
	const auto offset = items.size() > kExportNotices ? items.size() - kExportNotices : 0;
	for (const auto &item : std::span(items).subspan(offset)) {
		result += item.app ? QStringLiteral("\n[App] ") : QStringLiteral("\n[This account] ");
		result += NoticeBody(*item.record) + u'\n';
	}
	return items.empty() ? result + QStringLiteral("No recent notices.\n") : result;
}

bool Clear() {
	return Global().clear();
}

} // namespace Purple::Diagnostics
