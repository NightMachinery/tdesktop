/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_playback.h"

#include "purple/purple_media_rules.h"
#include "purple/purple_voice_queue.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_media_types.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_message.h"
#include "history/view/media/history_view_media.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_float.h"
#include "media/player/media_player_instance.h"
#include "base/event_filter.h"
#include <crl/crl_on_main.h>
#include "settings/settings_builder.h"
#include "ui/style/style_core.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_media_player.h"
#include "styles/style_purple_media_playback.h"
#include "styles/style_settings.h"

#include <QtGui/QPainter>
#include <QtWidgets/QWidget>

#include <array>

namespace Purple {
namespace {

constexpr auto kVideoMessageScalePref = "purple-video-message-scale"_cs;
constexpr auto kFloatKeepPref = "purple-video-message-float-keep"_cs;

rpl::event_stream<> VideoMessageScaleChanges;

[[nodiscard]] bool HostedInInstantView(const HistoryView::Element *parent) {
	return parent->Get<HistoryView::InstantViewMediaRuntime>() != nullptr;
}

[[nodiscard]] bool ScaledVideoMessage(
		const DocumentData *document,
		const HistoryView::Element *parent) {
	return document
		&& parent
		&& document->isVideoMessage()
		&& (VideoMessageScale() != 100)
		&& !HostedInInstantView(parent);
}

[[nodiscard]] double PixelRatio() {
	return double(style::DevicePixelRatio());
}

[[nodiscard]] QString ScaleLabel(int percent) {
	if (percent == MediaRules::kVideoMessageScaleActual) {
		return tr::lng_purple_video_message_size_actual(
			tr::now,
			lt_percent,
			QString::number(MediaRules::VideoMessageActualPercent(
				MediaRules::kVideoMessageReferenceSource,
				PixelRatio(),
				st::maxVideoMessageSize)));
	}
	return tr::lng_purple_video_message_size_percent(
		tr::now,
		lt_percent,
		QString::number(percent));
}

[[nodiscard]] std::vector<int> ScaleChoices() {
	auto result = std::vector<int>(
		begin(MediaRules::kVideoMessageScales),
		end(MediaRules::kVideoMessageScales));
	result.push_back(MediaRules::kVideoMessageScaleActual);
	return result;
}

void RelayoutVideoMessages() {
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto account = entry.account.get();
		if (!account->sessionExists()) {
			continue;
		}
		auto &owner = account->session().data();
		const auto relayout = [&](not_null<PeerData*> peer) {
			const auto history = owner.historyLoaded(peer);
			if (!history) {
				return;
			}
			for (const auto &block : history->blocks) {
				for (const auto &view : block->messages) {
					const auto media = view->media();
					const auto document = media
						? media->getDocument()
						: nullptr;
					if (document && document->isVideoMessage()) {
						owner.requestItemResize(view->data());
					}
				}
			}
		};
		owner.enumerateUsers(relayout);
		owner.enumerateGroups(relayout);
		owner.enumerateBroadcasts(relayout);
	}
}

void ChooseVideoMessageScaleBox(not_null<Ui::GenericBox*> box) {
	const auto scales = ScaleChoices();
	auto options = std::vector<QString>();
	for (const auto percent : scales) {
		options.push_back(ScaleLabel(percent));
	}
	const auto current = VideoMessageScale();
	const auto initial = int(
		std::ranges::find(scales, current) - begin(scales));
	SingleChoiceBox(box, {
		.title = tr::lng_purple_video_message_size(),
		.options = options,
		.initialSelection = initial,
		.callback = [=](int index) {
			SetVideoMessageScale(scales[index]);
		},
	});
}

[[nodiscard]] bool VoicePaused() {
	const auto state = Media::Player::instance()->getState(
		AudioMsgId::Type::Voice);
	return Media::Player::IsPausedOrPausing(state.state)
		|| Media::Player::IsStoppedOrStopping(state.state);
}

[[nodiscard]] bool SeekableCurrentVoice() {
	using Type = AudioMsgId::Type;
	const auto player = Media::Player::instance();
	const auto state = player->getState(Type::Voice);
	return (state.id == player->current(Type::Voice))
		&& !Media::Player::IsStoppedOrStopping(state.state)
		&& (state.length > 0);
}

void RestartCurrentVoice(MediaRules::Restart restart) {
	using Type = AudioMsgId::Type;
	const auto player = Media::Player::instance();
	if (restart == MediaRules::Restart::Replay) {
		player->play(player->current(Type::Voice));
		return;
	}
	player->finishSeeking(Type::Voice, 0.);
	if (Media::Player::IsPausedOrPausing(player->getState(Type::Voice).state)) {
		player->play(Type::Voice);
	}
}

not_null<Ui::AbstractButton*> AddFloatButton(
		not_null<Media::Player::Float*> floating,
		Fn<const style::icon&()> icon,
		Fn<void()> clicked,
		const QString &name) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(floating.get());
	button->resize(st::purpleFloatButtonSize, st::purpleFloatButtonSize);
	button->setAccessibleName(name);
	button->setClickedCallback(std::move(clicked));
	button->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(button);
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(button->isOver() ? st::msgDateImgBgOver : st::msgDateImgBg);
		p.drawEllipse(button->rect());
		icon().paintInCenter(p, button->rect());
	}, button->lifetime());
	button->hide();
	return button;
}

} // namespace

