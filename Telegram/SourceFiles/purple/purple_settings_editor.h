/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_settings_edit.h"

namespace Window {
class Controller;
} // namespace Window

namespace Purple {

enum class SettingsEditorRoute { AppWide, Own, AccountRow, MediaReply, Members, Peek };

enum class SettingsEditorStatus {
	Ready,
	IncompleteTrace,
	Unchanged,
	Unsupported,
	InvalidSource,
	InvalidValue,
	StaleRevision,
	Unavailable,
	Waiting,
	Cancelled,
	WriteFailed,
	Saved,
	SavedWithFollowUpError,
};

struct SettingsEditorMembers {
	QString list;
	std::optional<std::vector<PeerIdValue>> members;
};

using SettingsEditorIntent = std::variant<std::vector<SettingsEdit>, SettingsEditorMembers>;

struct SettingsEditorOpening {
	AccountKey requester;
	std::optional<AccountSettingsOpenTarget> ownSource;
	std::optional<AccountSettingsOpenTarget> globalSource;
	std::optional<AccountSettingsWriteTarget> own;
	std::optional<AccountSettingsWriteTarget> appWide;
	QString ownError;
	QString appWideError;
	SettingsFileInput ownGood;
	SettingsFileInput globalGood;
	bool globalSignedIn = true;
	AccountWorkMode workMode = AccountWorkMode::Own;
	uint64_t origin = 0;
	std::function<bool()> current;
	std::function<bool(Window::Controller*)> windowCurrent;
};

struct SettingsEditorPreview {
	SettingsEditorRoute route = SettingsEditorRoute::Own;
	AccountKey requester;
	AccountKey fileAccount;
	QString path;
	bool creates = false;
	bool offlineGlobalSource = false;
	bool usesGlobalRecovery = false;
	bool incompleteTrace = false;
	QByteArray before;
	QByteArray after;
	ParseResult physicalBefore;
	ParseResult physicalAfter;
	LayerSettingsResult beforeLayers;
	LayerSettingsResult afterLayers;
	ConfigTextDiff diff;
	ConfigChangeSummary summary;
	ConfigChangeImpact impact;
	QStringList introducedWarnings;
};

struct SettingsEditorApplyResult {
	SettingsEditorStatus status = SettingsEditorStatus::Unavailable;
	QString error;
	uint64_t operation = 0;
	bool created = false;
	bool offlineGlobalSource = false;
};

struct SettingsEditorPreparation;

class SettingsEditorReview final {
public:
	[[nodiscard]] const SettingsEditorPreview &preview() const { return _preview; }

private:
	friend SettingsEditorPreparation PrepareSettingsEditorEdit(
		DesktopAccountConfig&,
		const SettingsEditorOpening&,
		SettingsEditorIntent);
	friend SettingsEditorApplyResult ApplySettingsEditorEdit(
		DesktopAccountConfig&,
		SettingsEditorReview&,
		const std::function<bool()>&);
	friend void RequestSettingsEditorApply(
		not_null<Window::Controller*>,
		std::shared_ptr<SettingsEditorReview>,
		std::function<void(SettingsEditorApplyResult)>);

	[[nodiscard]] bool validate(DesktopAccountConfig &routing) const;
	[[nodiscard]] SettingsEditResult replay(const QByteArray &input) const;
	[[nodiscard]] SettingsEditorApplyResult commit(
		DesktopAccountConfig &routing,
		const std::function<bool()> &current,
		bool fromPeekWait);

	SettingsEditorPreview _preview;
	SettingsEditorOpening _opening;
	AccountSettingsWriteTarget _target;
	std::optional<AccountSettingsRowTarget> _row;
	std::optional<AccountListEditTarget> _list;
	std::optional<QByteArray> _inheritedMembers;
	SettingsEditorIntent _intent;
	QByteArray _revision;
	bool _waiting = false;
	bool _consumed = false;
};

struct SettingsEditorPreparation {
	SettingsEditorStatus status = SettingsEditorStatus::Unavailable;
	QString error;
	SettingAddress address;
	int line = 0;
	int column = 0;
	std::shared_ptr<SettingsEditorReview> review;
};

[[nodiscard]] SettingsEditorOpening CaptureSettingsEditorOpening(
	DesktopAccountConfig &routing,
	const AccountKey &requester);

[[nodiscard]] SettingsEditorOpening CaptureWindowSettingsEditor(
	not_null<Window::Controller*> window,
	std::function<bool()> editorCurrent = {});

[[nodiscard]] LayerSettingsResult InspectSettingsEditor(
	const SettingsEditorOpening &opening);

[[nodiscard]] SettingsEditorPreparation PrepareSettingsEditorEdit(
	DesktopAccountConfig &routing,
	const SettingsEditorOpening &opening,
	SettingsEditorIntent intent);

[[nodiscard]] SettingsEditorPreparation PrepareWindowSettingsEditorEdit(
	const SettingsEditorOpening &opening,
	SettingsEditorIntent intent);

[[nodiscard]] SettingsEditorApplyResult ApplySettingsEditorEdit(
	DesktopAccountConfig &routing,
	SettingsEditorReview &review,
	const std::function<bool()> &current);

void RequestSettingsEditorApply(
	not_null<Window::Controller*> window,
	std::shared_ptr<SettingsEditorReview> review,
	std::function<void(SettingsEditorApplyResult)> completed);

} // namespace Purple
