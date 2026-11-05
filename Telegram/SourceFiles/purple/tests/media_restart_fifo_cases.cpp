/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_restart.h"
#include "purple/purple_media_rules.h"
#include "ui/click_handler.h"

#include <crl/crl_on_main.h>
#include <rpl/event_stream.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif // Q_OS_MAC

#include <iostream>

namespace {

struct Voice {
	AudioMsgId identity = AudioMsgId::ForVideo();
	bool playing = false;
	bool seekable = true;
	int position = 7;
	rpl::event_stream<> updates;
};

class HistoryProbe final : public QWidget, public ClickHandlerHost {
public:
	HistoryProbe(Voice &voice, bool waveform)
	: voice(voice)
	, waveform(waveform)
	, action(std::make_shared<GenericClickHandler>([=, this] {
		++activations;
		if (!waveform) {
			this->voice.playing = !this->voice.playing;
		}
		if (nextSeekable) {
			this->voice.seekable = *nextSeekable;
		}
	})) {
		ClickHandler::setActive(action, this);
	}

	Voice &voice;
	bool waveform = false;
	bool handled = false;
	int activations = 0;
	int restarts = 0;
	int extraReleaseActivations = 0;
	std::optional<bool> nextSeekable;
	Purple::MediaRules::Restart lastRestart = Purple::MediaRules::Restart::None;
	ClickHandlerPtr action;

protected:
	void mousePressEvent(QMouseEvent *) override {
		ClickHandler::pressed();
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		if (const auto activated = ClickHandler::unpressed()) {
			if (handled) {
				++extraReleaseActivations;
			}
			ActivateClickHandler(window(), activated, Qt::LeftButton);
		}
	}
	void mouseDoubleClickEvent(QMouseEvent *) override {
		const auto decision = Purple::MediaRules::RestartOnDoubleClick({
			.leftButton = true,
			.documentOpen = true,
			.voiceOrRound = true,
			.currentTrack = true,
			.seekable = voice.seekable,
		});
		if (decision == Purple::MediaRules::Restart::None) {
			ClickHandler::pressed();
			return;
		}
		handled = true;
		Purple::QueueHistoryMediaRestart(
			this,
			voice.updates.events(),
			voice.identity,
			[=, this] { return voice.identity; },
			[=, this] {
				++restarts;
				lastRestart = voice.seekable
					? Purple::MediaRules::Restart::Seek
					: Purple::MediaRules::Restart::Replay;
				voice.position = 0;
				voice.playing = true;
			});
	}
	void clickHandlerPressedChanged(const ClickHandlerPtr &, bool pressed) override {
		if (waveform) {
			voice.playing = !pressed;
			if (!pressed) {
				voice.position = 3;
			}
		}
	}

};

bool Drain() {
	const auto done = std::make_shared<bool>(false);
	auto elapsed = QElapsedTimer();
	elapsed.start();
	crl::on_main([=] { *done = true; });
	while (!*done && elapsed.elapsed() < 2000) {
#ifdef Q_OS_MAC
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
#endif // Q_OS_MAC
		QCoreApplication::processEvents();
	}
	return *done;
}

void Deliver(HistoryProbe &probe, QEvent::Type type, bool down) {
	auto event = QMouseEvent(type, QPointF(5, 5), QPointF(5, 5),
		Qt::LeftButton, down ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(&probe, &event);
}

} // namespace

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	if (QApplication::platformName() != u"offscreen"_q) {
		return 90;
	}
	crl::init_main_queue([](void (*callback)(void*), void *argument) {
		QTimer::singleShot(0, QCoreApplication::instance(), [=] { callback(argument); });
	});
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool condition, const char *label) {
		++checks;
		if (!condition) {
			++failures;
			std::cerr << label << '\n';
		}
	};
	for (const auto initial : { false, true }) {
		for (const auto interleave : { false, true }) {
			for (const auto waveform : { false, true }) {
				auto voice = Voice();
				voice.playing = initial;
				auto probe = HistoryProbe(voice, waveform);
				Deliver(probe, QEvent::MouseButtonPress, true);
				Deliver(probe, QEvent::MouseButtonRelease, false);
				expect(probe.activations == 0, "first activation is genuinely deferred");
				if (interleave) {
					expect(Drain(), "first activation settles");
				}
				Deliver(probe, QEvent::MouseButtonDblClick, true);
				expect(probe.handled, "double click is classified synchronously");
				expect(probe.restarts == 0, "restart waits for the same FIFO");
				expect(!ClickHandler::getPressed(), "handled double click leaves release unarmed");
				Deliver(probe, QEvent::MouseButtonRelease, false);
				expect(Drain(), "restart and first activation settle");
				expect(voice.playing && voice.position == 0, "FIFO restart leaves voice playing at zero");
				expect(probe.restarts == 1 && probe.activations == 1, "one restart follows one activation");
				expect(probe.extraReleaseActivations == 0, "second release does not toggle");
				ClickHandler::clearActive();
			}
		}
	}
	for (const auto initial : { false, true }) {
		auto voice = Voice();
		voice.playing = initial;
		auto probe = HistoryProbe(voice, false);
		Deliver(probe, QEvent::MouseButtonPress, true);
		Deliver(probe, QEvent::MouseButtonRelease, false);
		expect(Drain(), "ordinary single click settles");
		expect(voice.playing != initial && voice.position == 7, "single click only toggles");
		expect(probe.activations == 1 && probe.restarts == 0, "single click never restarts");
		ClickHandler::clearActive();
	}
	for (const auto later : { false, true }) {
		auto voice = Voice();
		voice.seekable = !later;
		auto probe = HistoryProbe(voice, false);
		probe.nextSeekable = later;
		Deliver(probe, QEvent::MouseButtonPress, true);
		Deliver(probe, QEvent::MouseButtonRelease, false);
		Deliver(probe, QEvent::MouseButtonDblClick, true);
		Deliver(probe, QEvent::MouseButtonRelease, false);
		expect(Drain(), "changed seekability settles");
		expect(probe.lastRestart == (later
			? Purple::MediaRules::Restart::Seek
			: Purple::MediaRules::Restart::Replay), "restart reads post-activation seekability");
		ClickHandler::clearActive();
	}
	for (auto scenario = 0; scenario != 5; ++scenario) {
		auto voice = std::make_unique<Voice>();
		auto window = std::make_unique<QWidget>();
		auto context = std::make_unique<QWidget>(window.get());
		auto reads = 0;
		auto restarts = 0;
		Purple::QueueHistoryMediaRestart(
			context.get(),
			voice->updates.events(),
			voice->identity,
			[&] { ++reads; return voice ? voice->identity : AudioMsgId(); },
			[&] { ++restarts; });
		if (scenario == 0) {
			voice->identity = AudioMsgId::ForVideo();
		} else if (scenario == 1) {
			context.reset();
		} else if (scenario == 2) {
			context->setParent(nullptr);
			window.reset();
		} else if (scenario == 3) {
			voice.reset();
		} else {
			voice->updates.fire({});
		}
		expect(Drain(), "guarded restart settles");
		expect(restarts == (scenario == 4 ? 1 : 0), "identity and lifetime guards cancel obsolete restart");
		expect(reads == ((scenario == 0 || scenario == 4) ? 1 : 0), "dead player or context is never read");
	}
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
