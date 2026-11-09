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
failed = block("}).fail([=](const MTP::Error &)")
fixture = r'''
#include "purple/purple_settings.h"
#include <QtCore/QLockFile>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
using mtpRequestId = int;
template <class T> using not_null = T;
struct MTPUpdates {};
namespace MTP { struct Error {}; }
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
}
int checks = 0;
void check(bool value, const char *why) {
    ++checks;
    if (!value) { std::cerr << why << '\n'; std::exit(1); }
}
int main() {
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
    const auto failed = [=](const MTP::Error &) @FAIL@;
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
    std::cout << "MEDIA_REPLY_CALLBACK_CHECKS=" << checks << '\n';
}
'''
fixture = fixture.replace("@STATE@", state).replace("@ACTIVATE@", activate).replace("@DONE@", done).replace("@FAIL@", failed)
(out / "callbacks.cpp").write_text(fixture)
target = "Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o"
lines = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True)
recipe = next(shlex.split(line) for line in lines.splitlines() if " -c " in line and "purple_config_file.cpp" in line)
flags = ["-std=c++20", "-I" + str(repo / "Telegram/ThirdParty/purple_core")]
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
result = subprocess.run([str(out / "callbacks")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(out / "run.log").write_bytes(result.stdout)
(out / "inputs.json").write_text(json.dumps({"source": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "literalBodies": {"state": state, "activate": activate, "done": done, "fail": failed}, "compile": commands, "link": link, "exit": result.returncode, "boundary": "Literal production activation and completion bodies, actual shared policy and journal, inert session/request ports. No native session construction, GUI, transport or live account."}, indent=2) + "\n")
print(result.stdout.decode(), end="")
raise SystemExit(result.returncode)
