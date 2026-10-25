#pragma once

#include "purple/purple_archive_sql.h"
#include <memory>

namespace Purple {

struct DesktopArchiveSqlResult {
	bool ok = false;
	bool committed = false;
	int code = 0;
	QString failure;
	std::vector<std::vector<ArchiveSqlValue>> rows;
};

class DesktopArchiveSqlite final {
  public:
	DesktopArchiveSqlite();
	void SetActive(std::function<bool()> active);
	~DesktopArchiveSqlite();
	DesktopArchiveSqlite(const DesktopArchiveSqlite &) = delete;
	DesktopArchiveSqlite &operator=(const DesktopArchiveSqlite &) = delete;
	[[nodiscard]] DesktopArchiveSqlResult Open(
		const QString &path, bool create, bool readOnly = false);
	[[nodiscard]] DesktopArchiveSqlResult Configure();
	[[nodiscard]] DesktopArchiveSqlResult Read(const ArchiveSqlStatement &statement);
	[[nodiscard]] DesktopArchiveSqlResult Execute(const ArchiveSqlProgram &program,
		std::function<bool()> beforeCommit = {});
	[[nodiscard]] bool BeginRead();
	[[nodiscard]] bool EndRead();
	[[nodiscard]] bool Close();
	[[nodiscard]] bool CloseFinal();
	[[nodiscard]] bool usable() const;
	void FailForTest(const QString &phase, int occurrence = 1);

  private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}
