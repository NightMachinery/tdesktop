/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_settings_editor.h"

#include "purple/purple_media_reply.h"

#include <QtCore/QRegularExpression>

#include <set>

namespace Purple {
namespace {

using Route = SettingsEditorRoute;
using Status = SettingsEditorStatus;

std::optional<AccountSettingsRow> RowFor(SettingId id) {
	switch (id) {
	case SettingId::Reasons: return AccountSettingsRow::LastSeenReasons;
	case SettingId::Trade: return AccountSettingsRow::LastSeenPeek;
	case SettingId::SendAfterSave: return AccountSettingsRow::SendAfterSave;
	default: return std::nullopt;
	}
}

std::optional<Route> RouteFor(const SettingAddress &address) {
	const auto descriptor = FindSettingDescriptor(address);
	if (!descriptor || descriptor->codec == SettingCodec::Retired
		|| descriptor->codec == SettingCodec::Future) {
		return std::nullopt;
	}
	const auto id = descriptor->id;
	if (RowFor(id)) {
		return Route::AccountRow;
	} else if (id == SettingId::Members) {
		return Route::Members;
	} else if (id == SettingId::StartDelay || id == SettingId::ExitHiddenChats
		|| id == SettingId::ReadyTimeout) {
		return Route::Peek;
	} else if (std::get<QString>(address.front()) == u"media_reply"_q) {
		return Route::MediaReply;
	}
	switch (SettingsScope(address)) {
	case SettingScope::AppWide: return Route::AppWide;
	case SettingScope::PerAccount:
	case SettingScope::FileLocal: return Route::Own;
	case SettingScope::Unknown: return std::nullopt;
	}
	return std::nullopt;
}

Status FromCore(SettingsEditStatus status) {
	switch (status) {
	case SettingsEditStatus::Changed: return Status::Ready;
	case SettingsEditStatus::Unchanged: return Status::Unchanged;
	case SettingsEditStatus::StaleRevision: return Status::StaleRevision;
	case SettingsEditStatus::InvalidSource: return Status::InvalidSource;
	case SettingsEditStatus::InvalidValue: return Status::InvalidValue;
	case SettingsEditStatus::Unsupported: return Status::Unsupported;
	case SettingsEditStatus::VerificationFailed: return Status::WriteFailed;
	}
	return Status::WriteFailed;
}

QString StaleMessage() {
	return u"The settings file, account or opening context changed. Review again."_q;
}

SettingsFileInput OwnGoodInput(const AccountSettingsOpenTarget &target) {
	const auto path = target.file.path + u".good"_q;
	return AccountFilePathSafe(path) ? ReadSettingsFileInput(path) : SettingsFileInput();
}

LayerSettingsResult LayersFor(
		const SettingsEditorOpening &opening,
		const AccountSettingsWriteTarget &target,
		const SettingsFileInput &candidate) {
	const auto &file = target.destination.file;
	const auto &global = target.globalSource.file;
	const auto &own = opening.ownSource->file;
	const auto globalInput = global.path == file.path ? candidate : global.input;
	const auto ownInput = own.path == file.path ? candidate : own.input;
	auto options = SettingsLayerOptions();
	options.globalPresent = globalInput.state != SettingsFileState::Missing;
	options.accountPresent = ownInput.state != SettingsFileState::Missing;
	options.followListMembers = opening.workMode == AccountWorkMode::Follow;
	options.traceReads = true;
	if (target.globalGoodInput.state == SettingsFileState::Readable) {
		options.globalGood = SettingsLayerText{
			QString::fromUtf8(target.globalGoodInput.bytes), global.path + u".good"_q,
		};
	}
	if (opening.ownGood.state == SettingsFileState::Readable) {
		options.accountGood = SettingsLayerText{ QString::fromUtf8(opening.ownGood.bytes),
			own.path + u".good"_q };
	}
	return LayerSettings(
		QString::fromUtf8(globalInput.bytes), global.path,
		QString::fromUtf8(ownInput.bytes), own.path,
		options);
}

bool Incomplete(const SettingsEditorPreview &preview) {
	const auto incomplete = [](const LayerSettingsResult &layers) {
		const auto report = [](const SettingsLayerFileReport &part) {
			return part.current.parsed.traceIncomplete
				|| (part.good && part.good->parsed.traceIncomplete);
		};
		return layers.effective.traceIncomplete || report(layers.global) || report(layers.account);
	};
	return preview.physicalBefore.traceIncomplete || preview.physicalAfter.traceIncomplete
		|| incomplete(preview.beforeLayers) || incomplete(preview.afterLayers);
}

QStringList IntroducedWarnings(const ParseResult &before, const ParseResult &after) {
	static const auto location = QRegularExpression(u"(line |column )\\d+"_q);
	const auto identity = [](QString message) { return message.remove(location); };
	auto old = std::multiset<QString>();
	for (const auto &warning : before.warnings) {
		old.insert(identity(warning));
	}
	auto result = QStringList();
	for (const auto &warning : after.warnings) {
		const auto was = old.find(identity(warning));
		if (was == old.end()) {
			result.push_back(warning);
		} else {
			old.erase(was);
		}
	}
	return result;
}

bool ScalarMatchesReader(const SettingsEdit &edit, const Settings &parsed) {
	const auto requested = std::get_if<std::optional<SettingScalar>>(&edit.operation);
	if (!requested || !*requested) {
		return true;
	}
	const auto descriptor = FindSettingDescriptor(edit.address);
	if (descriptor->id == SettingId::Round) {
		const auto &kind = std::get<QString>(edit.address[1]);
		const auto index = kind == u"private"_q ? 0 : kind == u"groups"_q ? 1 : 2;
		const auto &round = parsed.mediaReply.defaults[index].round;
		return round && MediaReplyModeName(*round) == std::get<QString>(**requested);
	}
	const auto actual = EffectiveSettingValue(parsed, edit.address);
	if (!actual.isValid()) {
		return true;
	}
	const auto &value = **requested;
	if (const auto boolean = std::get_if<bool>(&value)) {
		return actual.toBool() == *boolean;
	} else if (const auto integer = std::get_if<int64>(&value)) {
		return actual.toLongLong() == *integer;
	}
	const auto &text = std::get<QString>(value);
	const auto codec = descriptor->codec;
	if (codec == SettingCodec::Duration) {
		const auto seconds = ParseDuration(text);
		return seconds && actual.toInt() == *seconds;
	} else if (codec == SettingCodec::Time) {
		const auto minutes = ParseTimeOfDay(text);
		return minutes && actual.toInt() == *minutes;
	}
	return actual.toString() == text;
}

SettingsEditorPreparation Unavailable(
		const std::optional<AccountSettingsOpenTarget> &source,
		const QString &error) {
	if (source && source->file.input.state == SettingsFileState::Readable) {
		const auto mapped = MapSettingsSource(source->file.input.bytes);
		if (!mapped.ok()) {
			return { .status = Status::InvalidSource, .error = mapped.error,
				.line = mapped.line, .column = mapped.column };
		}
	}
	return { .status = Status::Unavailable, .error = error };
}

} // namespace

SettingsEditorOpening CaptureSettingsEditorOpening(
		DesktopAccountConfig &routing,
		const AccountKey &requester) {
	auto result = SettingsEditorOpening();
	result.requester = requester;
	result.ownSource = routing.openTarget(requester, SettingsOpenRole::Own);
	result.globalSource = routing.openTarget(requester, SettingsOpenRole::AppWide);
	result.own = routing.settingsEditTarget(requester, SettingsOpenRole::Own, &result.ownError);
	result.appWide = routing.settingsEditTarget(requester, SettingsOpenRole::AppWide, &result.appWideError);
	if (result.ownSource) {
		result.ownGood = OwnGoodInput(*result.ownSource);
	}
	result.workMode = routing.workMode(requester);
	return result;
}

SettingsEditorPreparation PrepareSettingsEditorEdit(
		DesktopAccountConfig &routing,
		const SettingsEditorOpening &opening,
		SettingsEditorIntent intent) {
	if (opening.current && !opening.current()) {
		return { .status = Status::StaleRevision, .error = StaleMessage() };
	}
	auto route = Route::Members;
	auto row = std::optional<AccountSettingsRow>();
	const auto edits = std::get_if<std::vector<SettingsEdit>>(&intent);
	if (edits) {
		if (edits->empty()) {
			return { .status = Status::Unchanged };
		}
		const auto first = RouteFor(edits->front().address);
		if (!first || *first == Route::Members) {
			return { .status = Status::Unsupported, .error = u"This setting needs a dedicated editor."_q,
				.address = edits->front().address };
		}
		route = *first;
		for (const auto &edit : *edits) {
			if (RouteFor(edit.address) != route
				|| (edits->size() > 1 && (route == Route::AccountRow || route == Route::MediaReply))) {
				return { .status = Status::Unsupported,
					.error = u"Review edits to each settings destination and purpose separately."_q,
					.address = edit.address };
			}
		}
		row = RowFor(FindSettingDescriptor(edits->front().address)->id);
	}

	const auto appWide = route == Route::AppWide || route == Route::Peek
		|| (route == Route::Members && opening.workMode == AccountWorkMode::Follow);
	const auto &target = appWide ? opening.appWide : opening.own;
	if (!target || !opening.ownSource) {
		return appWide
			? Unavailable(opening.globalSource, opening.appWideError)
			: Unavailable(opening.ownSource, opening.ownError);
	}
	if (routing.workMode(opening.requester) != opening.workMode
		|| !routing.validateSettingsEdit(*target)
		|| !routing.validateOpen(*opening.ownSource)
		|| OwnGoodInput(*opening.ownSource) != opening.ownGood) {
		return { .status = Status::StaleRevision, .error = StaleMessage() };
	}

	auto review = std::make_shared<SettingsEditorReview>();
	review->_opening = opening;
	review->_target = *target;
	const auto &file = target->destination.file;
	if (row) {
		review->_row = routing.rowTarget(opening.requester, *row);
		if (!review->_row) {
			return { .status = Status::Unavailable, .error = u"The account row is unavailable."_q };
		}
	} else if (const auto members = std::get_if<SettingsEditorMembers>(&intent)) {
		auto error = QString();
		review->_list = routing.listEditTarget(opening.requester, members->list, &error);
		if (!review->_list) {
			return { .status = Status::Unavailable, .error = error };
		}
		const auto &global = target->globalSource.file;
		if (file.path != global.path) {
			const auto good = target->globalGoodInput;
			const auto primary = ParseSettings(QString::fromUtf8(global.input.bytes), global.path);
			review->_inheritedMembers = (global.input.state != SettingsFileState::Readable || !primary.ok())
				&& good.state == SettingsFileState::Readable ? good.bytes : global.input.bytes;
		}
	}

	auto &preview = review->_preview;
	preview.route = route;
	preview.requester = opening.requester;
	preview.fileAccount = file.account;
	preview.path = file.path;
	preview.creates = file.input.state == SettingsFileState::Missing;
	preview.offlineGlobalSource = file.account == target->destination.global && !opening.globalSignedIn;
	preview.before = file.input.bytes;
	const auto source = preview.creates ? NewAccountSettingsText() : preview.before;
	const auto map = MapSettingsSource(source);
	if (!map.ok()) {
		return { .status = Status::InvalidSource, .error = map.error,
			.line = map.line, .column = map.column };
	}
	review->_revision = map.revision;
	review->_intent = std::move(intent);
	const auto candidate = review->replay(source);
	if (candidate.status != SettingsEditStatus::Changed) {
		return { .status = FromCore(candidate.status), .error = candidate.error,
			.address = candidate.address };
	}
	preview.after = candidate.bytes;
	preview.physicalBefore = ParseSettingsWithTrace(QString::fromUtf8(preview.before), file.path);
	preview.physicalAfter = ParseSettingsWithTrace(QString::fromUtf8(preview.after), file.path);
	if (!preview.physicalAfter.ok()) {
		return { .status = Status::InvalidSource, .error = preview.physicalAfter.error };
	}
	if (const auto group = std::get_if<std::vector<SettingsEdit>>(&review->_intent)) {
		for (auto i = group->begin(); i != group->end(); ++i) {
			const auto shadowed = std::any_of(i + 1, group->end(), [&](const SettingsEdit &next) {
				return next.address == i->address;
			});
			if (!shadowed && !ScalarMatchesReader(*i, preview.physicalAfter.settings)) {
				return { .status = Status::InvalidValue, .error = u"The reader would change this value."_q,
					.address = i->address };
			}
		}
	}
	preview.beforeLayers = LayersFor(opening, *target, file.input);
	preview.afterLayers = LayersFor(opening, *target, { SettingsFileState::Readable, preview.after });
	preview.usesGlobalRecovery = preview.beforeLayers.global.usedGood;
	preview.diff = DiffConfigText(preview.before, preview.after);
	preview.summary = SummarizeConfigChange(preview.before, preview.after);
	const auto global = file.account == target->destination.global;
	preview.impact = AnalyzeConfigChange({
		.current = { preview.before, !preview.creates },
		.proposed = { preview.after, true },
		.role = global ? ConfigImpactFileRole::Global : ConfigImpactFileRole::Layered,
		.workMode = opening.workMode,
		.global = preview.usesGlobalRecovery ? ConfigImpactText{ target->globalGoodInput.bytes, true }
			: ConfigImpactText{ target->globalSource.file.input.bytes,
				target->globalSource.file.input.state == SettingsFileState::Readable },
	});
	preview.introducedWarnings = IntroducedWarnings(preview.physicalBefore, preview.physicalAfter);
	preview.incompleteTrace = Incomplete(preview);
	return { .status = preview.incompleteTrace ? Status::IncompleteTrace : Status::Ready,
		.review = std::move(review) };
}

bool SettingsEditorReview::validate(DesktopAccountConfig &routing) const {
	if ((_opening.current && !_opening.current())
		|| routing.workMode(_opening.requester) != _opening.workMode
		|| !routing.validateSettingsEdit(_target)
		|| !routing.validateOpen(*_opening.ownSource)
		|| OwnGoodInput(*_opening.ownSource) != _opening.ownGood
		|| (_row && !routing.validateRow(*_row))) {
		return false;
	}
	if (!_list) {
		return true;
	}
	const auto current = routing.listEditTarget(_opening.requester, _list->list);
	return current && current->edit.destination.role == _target.destination.role
		&& current->members == _list->members && current->comments == _list->comments;
}

SettingsEditResult SettingsEditorReview::replay(const QByteArray &input) const {
	if (const auto members = std::get_if<SettingsEditorMembers>(&_intent)) {
		return members->members
			? EditSettingListMembers(input, _revision, members->list, *members->members, _inheritedMembers)
			: EditSettingScalar(input, _revision, { u"lists"_q, members->list, u"members"_q }, std::nullopt);
	}
	return ApplySettingsEdits(input, _revision, std::get<std::vector<SettingsEdit>>(_intent));
}

SettingsEditorApplyResult SettingsEditorReview::commit(
		DesktopAccountConfig &routing,
		const std::function<bool()> &origin,
		bool fromPeekWait) {
	if (_preview.route == Route::Peek && !fromPeekWait) {
		return { .status = Status::Unsupported, .error = u"This edit requires the Peek wait gate."_q };
	}
	if (std::exchange(_consumed, true) || !origin || !origin() || !validate(routing)) {
		return { .status = Status::StaleRevision, .error = StaleMessage() };
	}
	const auto current = [&] {
		return origin() && (!_opening.current || _opening.current())
			&& (_opening.ownSource->file.path == _target.destination.file.path
				|| (routing.validateOpen(*_opening.ownSource)
					&& OwnGoodInput(*_opening.ownSource) == _opening.ownGood));
	};
	auto stale = false;
	const auto result = routing.writeSettingsBytes(_target, [&](const QByteArray &input) {
		auto result = replay(input);
		if (result.status != SettingsEditStatus::Changed || result.bytes != _preview.after) {
			stale = true;
			return SettingsEditResult{ .bytes = input, .status = SettingsEditStatus::StaleRevision,
				.error = StaleMessage() };
		}
		return result;
	}, current, _opening.origin);
	const auto status = result.ok ? Status::Saved
		: result.changed ? Status::SavedWithFollowUpError
		: (stale || !current() || !routing.validateSettingsEdit(_target)) ? Status::StaleRevision
		: Status::WriteFailed;
	return { .status = status, .error = result.error, .operation = result.operation,
		.created = result.created, .offlineGlobalSource = _preview.offlineGlobalSource };
}

SettingsEditorApplyResult ApplySettingsEditorEdit(
		DesktopAccountConfig &routing,
		SettingsEditorReview &review,
		const std::function<bool()> &current) {
	return review.commit(routing, current, false);
}

} // namespace Purple
