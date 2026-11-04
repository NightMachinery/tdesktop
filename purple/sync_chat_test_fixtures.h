#pragma once
#include "purple/purple_sync_chat_requests.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_history_scanner.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <unistd.h>
#include <cstdio>
#include "sync-chat-native-cases.inc"
#include "sync-chat-native-meta.inc"
using namespace Purple;
namespace {
auto Checks = 0;
auto Failures = 0;
#define CHECK(value) do { ++Checks; if (!(value)) { ++Failures; std::printf("FAIL line%d: %s\n", __LINE__, #value); } } while (false)
const auto Origin = DesktopSyncChatOrigin{{0, 123}, u"login-a"_q};
MTPMessage Message(int id, int64_t channel = 456) {
	auto message = NativeMessage();
	message.id = MTP_int(id);
	message.date = MTP_int(100);
	message.peer_id = MTP_peerChannel(MTP_long(channel));
	return message.Build();
}

template <typename Value>
Value Read(const mtpPrime *&from, const mtpPrime *end) {
	auto result = Value();
	CHECK(result.read(from, end));
	return result;
}

DesktopSyncChatPeer Peer(PeerToken chat) {
	auto input = DesktopSyncChatPeerInput{Origin, chat}; input.canSendFiles = true; input.chosenSendAs = PeerToken{};
	if (chat == PeerToken{}) {
		auto user = NativeUser(); user.flags = MTP_flags(MTPDuser::Flag::f_self); user.id = MTP_long(123);
		auto full = NativeUserFull(); full.id = MTP_long(123); input.user = user.Build(); input.userFull = full.Build();
	} else {
		auto group = NativeChannel(); group.id = MTP_long(chat.id); group.flags = MTP_flags(MTPDchannel::Flag::f_broadcast | MTPDchannel::Flag::f_creator);
		auto full = NativeChannelFull(); full.id = MTP_long(chat.id); full.flags = MTP_flags(MTPDchannelFull::Flag::f_admins_count | MTPDchannelFull::Flag::f_participants_count);
		full.admins_count = MTP_int(1); full.participants_count = MTP_int(1); input.group = group.Build(); input.groupFull = full.Build();
	}
	return MapDesktopSyncChatPeer(Origin, input);
}

MTPMessage PlainMessage(PeerToken chat, int id, int date = 100, const char *text = "ordinary") {
	auto value = NativeMessage(); value.id = MTP_int(id); value.date = MTP_int(date); value.flags = MTP_flags(MTPDmessage::Flag::f_out);
	value.peer_id = chat == PeerToken{} ? MTP_peerUser(MTP_long(123)) : MTPPeer(MTP_peerChannel(MTP_long(chat.id)));
	value.message = MTP_string(text); return value.Build();
}
}
