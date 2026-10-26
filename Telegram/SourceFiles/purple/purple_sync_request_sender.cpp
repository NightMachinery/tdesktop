#include "purple/purple_sync_request_sender.h"

#include "base/unixtime.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <limits>

namespace Purple {
namespace {

struct Notice {
	MTP::Instance *instance = nullptr;
	std::optional<SyncRequestTransient> pending;
};

std::map<SyncRequestSender*, Notice> Notices;
rpl::event_stream<MTP::Instance*> Changes;

}

std::optional<SyncRequestTransient> PendingSyncRequest(
		not_null<MTP::Instance*> instance) {
	auto result = std::optional<SyncRequestTransient>();
	for (const auto &[sender, notice] : Notices) {
		if (notice.instance == instance && notice.pending
			&& (!result || notice.pending->retryAt < result->retryAt)) {
			result = notice.pending;
		}
	}
	return result;
}

rpl::producer<MTP::Instance*> SyncRequestChanges() {
	return Changes.events();
}

QString SyncRequestWaitText(const SyncRequestTransient &pending) {
	const auto time = QLocale().toString(
		QDateTime::fromSecsSinceEpoch(pending.retryAt).toLocalTime(),
		QLocale::ShortFormat);
	return (pending.kind == SyncTransientKind::Flood
		? tr::lng_purple_sync_telegram_wait(tr::now)
		: tr::lng_purple_sync_transient_wait(tr::now)).arg(time);
}

SyncRequestSender::SyncRequestSender(
		not_null<MTP::Instance*> instance, Fn<bool()> current)
: _instance(instance)
, _api(instance)
, _current(std::move(current)) {
	Notices.emplace(this, Notice{ instance });
}

SyncRequestSender::~SyncRequestSender() {
	cancelAll();
	Notices.erase(this);
}

void SyncRequestSender::cancelAll() {
	while (!_pending.empty()) {
		cancel(_pending.begin()->first);
	}
}

void SyncRequestSender::cancel(mtpRequestId id) {
	const auto i = _pending.find(id);
	if (i == _pending.end()) {
		return;
	}
	const auto pending = i->second;
	pending->active = false;
	pending->timer.cancel();
	if (pending->wire) {
		_api.request(base::take(pending->wire)).cancel();
	}
	_pending.erase(i);
	publish();
}

std::shared_ptr<SyncRequestSender::Pending> SyncRequestSender::create(
		Fn<void(const MTP::Error &)> fail) {
	const auto pending = std::make_shared<Pending>(crl::now());
	pending->id = _api.allocateRequestId();
	pending->fail = std::move(fail);
	_pending.emplace(pending->id, pending);
	return pending;
}

mtpRequestId SyncRequestSender::watch(
		Fn<void()> expired, Fn<void()> retry, Fn<void()> cancelled) {
	const auto pending = create([cancelled = std::move(cancelled)](const MTP::Error &) {
		if (cancelled) { cancelled(); }
	});
	pending->expired = std::move(expired);
	pending->dispatch = std::move(retry);
	arm(pending, pending->policy.deadline() - crl::now());
	return pending->id;
}

void SyncRequestSender::dispatch(const std::shared_ptr<Pending> &pending) {
	if (!pending->active) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto current = _current && _current();
	if (!weak || !pending->active) {
		return;
	}
	if (!current) {
		const auto callback = pending->fail;
		cancel(pending->id);
		if (callback) {
			callback(MTP::Error::Local(u"PURPLE_REQUEST_CANCELLED"_q, {}));
		}
		return;
	}
	++pending->generation;
	pending->retryAtMs = 0;
	pending->transient.reset();
	arm(pending, pending->policy.deadline() - crl::now());
	publish();
	pending->dispatch();
}

void SyncRequestSender::failed(const std::shared_ptr<Pending> &pending,
		const MTP::Error &error, bool timeout) {
	if (!pending->active) {
		return;
	}
	const auto utf8 = error.type().toUtf8();
	const auto transient = pending->policy.failed(error.code(),
		std::string_view(utf8.constData(), utf8.size()),
		crl::now(), base::unixtime::now(), timeout);
	const auto weak = base::make_weak(this);
	const auto current = _current && _current();
	if (!weak || !pending->active) {
		return;
	}
	if (!transient || !current) {
		const auto callback = pending->fail;
		cancel(pending->id);
		if (callback) {
			callback(error);
		}
		return;
	}
	if (pending->resume && transient->kind != SyncTransientKind::Flood) {
		const auto callback = pending->fail;
		cancel(pending->id);
		if (callback) { callback(MTP::Error::Local(u"PURPLE_EFFECT_OUTCOME_UNKNOWN"_q, {})); }
		return;
	}
	++pending->generation;
	if (timeout && pending->expired) {
		const auto expired = pending->expired;
		expired();
		if (!weak || !pending->active) { return; }
	}
	if (pending->wire) {
		_api.request(base::take(pending->wire)).cancel();
	}
	pending->transient = transient;
	pending->retryAtMs = crl::now() + transient->delayMs;
	arm(pending, transient->delayMs);
	publish();
}

void SyncRequestSender::arm(
		const std::shared_ptr<Pending> &pending, int64_t delayMs) {
	const auto weak = base::make_weak(this);
	const auto state = std::weak_ptr<Pending>(pending);
	const auto generation = pending->generation;
	pending->timer.setCallback([weak, state, generation] {
		const auto self = weak.get();
		const auto pending = state.lock();
		if (self && pending && pending->active && pending->generation == generation) {
			self->tick(pending);
		}
	});
	pending->timer.callOnce(std::clamp(delayMs,
		int64_t(1), int64_t(std::numeric_limits<int>::max())));
}

void SyncRequestSender::tick(const std::shared_ptr<Pending> &pending) {
	if (!pending->active) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto current = _current && _current();
	if (!weak || !pending->active) {
		return;
	}
	if (!current) {
		dispatch(pending);
		return;
	}
	if (pending->retryAtMs) {
		if (pending->retryAtMs > crl::now()) {
			arm(pending, pending->retryAtMs - crl::now());
		} else {
			resume(pending);
		}
	} else {
		failed(pending, MTP::Error::Local(u"PURPLE_REQUEST_TIMEOUT"_q, {}), true);
	}
}

void SyncRequestSender::resume(const std::shared_ptr<Pending> &pending) {
	if (!pending->resume) {
		dispatch(pending);
		return;
	}
	pending->timer.cancel();
	const auto generation = ++pending->generation;
	const auto weak = base::make_weak(this);
	const auto state = std::weak_ptr<Pending>(pending);
	const auto gate = pending->resume;
	const auto started = crl::now();
	gate([weak, state, generation, started](bool allowed) {
		const auto self = weak.get();
		const auto pending = state.lock();
		if (!self || !pending || !pending->active || pending->generation != generation) {
			return;
		}
		if (allowed) {
			pending->policy.accountForResumeWait(started, crl::now());
			self->dispatch(pending);
		} else {
			const auto callback = pending->fail;
			self->cancel(pending->id);
			if (callback) { callback(MTP::Error::Local(u"PURPLE_EFFECT_RESUME_REFUSED"_q, {})); }
		}
	});
}

void SyncRequestSender::publish() {
	auto transient = std::optional<SyncRequestTransient>();
	for (const auto &[id, pending] : _pending) {
		if (pending->transient
			&& (!transient || pending->transient->retryAt < transient->retryAt)) {
			transient = pending->transient;
		}
	}
	Notices[this] = { _instance, transient };
	crl::on_main([instance = _instance.get()] { Changes.fire_copy(instance); });
}

}
