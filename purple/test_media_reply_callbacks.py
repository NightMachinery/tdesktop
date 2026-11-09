#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description="Exercise literal native revocation and completion bodies over inert session ports")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
source = repo / "Telegram/SourceFiles/purple/purple_media_reply_adapter.cpp"
text = source.read_text()
def block(marker):
    start = text.index("{", text.index(marker) + len(marker))
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]
state = "struct State " + block("struct State") + ";\n"
activate = "void Activate(not_null<Main::Session*> session, const std::shared_ptr<State> &state) " + block("void Activate(")
done = block(")).done([=](const MTPUpdates &result)")
failed = block("}).fail([=](const MTP::Error &")
failure_reason = "MediaReplyStatusReason FailureReason(const MTP::Error &error) " + block("MediaReplyStatusReason FailureReason(")
persist = "bool Persist(not_null<Main::Session*> session, const std::shared_ptr<State> &state, const MediaReplyAction &action) " + block("bool Persist(")
policy_enabled = "bool PolicyEnabled(const Settings &settings) " + block("bool PolicyEnabled(")
fixture = r'''
#include "purple/purple_settings.h"
#include "purple/purple_media_reply_presentation.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <functional>
#include <type_traits>
#include <QtCore/QLockFile>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
using mtpRequestId = int;
template <class T> using not_null = T;
struct MTPUpdates {};
struct MTPMessage {};
#include "purple/purple_media_reply_adapter.h"
namespace MTP {
struct Error { int value = 400; QString name; int code() const { return value; } const QString &type() const { return name; } };
@NATIVE_SENDER@
}
namespace base::unixtime { int64 clock = 100; int64 now() { return clock; } }
namespace Main {
struct Session;
struct Account {
    QString id = u"0-42"_q;
    bool leaving = false;
    Session *live = nullptr;
    bool loggingOut() { return leaving; }
    Session *maybeSession() { return live; }
};
struct Api {
    std::vector<int> cancelled;
    int reconciled = 0;
    struct Request { Api *api; int id; void cancel() { api->cancelled.push_back(id); } };
    Request request(int id) { return {this, id}; }
    void applyUpdates(const MTPUpdates &) { ++reconciled; }
};
struct Session {
    Account owner;
    Api transport;
    Session() { owner.live = this; }
    Account &account() { return owner; }
    Api &api() { return transport; }
};
}
struct Weak { Main::Session *value; Main::Session *get() const { return value; } };
namespace Purple {
Settings settings;
Settings &AccountSettings(Main::Session *) { return settings; }
QString AccountText(Main::Session *session) { return session->account().id; }
@STATE@
@ACTIVATE@
@FAILURE_REASON@
QString StorageDirectory;
struct Target { QString path; };
std::optional<Target> AccountSettingsTarget(Main::Session *) { return Target{StorageDirectory + u"/settings.toml"_q}; }
bool ValidateAccountSettingsTarget(Main::Session *, const Target &) { return true; }
@PERSIST@
@POLICY_ENABLED@
}
int checks = 0;
void check(bool value, const char *why) {
    ++checks;
    if (!value) { std::cerr << why << '\n'; std::exit(1); }
}
int main(int argc, char **argv) {
    using namespace Purple;
    Main::Session session;
    auto state = std::make_shared<State>();
    Activate(&session, state);
    check(state->journal.account == u"0-42"_q && state->journal.cutoff == 100 && state->journal.revision == 1, "activation binds account epoch");
    settings.mediaReply.defaults[0].voice = MediaReplyMode::Both;
    Activate(&session, state);
    const auto action = PlanMediaReply(settings, {u"0-42"_q, {PeerTokenKind::User, 17}, ChatKind::Private, 101, 0, 101, MediaReplyMedia::Voice, MediaReplyOrigin::LiveNew, false, true, false}, u"0-42"_q, 100, state->journal.revision);
    check(action.has_value(), "genuine new message plans");
    check(ReserveMediaReply(state->journal, *action), "reserve before enqueue");
    const auto requestId = std::make_shared<mtpRequestId>(11);
    auto weak = Weak{&session};
    const auto done = [=](const MTPUpdates &result) @DONE@;
    const auto failed = [=](const MTP::Error &error) @FAIL@;
    state->requests.emplace(11, action->revision);
    state->requests.emplace(12, action->revision);
    state->inFlight = 2;
    settings.mediaReply.defaults[0].voice = MediaReplyMode::Off;
    base::unixtime::clock = 102;
    Activate(&session, state);
    check(session.api().cancelled == std::vector<int>({11, 12}), "disable cancels exact owned tokens");
    check(state->requests.empty() && state->inFlight == 0, "cancel retires capacity");
    check(state->policy.defaults[0].voice == MediaReplyMode::Off && state->journal.revision == 3 && state->journal.cutoff == 102, "disable changes policy and epoch");
    check(!ReserveMediaReply(state->journal, *action) && state->journal.reserved.size() == 1, "cancel keeps terminal reservation");
    done({}); failed({});
    check(state->inFlight == 0 && session.api().reconciled == 1, "late acceptance reconciles without renewed capacity or send");
    state->requests.emplace(21, state->journal.revision);
    state->inFlight = 1;
    done({}); failed({});
    check(state->inFlight == 1 && state->requests.contains(21), "old callbacks leave newer request capacity");
    *requestId = 21;
    failed({}); failed({});
    check(state->inFlight == 0 && state->requests.empty(), "completion accounting retires once");
    const auto reconciled = session.api().reconciled;
    session.owner.leaving = true; done({});
    check(session.api().reconciled == reconciled, "logout rejects late reconciliation");
    session.owner.leaving = false; session.owner.live = nullptr; done({});
    check(session.api().reconciled == reconciled, "replacement session rejects late reconciliation");
    session.owner.live = &session; session.owner.id = u"0-43"_q; done({});
    check(session.api().reconciled == reconciled, "replacement account rejects late reconciliation");
    session.owner.id = u"0-42"_q;
    settings.mediaReply.text = u"changed"_q;
    state->requests.emplace(31, state->journal.revision); state->inFlight = 1;
    Activate(&session, state);
    check(session.api().cancelled.back() == 31 && state->policy.text == u"changed"_q && state->inFlight == 0, "rule replacement cancels owned token");
    const auto encoded = EncodeMediaReplyJournal(state->journal);
    const auto decoded = DecodeMediaReplyJournal(encoded, u"0-42"_q);
    check(decoded && decoded->reserved.size() == 1 && decoded->reserved.front() == *action, "cancelled unknown acceptance survives restart");
    const auto cancelled = state->reason;
    failed({400, u"ALLOW_PAYMENT_REQUIRED_1"_q});
    check(state->reason == cancelled, "old revision cannot publish paid status");
    check(FailureReason({400, u"ALLOW_PAYMENT_REQUIRED_123"_q}) == MediaReplyStatusReason::ApprovalRequired, "paid approval has bounded reason");
    check(FailureReason({420, u"FLOOD_WAIT_10"_q}) == MediaReplyStatusReason::Flood, "flood refusal has bounded reason");
    check(FailureReason({400, u"CHAT_WRITE_FORBIDDEN"_q}) == MediaReplyStatusReason::Permission, "permission refusal has bounded reason");
    check(FailureReason({500, u"PRIVATE_SERVER_DIAGNOSTIC"_q}) == MediaReplyStatusReason::Uncertain, "unknown error sanitized to uncertainty");
    MTP::Sender sender;
    MTP::Builder builder;
    builder.@BUILDER@();
    auto retired = 0;
    const auto terminal = builder.MakeFailHandler(&sender, std::function<void(const MTP::Error&)>([&](const auto &) { ++retired; }), builder.policy);
    for (const auto code : {-1, 500, 503, 420, 403}) check(terminal({code, code == 420 ? u"FLOOD_WAIT_1"_q : u"RPC_FAIL"_q}, {71}), "actual selected Sender policy consumes error without default retry");
    check(retired == 5 && sender.handled == 5, "selected native policy retires exact requests");
    MTP::Builder reviewed;
    reviewed.handleFloodErrors();
    const auto oldPolicy = reviewed.MakeFailHandler(&sender, std::function<void(const MTP::Error&)>([&](const auto &) { ++retired; }), reviewed.policy);
    check(!oldPolicy({-1, u"CONNECTION_NOT_INITED"_q}, {72}) && !oldPolicy({500, u"RPC_FAIL"_q}, {72}) && retired == 5, "reviewed native flood-only builder exposes default retry path");
    settings = Settings();
    auto row = MediaReplyRow();
    check(MediaReplyRoundDisabled(settings, row, true), "initial checked follow disables independent mode");
    settings.mediaReply.defaults[0] = {MediaReplyMode::Off, false, MediaReplyMode::Both};
    row.roundFollow = false;
    check(MediaReplyModeSelection(row.round, true) == 4 && !MediaReplyRoundDisabled(settings, row, true), "missing independent round selects inheritance rather than Off");
    row.roundFollow.reset();
    check(!MediaReplyRoundDisabled(settings, row, true), "inherited false or mixed follow enables editing");
    row.round = MediaReplyMode::Off;
    check(MediaReplyModeSelection(row.round, true) == 0, "explicit Off remains distinct");
    row.round = MediaReplyMode::Received;
    check(MediaReplyModeSelection(row.round, true) == 2, "independent selected value is stable");
    check(PolicyEnabled(settings), "core-derived effective Both enables status");
    settings = Settings();
    check(!PolicyEnabled(settings), "initial policy reports disabled");
    const auto root = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(root);
    const auto storageCase = [&](const QString &name) {
        StorageDirectory = root + '/' + name; QDir().mkpath(StorageDirectory);
        const auto value = std::make_shared<State>(); Activate(&session, value); return value;
    };
    auto saved = storageCase(u"success"_q);
    auto storageAction = *action; storageAction.message.date = saved->journal.cutoff + 1; storageAction.revision = saved->journal.revision;
    check(Persist(&session, saved, storageAction) && saved->committed == 1, "literal native checked writer commits before capacity ACK");
    auto locked = std::make_shared<State>(); Activate(&session, locked);
    check(!Persist(&session, locked, storageAction) && locked->reason == MediaReplyStatusReason::LockUnavailable && locked->committed == -1, "real native journal lock refusal is visible");
    auto corrupt = storageCase(u"corrupt"_q);
    { auto file = QFile(StorageDirectory + u"/media-reply-journal.json"_q); check(file.open(QIODevice::WriteOnly) && file.write("x") == 1, "corrupt fixture written"); }
    check(!Persist(&session, corrupt, storageAction) && corrupt->reason == MediaReplyStatusReason::JournalUnreadable, "literal corrupt decode refuses visibly");
    auto full = storageCase(u"full"_q);
    auto ledger = full->journal;
    for (auto id = 1; id <= 4096; ++id) { auto entry = storageAction; entry.message.message = id; entry.randomId = MediaReplyRandomId(id); ledger.reserved.push_back(entry); }
    { auto file = QFile(StorageDirectory + u"/media-reply-journal.json"_q); const auto bytes = EncodeMediaReplyJournal(ledger); check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "full fixture committed"); }
    check(!Persist(&session, full, storageAction) && full->reason == MediaReplyStatusReason::JournalFull && full->committed == 4096, "journal capacity refusal shows confirmed count");
    auto unwritable = storageCase(u"write-refusal"_q);
    unwritable->loaded = true; unwritable->committed = 0;
    unwritable->path = StorageDirectory + u"/media-reply-journal.json"_q;
    unwritable->lock = std::make_unique<QLockFile>(unwritable->path + u".lock"_q);
    check(unwritable->lock->tryLock(0) && QDir().mkpath(unwritable->path), "real write-refusal fixture prepared");
    check(!Persist(&session, unwritable, storageAction) && unwritable->reason == MediaReplyStatusReason::StorageRefused && unwritable->committed == 0, "write refusal cannot publish false confirmed reservation");
    std::cout << "MEDIA_REPLY_CALLBACK_CHECKS=" << checks << '\n';
}
'''
sender_path = repo / "Telegram/SourceFiles/mtproto/sender.h"
response_path = repo / "Telegram/SourceFiles/mtproto/mtproto_response.h"
sender_text = sender_path.read_text()
response_text = response_path.read_text()
def native_block(body, marker):
    opening = body.index("{", body.index(marker) + len(marker))
    end, depth = opening + 1, 1
    while depth:
        depth += (body[end] == "{") - (body[end] == "}")
        end += 1
    return body[opening:end]