int VideoMessageScale() {
	const auto stored = Core::App().settings().readPref<QByteArray>(
		kVideoMessageScalePref);
	return MediaRules::VideoMessageScaleFromText(
		std::string_view(stored.constData(), stored.size()));
}

void SetVideoMessageScale(int percent) {
	const auto value = MediaRules::VideoMessageScaleFrom(percent);
	if (value == VideoMessageScale()) {
		return;
	}
	Core::App().settings().writePref<QByteArray>(
		kVideoMessageScalePref,
		(value == MediaRules::kVideoMessageScaleActual)
			? QByteArray(MediaRules::kVideoMessageActualText.data(),
				MediaRules::kVideoMessageActualText.size())
			: QByteArray::number(value));
	Core::App().saveSettingsDelayed();
	VideoMessageScaleChanges.fire({});
	RelayoutVideoMessages();
}

int VideoMessageSide(const DocumentData *document) {
	const auto percent = VideoMessageScale();
	if (percent == MediaRules::kVideoMessageScaleActual) {
		const auto source = document ? document->dimensions.width() : 0;
		return MediaRules::VideoMessageActualSide(source, PixelRatio());
	}
	return MediaRules::VideoMessageSide(percent, st::maxVideoMessageSize);
}

bool FloatKeepsSize() {
	return Core::App().settings().readPref<bool>(kFloatKeepPref, true);
}

void SetFloatKeepsSize(bool keep) {
	Core::App().settings().writePref<bool>(kFloatKeepPref, keep);
	Core::App().saveSettingsDelayed();
}

int FloatPlayerSize(
		const QWidget *parent,
		const DocumentData *document,
		int size) {
	if (!FloatKeepsSize()) {
		return size;
	}
	const auto window = parent ? parent->window()->size() : QSize();
	return MediaRules::FloatPlayerSide(
		size,
		2 * st::mediaPlayerFloatMargin,
		VideoMessageSide(document),
		std::min(window.width(), window.height()));
}

QSize VideoMessageThumbSize(
		const DocumentData *document,
		const HistoryView::Element *parent,
		int &inOutWidthMax) {
	if (!ScaledVideoMessage(document, parent)) {
		return QSize();
	}
	const auto side = MediaRules::VideoMessageThumbSide(
		VideoMessageSide(document),
		inOutWidthMax,
		st::msgMaxWidth);
	inOutWidthMax = side;
	return QSize(side, side);
}

int VideoMessageBubbleWidthLimit(
		const DocumentData *document,
		const HistoryView::Element *parent,
		int maxWidth,
		bool ephemeral) {
	return (!ephemeral && ScaledVideoMessage(document, parent))
		? MediaRules::VideoMessageBubbleLimit(maxWidth, st::msgMaxWidth)
		: 0;
}

