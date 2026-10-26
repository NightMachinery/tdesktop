#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "mtproto/sender.h"
#include "purple/purple_sync_retry.h"
#include "rpl/event_stream.h"

#include <map>
#include <memory>

namespace Purple {

[[nodiscard]] std::optional<SyncRequestTransient> PendingSyncRequest(
	not_null<MTP::Instance*> instance);
[[nodiscard]] rpl::producer<MTP::Instance*> SyncRequestChanges();
[[nodiscard]] QString SyncRequestWaitText(const SyncRequestTransient &pending);

class SyncRequestSender final : public base::has_weak_ptr {
public:
	SyncRequestSender(not_null<MTP::Instance*> instance, Fn<bool()> current,
		Fn<void(mtpRequestId, SyncRequestTransient)> deferred = {});
	~SyncRequestSender();
	void cancelAll();
	mtpRequestId watch(Fn<void()> expired, Fn<void()> retry, Fn<void()> cancelled);

	template <typename Request>
	class Builder {
	public:
		Builder(SyncRequestSender *owner, Request request)
		: _owner(owner), _request(std::move(request)) {
		}
		Builder &done(FnMut<void(const typename Request::ResponseType &)> done) {
			_done = std::move(done);
			return *this;
		}
		Builder &fail(Fn<void(const MTP::Error &)> fail) {
			_fail = std::move(fail);
			return *this;
		}
		Builder &resumeEffect(Fn<void(Fn<void(bool)>)> gate) {
			_resume = std::move(gate);
			return *this;
		}
		Builder &toDC(MTP::ShiftedDcId dc) {
			_dc = dc;
			return *this;
		}
		Builder &handleAllErrors() { return *this; }
		mtpRequestId send() {
			return _owner->send(
				std::move(_request), std::move(_done), std::move(_fail), _dc, std::move(_resume));
		}

	private:
		SyncRequestSender *_owner = nullptr;
		Request _request;
		FnMut<void(const typename Request::ResponseType &)> _done;
		Fn<void(const MTP::Error &)> _fail;
		MTP::ShiftedDcId _dc = 0;
		Fn<void(Fn<void(bool)>)> _resume;

	};

	template <typename Request>
	[[nodiscard]] Builder<Request> request(Request &&request) {
		return Builder<Request>(this, std::move(request));
	}
	struct Cancellation {
		SyncRequestSender *owner = nullptr;
		mtpRequestId id = 0;
		void cancel() { owner->cancel(id); }
	};
	[[nodiscard]] Cancellation request(mtpRequestId id) { return { this, id }; }

private:
	struct Pending {
		explicit Pending(int64_t now) : policy(now) {
		}
		SyncRetryState policy;
		base::Timer timer;
		Fn<void()> dispatch;
		Fn<void()> expired;
		Fn<void(Fn<void(bool)>)> resume;
		Fn<void(const MTP::Error &)> fail;
		std::optional<SyncRequestTransient> transient;
		mtpRequestId id = 0;
		mtpRequestId wire = 0;
		int64_t retryAtMs = 0;
		uint64_t generation = 0;
		bool active = true;
	};

	template <typename Request>
	mtpRequestId send(Request request,
			FnMut<void(const typename Request::ResponseType &)> done,
			Fn<void(const MTP::Error &)> fail, MTP::ShiftedDcId dc,
			Fn<void(Fn<void(bool)>)> resume) {
		const auto pending = create(std::move(fail));
		pending->resume = std::move(resume);
		const auto id = pending->id;
		const auto weak = base::make_weak(this);
		const auto state = std::weak_ptr<Pending>(pending);
		const auto handler = std::make_shared<FnMut<void(const typename Request::ResponseType &)>>(std::move(done));
		pending->dispatch = [weak, state, request = std::move(request), handler, dc] {
			const auto self = weak.get();
			const auto pending = state.lock();
			if (!self || !pending || !pending->active) {
				return;
			}
			auto copy = request;
			const auto generation = pending->generation;
			pending->wire = self->_api.request(std::move(copy)).toDC(dc)
				.done([weak, state, handler, generation](const typename Request::ResponseType &result) {
					const auto self = weak.get();
					const auto pending = state.lock();
					if (!self || !pending || !pending->active
						|| pending->generation != generation) {
						return;
					}
					const auto current = self->_current && self->_current();
					const auto live = weak.get();
					if (!live || !pending->active || pending->generation != generation) {
						return;
					}
					const auto failed = pending->fail;
					live->cancel(pending->id);
					if (current && *handler) {
						auto callback = std::move(*handler);
						callback(result);
					} else if (!current && failed) {
						failed(MTP::Error::Local(u"PURPLE_REQUEST_CANCELLED"_q, {}));
					}
				})
				.fail([weak, state, generation](const MTP::Error &error) {
					const auto self = weak.get();
					const auto pending = state.lock();
					if (self && pending && pending->active
						&& pending->generation == generation) {
						self->failed(pending, error);
					}
				}).handleAllErrors().send();
		};
		dispatch(pending);
		return id;
	}
	std::shared_ptr<Pending> create(Fn<void(const MTP::Error &)> fail);
	void dispatch(const std::shared_ptr<Pending> &pending);
	void failed(const std::shared_ptr<Pending> &pending,
		const MTP::Error &error, bool timeout = false);
	void tick(const std::shared_ptr<Pending> &pending);
	void resume(const std::shared_ptr<Pending> &pending);
	void arm(const std::shared_ptr<Pending> &pending, int64_t delayMs);
	void cancel(mtpRequestId id);
	void publish();

	not_null<MTP::Instance*> _instance;
	MTP::Sender _api;
	Fn<bool()> _current;
	Fn<void(mtpRequestId, SyncRequestTransient)> _deferred;
	std::map<mtpRequestId, std::shared_ptr<Pending>> _pending;

};

}
