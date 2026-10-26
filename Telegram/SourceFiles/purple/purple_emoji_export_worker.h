#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Purple {

using EmojiExportAuthority = std::function<bool()>;

class EmojiExportWorker final : public QObject {
public:
	using Work = std::function<bool(const EmojiExportAuthority&)>;
	using Done = std::function<void(quint64, bool)>;
	explicit EmojiExportWorker(QString directory, QObject *parent = nullptr);
	~EmojiExportWorker();
	[[nodiscard]] quint64 generation() const;
	[[nodiscard]] bool submit(quint64 generation, Work work, Done done);
	quint64 revoke(bool clear, Done done);

private:
	struct Job {
		quint64 generation = 0;
		bool barrier = false;
		Work work;
		Done done;
	};
	void run();
	void completed(Job job, bool ok);

	const QString _directory;
	std::atomic<quint64> _generation = 1;
	std::atomic<bool> _stopping = false;
	std::mutex _mutex;
	std::condition_variable _ready;
	std::optional<Job> _pending;
	bool _clearRequired = false;
	std::thread _thread;

};

}
