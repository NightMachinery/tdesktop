#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_config_diff.h"

namespace Main { class Session; }
namespace Ui { class GenericBox; }
namespace Window { class SessionController; }

namespace Purple {

struct AccountAppWideRemovalTarget {
	AccountSettingsOpenTarget destination;
	std::vector<ConfigKeyChange> removed;
	QByteArray successor;
	SettingsFileInput backup;
	std::map<QString, QByteArray> identities;
	uint64_t revision = 0;
	bool writable = false;
};

struct AccountAppWideRemovalResult {
	bool ok = false;
	bool changed = false;
	bool backupChanged = false;
	QString error;
	QByteArray committedIdentity;
	QByteArray backupIdentity;
};

[[nodiscard]] std::optional<AccountAppWideRemovalTarget> PreviewAccountAppWideRemoval(
	DesktopAccountConfig &config, const AccountKey &account);
[[nodiscard]] bool ValidateAccountAppWideRemoval(
	DesktopAccountConfig &config, const AccountAppWideRemovalTarget &target);
[[nodiscard]] AccountAppWideRemovalResult CommitAccountAppWideRemoval(
	DesktopAccountConfig &config,
	const AccountAppWideRemovalTarget &target,
	const std::function<bool()> &originCurrent);
[[nodiscard]] bool ValidateCommittedAppWideRemoval(
	DesktopAccountConfig &config,
	const AccountAppWideRemovalTarget &target,
	const AccountAppWideRemovalResult &result);
[[nodiscard]] QString InactiveAppWideSettingsSummary(Main::Session *session);
[[nodiscard]] QString RemoveAccountAppWideSettingsTitle(Main::Session *session);
void RemoveAccountAppWideSettingsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	AccountAppWideRemovalTarget target);
[[nodiscard]] std::optional<AccountAppWideRemovalTarget> AccountAppWideRemovalFor(
	Main::Session *session);

}
