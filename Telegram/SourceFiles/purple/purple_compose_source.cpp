#include "purple/purple_compose_source.h"

#include "api/api_common.h"
#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_compose_receipt.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_markdown_source.h"
#include "purple/purple_config.h"
#include "ui/widgets/fields/input_field.h"
#include <QtCore/QJsonArray>
#include <algorithm>
#include <set>

namespace Purple {

struct DesktopComposeSource {
	Main::Session *owner = nullptr;
	QByteArray bytes;
	TextWithTags parsed;
	ArchiveMessageKey key;
	std::function<bool()> current;
	Fn<void(ArchiveEvent, ArchiveSource)> submit;
	crl::time created = crl::now();
	bool bound = false;
};

namespace {

std::vector<std::weak_ptr<DesktopComposeSource>> Pending;
std::set<Main::Session*> Registered;
constexpr auto kComposeLifetime = crl::time(5 * 60 * 1000);

bool Current(const std::shared_ptr<DesktopComposeSource> &source) {
	return source && !source->bytes.isEmpty()
		&& crl::now() - source->created < kComposeLifetime
		&& source->current && source->current();
}

QJsonArray WireEntities(const QVector<MTPMessageEntity> &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		auto buffer = mtpBuffer();
		entity.write(buffer);
		result.push_back(QJsonObject{{u"tl"_q, QString::fromLatin1(QByteArray(
			reinterpret_cast<const char*>(buffer.constData()),
			buffer.size() * sizeof(mtpPrime)).toBase64())}});
	}
	return result;
}

void Complete(const std::shared_ptr<DesktopComposeSource> &source,
		const base::weak_ptr<Main::Session> &weak,
		HistoryItem *owner,
		FullMsgId local,
		uint64 random,
		ArchiveContent expected,
		const MTPUpdates &updates) {
	if (!Current(source) || !weak) {
		return;
	}
	const auto release = gsl::finally([&] { source->bytes.clear(); source->parsed = {}; });
	const auto observed = DesktopComposeAcknowledgement(source->key, random, expected, updates);
	if (!observed || weak->data().message(local.peer, MsgId(observed->key.messageId)) != owner
		|| !Current(source)) {
		return;
	}
	auto genuine = ArchiveSource();
	genuine.kind = ArchiveSourceKind::ComposeMarkdown;
	genuine.bytes = source->bytes;
	genuine.binding = ArchiveSourceBinding{{}, {}, local.msg.bare, int64(random)};
	source->submit(std::move(*observed), std::move(genuine));
}

}

void PrepareDesktopCompose(Api::MessageToSend &message, Ui::InputField *field) {
	if (!field || message.action.options.scheduled || message.action.options.shortcutId
		|| message.action.options.ttlSeconds || !message.webPage.url.isEmpty()) {
		return;
	}
	const auto raw = field->getTextWithTags();
	if (raw.text.isEmpty() || !raw.tags.empty() || raw.text.toUtf8().size() > kDesktopMarkdownSourceLimit
		|| raw.text == message.textWithTags.text
		|| field->getTextWithAppliedMarkdown() != message.textWithTags) {
		return;
	}
	auto bytes = qsizetype(0);
	std::erase_if(Pending, [&](const auto &entry) {
		const auto candidate = entry.lock();
		if (!Current(candidate)) {
			if (candidate) { candidate->bytes.clear(); candidate->parsed = {}; }
			return true;
		}
		bytes += candidate->bytes.size();
		return false;
	});
	if (Pending.size() >= 64 || bytes + raw.text.toUtf8().size() > 2 * kDesktopMarkdownSourceLimit) {
		return;
	}
	const auto session = &message.action.history->session();
	const auto key = ArchiveMessageKey{{}, DesktopArchiveChat(message.action.history->peer->id), 1};
	auto source = std::make_shared<DesktopComposeSource>();
	const auto ingress = DesktopComposeSourceIngress(session, key.chat);
	if (!ingress) {
		return;
	}
	source->key = {ingress->epoch.account, key.chat, 1};
	source->current = ingress->current;
	source->submit = ingress->submit;
	source->bytes = raw.text.toUtf8();
	source->parsed = message.textWithTags;
	source->owner = session;
	if (Registered.insert(session).second) {
		ArchiveStorageChanges() | rpl::on_next([] {
			for (const auto &entry : Pending) {
				if (const auto live = entry.lock()) {
					if (!Current(live)) { live->bytes.clear(); live->parsed = {}; }
				}
			}
		}, session->lifetime());
		session->lifetime().add([session] {
			Registered.erase(session);
			for (const auto &entry : Pending) {
				if (const auto live = entry.lock()) {
					if (live->owner == session) { live->bytes.clear(); live->parsed = {}; }
				}
			}
		});
	}
	Pending.push_back(source);
	message.purpleMarkdownSource = std::move(source);
}

Fn<void(const MTPUpdates &)> BindDesktopCompose(
		const std::shared_ptr<DesktopComposeSource> &source,
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		uint64 randomId,
		const TextWithEntities &sending,
		const QVector<MTPMessageEntity> &entities,
		bool singlePart) {
	if (!Current(source) || source->bound) {
		return {};
	}
	source->bound = true;
	if (!singlePart || source->owner != session.get()
		|| source->key.account != SessionAccountKey(session) || item->isScheduled() || item->shortcutId() || !randomId
		|| item->id.bare >= 0 || item->media() || item->richPage()
		|| source->parsed.text != sending.text || entities.empty()
		|| source->key.chat != DesktopArchiveChat(item->history()->peer->id)) {
		source->bytes.clear();
		return {};
	}
	auto content = ArchiveContent();
	content.text = sending.text;
	content.entities = WireEntities(entities);
	return [source, weak = base::make_weak(session.get()), owner = item.get(),
		local = item->fullId(), randomId, content = std::move(content)](const MTPUpdates &result) {
		Complete(source, weak, owner, local, randomId, content, result);
	};
}

}
