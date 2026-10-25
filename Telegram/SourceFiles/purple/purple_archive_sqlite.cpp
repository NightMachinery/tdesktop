#include "purple/purple_archive_sqlite.h"
#include "purple_sqlite.h"
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <cctype>
#include <limits>

namespace Purple {
namespace {

DesktopArchiveSqlResult Failure(int code, QString phase) {
	auto result = DesktopArchiveSqlResult();
	result.code = code;
	result.failure = std::move(phase);
	return result;
}

}

class DesktopArchiveSqlite::Impl {
  public:
	sqlite3* db = nullptr;
	QThread* thread = nullptr;
	bool poisoned = false;
	bool configured = false;
	std::function<bool()> active;
	QString fault;
	int faultCount = 0;
	bool Fail(const QString &phase) { return fault == phase && --faultCount == 0; }
	bool Current() const { return db && !poisoned && thread == QThread::currentThread()
		&& (!active || active()); }
	DesktopArchiveSqlResult Run(const ArchiveSqlStatement &spec, bool collect) {
		if (!Current() || spec.sql.isEmpty()) {
			return Failure(SQLITE_MISUSE, u"invalid-statement"_q);
		}
		const auto sql = spec.sql.toUtf8();
		if (sql.contains(char(0)) || sql.size() > std::numeric_limits<int>::max()) {
			return Failure(SQLITE_TOOBIG, u"sql-limit"_q);
		}
		auto statement = static_cast<sqlite3_stmt*>(nullptr);
		auto tail = static_cast<const char*>(nullptr);
		auto code = Fail(u"prepare"_q) ? SQLITE_IOERR
									   : sqlite3_prepare_v3(db, sql.constData(),
											 int(sql.size()), 0, &statement, &tail);
		if (code != SQLITE_OK || !statement) {
			if (statement && sqlite3_finalize(statement) != SQLITE_OK) {
				poisoned = true;
			}
			return Failure(code == SQLITE_OK ? SQLITE_MISUSE : code, u"prepare"_q);
		}
		while (tail && *tail && std::isspace(static_cast<unsigned char>(*tail))) {
			++tail;
		}
		auto result = DesktopArchiveSqlResult();
		const auto total = sqlite3_total_changes64(db);
		auto rowCount = int64(0);
		auto byteCount = uint64_t(0);
		if ((tail && *tail)
			|| sqlite3_bind_parameter_count(statement) != int(spec.binds.size())) {
			code = SQLITE_MISUSE;
		} else {
			for (auto i = size_t(0); i != spec.binds.size(); ++i) {
				code = std::visit(
					[&](const auto &value) {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, std::nullptr_t>) {
							return sqlite3_bind_null(statement, int(i + 1));
						} else if constexpr (std::is_same_v<T, int64>) {
							return sqlite3_bind_int64(statement, int(i + 1), value);
						} else if constexpr (std::is_same_v<T, QString>) {
							const auto bytes = value.toUtf8();
							return sqlite3_bind_text64(statement, int(i + 1),
								bytes.constData(), bytes.size(), SQLITE_TRANSIENT,
								SQLITE_UTF8);
						} else {
							return sqlite3_bind_blob64(statement, int(i + 1),
								value.isEmpty() ? "" : value.constData(), value.size(),
								SQLITE_TRANSIENT);
						}
					},
					spec.binds[i]);
				if (Fail(u"bind"_q)) {
					code = SQLITE_IOERR;
				}
				if (code != SQLITE_OK) {
					break;
				}
			}
		}
		if (code == SQLITE_OK) {
			while ((code = Fail(u"step"_q) ? SQLITE_IOERR : sqlite3_step(statement))
				   == SQLITE_ROW) {
				if (!Current()) {
					code = SQLITE_INTERRUPT;
					break;
				}
				++rowCount;
				if (rowCount > spec.rows.maximum
					|| (collect && rowCount > kArchiveSqlPageLimit)) {
					code = SQLITE_TOOBIG;
					break;
				}
				auto row = std::vector<ArchiveSqlValue>();
				const auto columns = sqlite3_column_count(statement);
				if (columns > 64) {
					code = SQLITE_TOOBIG;
					break;
				}
				for (auto i = 0; i != columns; ++i) {
					const auto type = sqlite3_column_type(statement, i);
					if (type == SQLITE_NULL) {
						row.emplace_back(nullptr);
					} else if (type == SQLITE_INTEGER) {
						row.emplace_back(int64(sqlite3_column_int64(statement, i)));
					} else if (type == SQLITE_TEXT || type == SQLITE_BLOB) {
						const auto size = sqlite3_column_bytes(statement, i);
						const auto data = type == SQLITE_TEXT
											  ? reinterpret_cast<const void*>(
													sqlite3_column_text(statement, i))
											  : sqlite3_column_blob(statement, i);
						if (size < 0 || (size && !data)
							|| uint64_t(size) > kArchiveSqlBindByteLimit - byteCount) {
							code = SQLITE_TOOBIG;
							break;
						}
						byteCount += size;
						if (sqlite3_errcode(db) == SQLITE_NOMEM) {
							code = SQLITE_NOMEM;
							break;
						}
						const auto bytes
							= QByteArray(static_cast<const char*>(data), size);
						if (type == SQLITE_BLOB) {
							row.emplace_back(bytes);
						} else {
							const auto text = QString::fromUtf8(bytes);
							if (text.toUtf8() != bytes) {
								code = SQLITE_CORRUPT;
								break;
							}
							row.emplace_back(text);
						}
					} else {
						code = SQLITE_MISMATCH;
						break;
					}
				}
				if (code != SQLITE_ROW) {
					break;
				}
				if (collect) {
					result.rows.push_back(std::move(row));
				}
			}
		}
		const auto direct = sqlite3_changes64(db);
		const auto delta = sqlite3_total_changes64(db) - total;
		const auto finalized = sqlite3_finalize(statement);
		if (finalized != SQLITE_OK || Fail(u"finalize"_q)) {
			return Failure(
				finalized == SQLITE_OK ? SQLITE_IOERR : finalized, u"finalize"_q);
		}
		if (!Current()) {
			return Failure(SQLITE_INTERRUPT, u"revoked"_q);
		}
		if (code != SQLITE_DONE) {
			return Failure(code, u"step-or-bind"_q);
		}
		if (!CheckArchiveSqlResult(spec, true, rowCount, direct, delta)) {
			return Failure(SQLITE_CONSTRAINT, u"expected-count"_q);
		}
		result.ok = true;
		result.code = SQLITE_OK;
		return result;
	}

};