predicates = "\n".join(signature + " " + native_block(response_text, signature) for signature in [
    "inline bool IsFloodError(const QString &type)", "inline bool IsFloodError(const Error &error)",
    "inline bool IsTemporaryError(const Error &error)", "inline bool IsDefaultHandledError(const Error &error)"])
native_sender = predicates + "\n" + r"""
struct Response { int requestId = 0; };
struct Sender { int handled = 0; void senderRequestHandled(int) { ++handled; } };
using FailHandler = std::function<bool(const Error&, const Response&)>;
template <class T> constexpr bool false_t(T) { return false; }
struct Builder {
    enum class FailSkipPolicy { Simple, HandleFlood, HandleAll };
    FailSkipPolicy policy = FailSkipPolicy::Simple;
    template <typename... Args> static constexpr bool IsCallable = std::is_invocable_v<Args...>;
    void setFailSkipPolicy(FailSkipPolicy value) { policy = value; }
    Builder &handleFloodErrors() noexcept @FLOOD_BUILDER@
    Builder &handleAllErrors() noexcept @ALL_BUILDER@
    template <typename Handler> FailHandler MakeFailHandler(not_null<Sender*> sender, Handler &&handler, FailSkipPolicy skipPolicy) @FAIL_HANDLER@
};
"""
native_sender = native_sender.replace("@FLOOD_BUILDER@", native_block(sender_text, "SpecificRequestBuilder &handleFloodErrors()"))
native_sender = native_sender.replace("@ALL_BUILDER@", native_block(sender_text, "SpecificRequestBuilder &handleAllErrors()"))
native_sender = native_sender.replace("@FAIL_HANDLER@", native_block(sender_text, "[[nodiscard]] FailHandler MakeFailHandler("))
assert ").handleAllErrors().send()" in text and ").handleFloodErrors().send()" not in text
fixture = fixture.replace("@NATIVE_SENDER@", native_sender).replace("@BUILDER@", "handleAllErrors")
fixture = fixture.replace("@PERSIST@", persist).replace("@FAILURE_REASON@", failure_reason).replace("@POLICY_ENABLED@", policy_enabled)
fixture = fixture.replace("@STATE@", state).replace("@ACTIVATE@", activate).replace("@DONE@", done).replace("@FAIL@", failed)
overlay = out / "include/purple"
overlay.mkdir(parents=True, exist_ok=True)
for name in ["purple_media_reply_presentation.h", "purple_media_reply_adapter.h"]:
    (overlay / name).write_bytes((repo / "Telegram/SourceFiles/purple" / name).read_bytes())
