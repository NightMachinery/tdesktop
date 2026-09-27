#include "purple/purple_pinned_music.h"

#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <set>

namespace Purple {
namespace {

constexpr auto kPinnedPageSize = 100;
constexpr auto kNeighborLimit = 20;
constexpr auto kAlbumWindow = 100;

class PinnedMusicDownload final
	: public std::enable_shared_from_this<PinnedMusicDownload> {
public:
	PinnedMusicDownload(
		not_null<Data::Thread*> thread,
		std::shared_ptr<Ui::Show> show,
		int before,
		int after,
		bool albums)
	: _history(thread->owningHistory())
	, _pinnedHistory(_history)
	, _show(std::move(show))
	, _before(before)
	, _after(after)
	, _albums(albums) {
		_topicRootId = thread->topicRootId();
	}

	void start() {
		loadPinned(0);
	}

private:
	void request(
		not_null<PeerData*> peer,
		MTPMessagesFilter filter,
		MsgId offsetId,
		int addOffset,
		int limit,
		Fn<void(const QVector<MTPMessage> &)> done) {
		const auto self = shared_from_this();
		const auto session = &_history->session();
		using Flag = MTPmessages_Search::Flag;
		session->api().request(MTPmessages_Search(
			MTP_flags(_topicRootId ? Flag::f_top_msg_id : Flag(0)),
			peer->input(),
			MTP_string(),
			MTP_inputPeerEmpty(),
			MTPInputPeer(),
			MTPVector<MTPReaction>(),
			MTP_int(_topicRootId),
			filter,
			MTP_int(0),
			MTP_int(0),
			MTP_int(offsetId.bare),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(crl::guard(session, [self, peer, done = std::move(done)](
				const MTPmessages_Messages &result) {
			self->apply(peer, result, done);
		})).fail(crl::guard(session, [self](const MTP::Error &) {
			self->failed();
		})).send();
	}

	void apply(
		not_null<PeerData*> peer,
		const MTPmessages_Messages &result,
		const Fn<void(const QVector<MTPMessage> &)> &done) {
		result.match([&](const MTPDmessages_messagesNotModified &) {
			done({});
		}, [&](const auto &data) {
			auto &owner = _history->owner();
			owner.processUsers(data.vusers());
			owner.processChats(data.vchats());
			peer->processTopics(data.vtopics());
			const auto &messages = data.vmessages().v;
			owner.processMessages(messages, NewMessageType::Existing);
			done(messages);
		});
	}

	void loadPinned(MsgId offsetId) {
		const auto self = shared_from_this();
		request(
			_pinnedHistory->peer,
			MTP_inputMessagesFilterPinned(),
			offsetId,
			0,
			kPinnedPageSize,
			[self](const QVector<MTPMessage> &messages) {
				for (const auto &message : messages) {
					const auto id = FullMsgId(
						PeerFromMessage(message),
						IdFromMessage(message));
					if (const auto item = self->_history->owner().message(id)) {
						const auto media = item->media();
						const auto document = media ? media->document() : nullptr;
						if (document && document->isSong()) {
							self->_anchors.push_back(id);
						}
					}
				}
				const auto next = messages.empty()
					? MsgId()
					: IdFromMessage(messages.back());
				if (messages.size() == kPinnedPageSize
					&& next
					&& next != self->_lastPinnedOffset) {
					self->_lastPinnedOffset = next;
					self->loadPinned(next);
				} else if (!self->_topicRootId
					&& self->_pinnedHistory == self->_history
					&& self->_history->migrateFrom()) {
					self->_pinnedHistory = self->_history->migrateFrom();
					self->_lastPinnedOffset = 0;
					self->loadPinned(0);
				} else if (self->_anchors.empty()) {
					self->toast(tr::lng_pinned_music_none(tr::now));
				} else {
					self->nextAnchor();
				}
			});
	}

	void queue(const QVector<MTPMessage> &messages) {
		for (const auto &message : messages) {
			queue(FullMsgId(
				PeerFromMessage(message),
				IdFromMessage(message)));
		}
	}

	void queue(FullMsgId id) {
		if (_queued.contains(id)) {
			return;
		}
		const auto item = _history->owner().message(id);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		if (document && document->isAudioFile()) {
			_queued.insert(id);
			document->save(Data::FileOrigin(id), QString());
			++_downloaded;
		}
	}

	void nextAnchor() {
		if (_anchorIndex == _anchors.size()) {
			toast(tr::lng_pinned_music_started(
				tr::now,
				lt_count,
				_downloaded));
			return;
		}
		const auto anchor = _anchors[_anchorIndex++];
		queue(anchor);
		if (_before) {
			const auto self = shared_from_this();
			request(
				_history->owner().peer(anchor.peer),
				MTP_inputMessagesFilterMusic(),
				anchor.msg,
				0,
				_before,
				[self, anchor](const QVector<MTPMessage> &messages) {
					self->queue(messages);
					self->loadAfter(anchor);
				});
		} else {
			loadAfter(anchor);
		}
	}

	void loadAfter(FullMsgId anchor) {
		if (_after) {
			const auto self = shared_from_this();
			request(
				_history->owner().peer(anchor.peer),
				MTP_inputMessagesFilterMusic(),
				MsgId(anchor.msg.bare + 1),
				-_after,
				_after,
				[self, anchor](const QVector<MTPMessage> &messages) {
					self->queue(messages);
					self->loadAlbum(anchor);
				});
		} else {
			loadAlbum(anchor);
		}
	}

	void loadAlbum(FullMsgId anchor) {
		const auto item = _history->owner().message(anchor);
		const auto groupId = item ? item->groupId() : MessageGroupId();
		if (!_albums || !groupId) {
			nextAnchor();
			return;
		}
		const auto self = shared_from_this();
		const auto session = &_history->session();
		const auto peer = _history->owner().peer(anchor.peer);
		const auto done = crl::guard(session, [self, peer, groupId](
				const MTPmessages_Messages &result) {
			self->apply(peer, result, [self, groupId](
						const QVector<MTPMessage> &messages) {
					for (const auto &message : messages) {
						const auto id = FullMsgId(
							PeerFromMessage(message),
							IdFromMessage(message));
						const auto item = self->_history->owner().message(id);
						if (item && item->groupId() == groupId) {
							self->queue(id);
						}
					}
					self->nextAnchor();
				});
		});
		const auto fail = crl::guard(session, [self](const MTP::Error &) {
			self->failed();
		});
		if (_topicRootId) {
			session->api().request(MTPmessages_GetReplies(
				peer->input(),
				MTP_int(_topicRootId),
				MTP_int(anchor.msg.bare),
				MTP_int(0),
				MTP_int(-kAlbumWindow / 2),
				MTP_int(kAlbumWindow),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done(done).fail(fail).send();
		} else {
			session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(anchor.msg.bare),
				MTP_int(0),
				MTP_int(-kAlbumWindow / 2),
				MTP_int(kAlbumWindow),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done(done).fail(fail).send();
		}
	}

	void failed() {
		toast(tr::lng_pinned_music_failed(tr::now));
	}

	void toast(const QString &message) {
		if (_show && *_show) {
			_show->showToast(message);
		}
	}

	not_null<History*> _history;
	not_null<History*> _pinnedHistory;
	std::shared_ptr<Ui::Show> _show;
	std::vector<FullMsgId> _anchors;
	std::set<FullMsgId> _queued;
	MsgId _lastPinnedOffset = 0;
	size_t _anchorIndex = 0;
	int _before = 0;
	int _after = 0;
	int _downloaded = 0;
	bool _albums = false;
	MsgId _topicRootId = 0;

};

void PinnedMusicBox(
		not_null<Ui::GenericBox*> box,
		not_null<Data::Thread*> thread,
		std::shared_ptr<Ui::Show> show) {
	const auto weak = base::make_weak(thread);
	box->setTitle(tr::lng_pinned_music_title());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_pinned_music_before(),
		st::boxLabel));
	const auto before = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		tr::lng_pinned_music_before(),
		u"1"_q));
	box->addSkip(st::boxMediumSkip);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_pinned_music_after(),
		st::boxLabel));
	const auto after = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		tr::lng_pinned_music_after(),
		u"1"_q));
	before->setMaxLength(2);
	after->setMaxLength(2);
	box->addSkip(st::boxMediumSkip);
	const auto albums = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::lng_pinned_music_album(tr::now),
		true,
		st::defaultBoxCheckbox));
	box->addButton(tr::lng_media_download(), [=] {
		bool beforeOk = false;
		bool afterOk = false;
		const auto beforeCount = before->getLastText().toInt(&beforeOk);
		const auto afterCount = after->getLastText().toInt(&afterOk);
		if (!beforeOk || beforeCount < 0 || beforeCount > kNeighborLimit) {
			before->showError();
			return;
		} else if (!afterOk || afterCount < 0
			|| afterCount > kNeighborLimit) {
			after->showError();
			return;
		}
		const auto includeAlbums = albums->checked();
		const auto strong = weak.get();
		if (!strong) {
			box->closeBox();
			return;
		}
		box->closeBox();
		std::make_shared<PinnedMusicDownload>(
			strong,
			show,
			beforeCount,
			afterCount,
			includeAlbums)->start();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void ShowPinnedMusicBox(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread) {
	const auto show = controller->uiShow();
	controller->show(Box(PinnedMusicBox, thread, show));
}

} // namespace Purple