void DesktopArchiveSqlite::SetActive(std::function<bool()> active) {
	_impl->active = std::move(active);
}

DesktopArchiveSqlite::DesktopArchiveSqlite() : _impl(std::make_unique<Impl>()) {}
DesktopArchiveSqlite::~DesktopArchiveSqlite() { static_cast<void>(CloseFinal()); }

bool DesktopArchiveSqlite::CloseFinal() {
	if (Close()) {
		return true;
	}
	if (_impl->db && _impl->thread == QThread::currentThread()
		&& sqlite3_close_v2(_impl->db) == SQLITE_OK) {
		_impl->db = nullptr;
		_impl->configured = false;
		return true;
	}
	return false;
}

DesktopArchiveSqlResult DesktopArchiveSqlite::Open(
	const QString &path, bool create, bool readOnly) {
	if (_impl->db || path.contains(QChar(0))) {
		return Failure(SQLITE_MISUSE, u"already-open"_q);
	}
	_impl->thread = QThread::currentThread();
	const auto name = QFile::encodeName(path);
	const auto code = sqlite3_open_v2(name.constData(), &_impl->db,
		(readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_NOMUTEX
			| SQLITE_OPEN_NOFOLLOW | (create ? SQLITE_OPEN_CREATE : 0),
		nullptr);
	if (code != SQLITE_OK) {
		_impl->poisoned = true;
		static_cast<void>(Close());
		return Failure(code, u"open"_q);
	}
	if (sqlite3_extended_result_codes(_impl->db, 1) != SQLITE_OK
		|| sqlite3_busy_timeout(_impl->db, 0) != SQLITE_OK) {
		static_cast<void>(Close());
		return Failure(SQLITE_ERROR, u"configure-api"_q);
	}
	auto result = DesktopArchiveSqlResult();
	result.ok = true;
	return result;
}

DesktopArchiveSqlResult DesktopArchiveSqlite::Configure() {
	_impl->configured = false;
	for (const auto &sql : ArchiveSqlConnectionSetup()) {
		auto spec = ArchiveSqlStatement{sql, {},
			sql.contains(u"journal_mode"_q) ? ArchiveSqlExpectation::OneRow
											: ArchiveSqlExpectation::Done};
		const auto result = _impl->Run(spec, true);
		if (!result.ok) {
			return result;
		}
	}
	for (const auto &[sql, expected] : std::vector<std::pair<QString, ArchiveSqlValue>>{
			 {u"PRAGMA foreign_keys"_q, int64(1)}, {u"PRAGMA synchronous"_q, int64(2)},
			 {u"PRAGMA journal_mode"_q, u"wal"_q}}) {
		const auto result = Read({sql, {}, ArchiveSqlExpectation::OneRow});
		if (!result.ok || result.rows.size() != 1 || result.rows.front().size() != 1
			|| result.rows.front().front() != expected) {
			return Failure(SQLITE_ERROR, u"pragma-readback"_q);
		}
	}
	_impl->configured = true;
	auto result = DesktopArchiveSqlResult();
	result.ok = true;
	return result;
}

DesktopArchiveSqlResult DesktopArchiveSqlite::Read(const ArchiveSqlStatement &statement) {
	if (!_impl->Current()) {
		return Failure(SQLITE_MISUSE, u"not-open"_q);
	}
	return _impl->Run(statement, true);
}

DesktopArchiveSqlResult DesktopArchiveSqlite::Execute(const ArchiveSqlProgram &program) {
	if (!_impl->Current() || !_impl->configured || !program
		|| !IsBoundedArchiveSqlProgram(program)) {
		return Failure(SQLITE_MISUSE, u"invalid-program"_q);
	}
	if (program.statements.empty()) {
		auto result = DesktopArchiveSqlResult();
		result.ok = true;
		return result;
	}
	if (program.statements.front().sql != u"BEGIN IMMEDIATE"_q
		|| program.statements.back().sql != u"COMMIT"_q
		|| !sqlite3_get_autocommit(_impl->db)) {
		return Failure(SQLITE_MISUSE, u"transaction-shape"_q);
	}
	for (const auto &statement : program.statements) {
		auto result = statement.sql == u"COMMIT"_q && _impl->Fail(u"commit"_q)
						  ? Failure(SQLITE_IOERR, u"commit"_q)
						  : _impl->Run(statement, false);
		if (!result.ok) {
			if (!sqlite3_get_autocommit(_impl->db)
				&& !_impl->Run({u"ROLLBACK"_q}, false).ok) {
				_impl->poisoned = true;
			}
			return result;
		}
	}
	auto result = DesktopArchiveSqlResult();
	result.ok = result.committed
		= AcknowledgeArchiveSqlCommit(true, sqlite3_get_autocommit(_impl->db) != 0);
	return result;
}

bool DesktopArchiveSqlite::BeginRead() {
	if (!_impl->Current() || !sqlite3_get_autocommit(_impl->db)) {
		return false;
	}
	const auto ok = _impl->Run({u"BEGIN"_q}, false).ok;
	if (!ok && !sqlite3_get_autocommit(_impl->db)
		&& !_impl->Run({u"ROLLBACK"_q}, false).ok) {
		_impl->poisoned = true;
	}
	return ok;
}

bool DesktopArchiveSqlite::EndRead() {
	if (!_impl->Current() || sqlite3_get_autocommit(_impl->db)) {
		return false;
	}
	const auto result = _impl->Run({u"ROLLBACK"_q}, false).ok;
	if (!result) {
		_impl->poisoned = true;
	}
	return result;
}

bool DesktopArchiveSqlite::Close() {
	if (!_impl->db) {
		return true;
	}
	if (_impl->thread != QThread::currentThread() || _impl->Fail(u"close"_q)) {
		return false;
	}
	const auto code = sqlite3_close(_impl->db);
	if (code != SQLITE_OK) {
		return false;
	}
	_impl->db = nullptr;
	_impl->poisoned = false;
	_impl->configured = false;
	return true;
}

bool DesktopArchiveSqlite::usable() const { return _impl->Current(); }
void DesktopArchiveSqlite::FailForTest(const QString &phase, int occurrence) {
	_impl->fault = phase;
	_impl->faultCount = occurrence;
}

}
