#include "purple/purple_emoji_export_worker.h"

#include "purple/purple_emoji_export_store.h"
#include <QtCore/QMetaObject>

namespace Purple {

EmojiExportWorker::EmojiExportWorker(QString directory, QObject *parent)
: QObject(parent)
, _directory(std::move(directory))
, _thread([this] { run(); }) {
}

EmojiExportWorker::~EmojiExportWorker() {
	{
		const auto lock = std::lock_guard(_mutex);
		_stopping.store(true);
		++_generation;
	}
	_ready.notify_one();
	_thread.join();
}

quint64 EmojiExportWorker::generation() const {
	return _generation.load();
}

bool EmojiExportWorker::submit(quint64 generation, Work work, Done done) {
	const auto lock = std::lock_guard(_mutex);
	if (_stopping.load() || generation != _generation.load() || _pending || _clearRequired) {
		return false;
	}
	_pending = Job{ generation, false, std::move(work), std::move(done) };
	_ready.notify_one();
	return true;
}

quint64 EmojiExportWorker::revoke(bool clear, Done done) {
	auto removed = std::optional<Job>();
	auto token = quint64(0);
	{
		const auto lock = std::lock_guard(_mutex);
		token = ++_generation;
		_clearRequired = _clearRequired || clear;
		removed = std::move(_pending);
		_pending = Job{ token, true, {}, std::move(done) };
	}
	if (removed) completed(std::move(*removed), false);
	_ready.notify_one();
	return token;
}

void EmojiExportWorker::completed(Job job, bool ok) {
	const auto generation = job.generation;
	QMetaObject::invokeMethod(this, [done = std::move(job.done), generation, ok] {
		if (done) done(generation, ok);
	}, Qt::QueuedConnection);
}

void EmojiExportWorker::run() {
	while (true) {
		auto job = Job();
		auto clear = false;
		{
			auto lock = std::unique_lock(_mutex);
			_ready.wait(lock, [&] { return _stopping.load() || _pending.has_value(); });
			if (_stopping.load()) {
				const auto clearOnStop = _clearRequired;
				lock.unlock();
				if (clearOnStop) static_cast<void>(ClearEmojiExport(_directory));
				return;
			}
			job = std::move(*_pending);
			_pending.reset();
			clear = _clearRequired;
		}
		const auto current = [this, generation = job.generation] {
			return !_stopping.load() && _generation.load() == generation;
		};
		const auto ok = current() && (job.barrier
			? (!clear || ClearEmojiExport(_directory, current)) : job.work(current));
		if (job.barrier && ok) {
			const auto lock = std::lock_guard(_mutex);
			if (current()) _clearRequired = false;
		}
		completed(std::move(job), ok);
	}
}

}