(out / "callbacks.cpp").write_text(fixture)
target = "Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o"
lines = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True)
recipe = next(shlex.split(line) for line in lines.splitlines() if " -c " in line and "purple_config_file.cpp" in line)
flags = ["-std=c++20", "-I" + str(out / "include"), "-I" + str(repo / "Telegram/ThirdParty/purple_core")]
for flag in ["-arch", "-isysroot"]:
    if flag in recipe: flags += [flag, recipe[recipe.index(flag) + 1]]
for flag in recipe:
    if flag.startswith("-F"): flags += [flag, "-Wl,-rpath," + flag[2:]]
units = [out / "callbacks.cpp", *[repo / "Telegram/ThirdParty/purple_core/purple" / (name + ".cpp") for name in ["purple_media_reply", "purple_peer_token"]]]
commands = []
objects = []
for path in units:
    obj = out / (path.stem + ".o")
    command = [recipe[0], *flags, "-MD", "-MF", str(obj.with_suffix(".d")), "-c", str(path), "-o", str(obj)]
    with (out / (path.stem + ".log")).open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    commands.append(command); objects.append(str(obj))
link = [recipe[0], *flags, *objects, "-framework", "QtCore", "-o", str(out / "callbacks")]
subprocess.run(link, check=True)
result = subprocess.run([str(out / "callbacks"), str(out / "storage")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(out / "run.log").write_bytes(result.stdout)
(out / "inputs.json").write_text(json.dumps({"source": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "literalBodies": {"state": state, "activate": activate, "done": done, "fail": failed, "persist": persist, "failureReason": failure_reason, "nativeSender": native_sender}, "senderSha256": hashlib.sha256(sender_path.read_bytes()).hexdigest(), "responseSha256": hashlib.sha256(response_path.read_bytes()).hexdigest(), "compile": commands, "link": link, "exit": result.returncode, "boundary": "Literal production activation/storage/completion and actual native Sender builder/error-policy bodies; real Qt journal IO/lock and shared policy, inert session/request ports. No native session construction, GUI, transport or live account."}, indent=2) + "\n")
print(result.stdout.decode(), end="")
raise SystemExit(result.returncode)