void BuildMediaPlaybackSettings(::Settings::Builder::SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/purple_media_playback"_q,
		.title = tr::lng_purple_media_playback(),
		.keywords = { u"purple"_q, u"voice"_q, u"video"_q, u"round"_q },
	});
	AddVoiceQueueSetting(builder);
	builder.addButton({
		.id = u"advanced/purple_video_message_size"_q,
		.title = tr::lng_purple_video_message_size(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(rpl::empty) | rpl::then(
			VideoMessageScaleChanges.events()
		) | rpl::map([] { return ScaleLabel(VideoMessageScale()); }),
		.onClick = [=] {
			if (controller) {
				controller->show(Box(ChooseVideoMessageScaleBox));
			}
		},
		.keywords = { u"purple"_q, u"video"_q, u"round"_q, u"size"_q },
	});
	const auto keep = builder.addButton({
		.id = u"advanced/purple_video_message_float_keep"_q,
		.title = tr::lng_purple_video_message_float_keep(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(FloatKeepsSize()),
		.keywords = { u"purple"_q, u"video"_q, u"round"_q, u"float"_q },
	});
	if (keep) {
		keep->toggledValue() | rpl::filter([](bool value) {
			return value != FloatKeepsSize();
		}) | rpl::on_next(SetFloatKeepsSize, keep->lifetime());
	}
	builder.addDividerText(tr::lng_purple_video_message_size_about());
	builder.addSkip();
}

bool RestartFloatOnDoubleClick(const HistoryItem *item) {
	const auto current = Media::Player::instance()->current(
		AudioMsgId::Type::Voice);
	if (!item || !current || current.contextId() != item->fullId()) {
		return false;
	}
	RestartCurrentVoice(SeekableCurrentVoice()
		? MediaRules::Restart::Seek
		: MediaRules::Restart::Replay);
	return true;
}

void DecorateFloat(
		not_null<Media::Player::Float*> floating,
		Fn<void(not_null<const HistoryItem*>)> open) {
	using Type = AudioMsgId::Type;
	const auto buttons = std::array{
		AddFloatButton(floating, []() -> const style::icon& {
			return st::purpleFloatOpenIcon;
		}, [=] {
			if (const auto item = floating->item(); item && open) {
				open(item);
			}
		}, tr::lng_purple_round_float_open(tr::now)),
		AddFloatButton(floating, []() -> const style::icon& {
			return VoicePaused()
				? st::purpleFloatPlayIcon
				: st::purpleFloatPauseIcon;
		}, [] {
			Media::Player::instance()->playPause(Type::Voice);
		}, tr::lng_purple_round_float_pause(tr::now)),
		AddFloatButton(floating, []() -> const style::icon& {
			return st::purpleFloatCloseIcon;
		}, [] {
			Media::Player::instance()->stop(Type::Voice);
		}, tr::lng_purple_round_float_close(tr::now)),
	};
	const auto show = [=](bool shown) {
		const auto visible = shown && floating->item() && !floating->dragged();
		for (const auto &button : buttons) {
			button->setVisible(visible);
		}
	};
	floating->sizeValue() | rpl::on_next([=](QSize size) {
		const auto side = st::purpleFloatButtonSize;
		const auto skip = st::purpleFloatButtonSkip;
		const auto count = int(buttons.size());
		auto left = (size.width() - count * side - (count - 1) * skip) / 2;
		const auto top = (size.height() - side) / 2;
		for (const auto &button : buttons) {
			button->move(left, top);
			left += side + skip;
		}
	}, floating->lifetime());
	base::install_event_filter(floating, [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::Enter) {
			show(true);
		} else if (type == QEvent::Leave) {
			show(false);
		} else if (type == QEvent::MouseMove && floating->dragged()) {
			show(false);
		} else if (type == QEvent::MouseButtonRelease) {
			crl::on_main(floating, [=] { show(floating->underMouse()); });
		}
		return base::EventFilterResult::Continue;
	});
	const auto pause = buttons[1];
	Media::Player::instance()->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return (state.id.type() == Type::Voice);
	}) | rpl::on_next([=] {
		pause->update();
	}, pause->lifetime());
}

bool RestartOnDoubleClick(
		const std::shared_ptr<ClickHandler> &active,
		Qt::MouseButton button,
		bool selecting) {
	using Type = AudioMsgId::Type;
	const auto open = dynamic_cast<DocumentOpenClickHandler*>(active.get());
	const auto document = open ? open->document().get() : nullptr;
	const auto player = Media::Player::instance();
	const auto current = player->current(Type::Voice);
	const auto context = MediaRules::ResolveRestartContext(
		open ? open->context() : FullMsgId(),
		document && (current.audio() == document),
		current.contextId());
	const auto item = document ? document->owner().message(context) : nullptr;
	const auto media = item ? item->media() : nullptr;
	const auto restart = MediaRules::RestartOnDoubleClick({
		.leftButton = (button == Qt::LeftButton),
		.selecting = selecting,
		.documentOpen = (open != nullptr),
		.voiceOrRound = document
			&& (document->isVoiceMessage() || document->isVideoMessage()),
		.viewOnce = media && (media->ttlSeconds() > 0),
		.currentTrack = document
			&& context
			&& (current.audio() == document)
			&& (current.contextId() == context),
		.seekable = SeekableCurrentVoice(),
	});
	if (restart == MediaRules::Restart::None) {
		return false;
	}
	RestartCurrentVoice(restart);
	return true;
}

} // namespace Purple
