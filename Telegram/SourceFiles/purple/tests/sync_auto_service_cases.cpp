#include "base/weak_ptr.h"
#include "rpl/rpl.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <iostream>
#include <map>
#include <optional>
#include <vector>

namespace Probe {
enum class SyncAutoEvent { SessionReady, Foreground, Enable, Disable, Retry };
struct AccountKey { int environment = 0; int user = 0; bool operator==(const AccountKey&) const = default; };
enum class SettingsOpenRole { Own };
struct AccountSettingsOpenTarget { int epoch = 0; bool operator==(const AccountSettingsOpenTarget&) const = default; };
struct Account;
struct Session final : QObject, base::has_weak_ptr {
    Account *owner = nullptr;
    AccountKey key;
    int context = 1;
    bool fileEligible = true;
    rpl::lifetime life;
    explicit Session(Account *a) : owner(a) {}
    ~Session() { life.destroy(); }
    Account &account() { return *owner; }
    rpl::lifetime &lifetime() { return life; }
};
struct Record { bool enabled = false, runStopped = false; int64_t retryAt = 0; };
struct Account {
    std::unique_ptr<Session> session;
    Record record;
    bool logout = false;
    QByteArray token = "binding";
    rpl::event_stream<Session*> changed;
    Account() : session(std::make_unique<Session>(this)) {}
    Session *maybeSession() { return session.get(); }
    bool loggingOut() const { return logout; }
    rpl::producer<Session*> sessionChanges() const { return changed.events(); }
};
struct Files { Session *session = nullptr; };
struct Target { Files files; };
struct DesktopSyncAutoRead { std::optional<Record> record; };
bool Owner = true;
AccountKey SessionAccountKey(Session *session) { return session->key; }
std::optional<int> SettingsStartupOwner() { return Owner ? std::optional<int>(1) : std::nullopt; }
QByteArray AccountSyncBindingToken(Account &a) { return a.token; }
std::optional<AccountSettingsOpenTarget> AccountSettingsOpenTargetFor(Session *s, SettingsOpenRole) {
    return s->fileEligible ? std::optional(AccountSettingsOpenTarget{s->context}) : std::nullopt;
}
bool SameDesktopSyncAutoContext(const AccountSettingsOpenTarget &a, const AccountSettingsOpenTarget &b) { return a == b; }
bool AutomaticSettingsSyncEnabled(Session *s) { return s->account().record.enabled; }
std::optional<Target> AccountSyncInspectionTarget(Session *s) { return s->fileEligible ? std::optional(Target{{s}}) : std::nullopt; }
DesktopSyncAutoRead ReadDesktopSyncAuto(const Files &files, AccountKey) { return { files.session->account().record }; }
struct Gate { bool held = false; bool postingHeld() const { return held; } } WriteGate;
Gate *DesktopSyncWriteGateFor(AccountKey) { return &WriteGate; }
constexpr auto kSyncAutoCheckSeconds = 15 * 60;
crl::time Now = 1;
crl::time ClockNow() { return Now; }
class ClockTimer;
std::vector<ClockTimer*> Timers;
void Deliver(Fn<void()> callback) { QTimer::singleShot(0, QCoreApplication::instance(), std::move(callback)); }
class ClockTimer {
public:
    Fn<void()> callback;
    crl::time due = 0;
    int arms = 0;
    bool active = false;
    ClockTimer() { Timers.push_back(this); }
    ~ClockTimer() { std::erase(Timers, this); }
    void setCallback(Fn<void()> next) { callback = std::move(next); }
    void callOnce(crl::time delay, Qt::TimerType = Qt::CoarseTimer) { active = true; due = Now + delay; ++arms; }
    void cancel() { active = false; }
    bool isActive() const { return active; }
};
void Pump() { QCoreApplication::processEvents(); }
void QueueDue() {
    for (const auto timer : Timers) {
        if (timer->active && timer->due <= Now) { timer->active = false; Deliver(timer->callback); }
    }
}
void Advance(crl::time duration) { Now += duration; QueueDue(); Pump(); }
struct Domain {
    struct Entry { std::unique_ptr<Account> account; };
    std::vector<Entry> entries;
    rpl::event_stream<> accountChanges;
    rpl::event_stream<Session*> activeChanges;
    const auto &accounts() const { return entries; }
    auto accountsChanges() const { return accountChanges.events(); }
    auto activeSessionChanges() const { return activeChanges.events(); }
};
struct Application {
    Domain owned;
    rpl::variable<bool> deactivated = false;
    auto &domain() { return owned; }
    auto appDeactivatedValue() const { return deactivated.value(); }
} App;
namespace Core { Application &App() { return Probe::App; } }
rpl::event_stream<> Changes;
rpl::event_stream<void*> Requests;
rpl::event_stream<Session*> Chat;
auto SyncRequestChanges() { return Requests.events(); }
auto SyncChatStatusChanges() { return Chat.events(); }
bool SyncMagicBound(Session*) { return true; }
bool SyncMagicPaused(Session*) { return false; }
void OnMain(Session *session, Fn<void()> callback) {
    QPointer<Session> guard(session);
    Deliver([guard, callback] { if (guard) callback(); });
}
class AutoSession final : public base::has_weak_ptr {
public:
    explicit AutoSession(not_null<Session*> s) : _session(base::make_weak(s.get())), _account(s->key) {}
    ~AutoSession() { RetireBackstop(); }
    void RetireBackstop();
    void RefreshBackstop();
    bool BackstopCurrent() const;
    void Backstop(uint64_t epoch);
    bool Event(SyncAutoEvent event);
    void MoveToBound() {}
    void ChatChanged() { Event(SyncAutoEvent::SessionReady); }
    decltype(base::make_weak((Session*)nullptr)) _session;
    AccountKey _account;
    ClockTimer _backstop;
    std::optional<AccountSettingsOpenTarget> _backstopOpening;
    QByteArray _backstopToken;
    uint64_t _backstopEpoch = 0;
    crl::time _backstopDue = 0;
    int checks = 0, events = 0;
};
std::map<Session*, std::unique_ptr<AutoSession>> Sessions;
void TriggerAutomaticSettingsSync(SyncAutoEvent event);
#include "sync-auto-service.inc"
bool AutoSession::Event(SyncAutoEvent event) {
    const auto refresh = gsl::finally([=] { if (EventRefreshesBackstop) RefreshBackstop(); });
    ++events;
    const auto session = _session.get();
    if (!session) return false;
    if (event == SyncAutoEvent::Enable) session->account().record.enabled = true;
    if (event == SyncAutoEvent::Disable) session->account().record.enabled = false;
    if (event == SyncAutoEvent::Foreground && session->account().record.enabled) ++checks;
    return true;
}

} // namespace Probe

int main(int argc, char **argv) {
    auto app = QApplication(argc, argv);
    using namespace Probe;
    auto checks = 0, failures = 0;
    const auto expect = [&](bool ok, const char *why) { ++checks; if (!ok) { ++failures; std::cerr << why << '\n'; } };
    StartAutomaticSettingsSync();
    auto &domain = Probe::App.owned;
    for (auto i = 0; i != 3; ++i) {
        auto account = std::make_unique<Account>();
        account->session->key.user = i + 1;
        account->record.enabled = i != 2;
        domain.entries.push_back({std::move(account)});
    }
    const auto root = domain.entries[0].account->session.get();
    const auto secondary = domain.entries[1].account->session.get();
    const auto off = domain.entries[2].account->session.get();
    domain.activeChanges.fire_copy(root); Pump();
    auto &one = For(root);
    auto &two = For(secondary);
    expect(one._backstop.isActive(), "stored root account arms the registered idle callback without state");
    expect(two._backstop.isActive(), "stored secondary account arms the registered idle callback");
    expect(!For(off)._backstop.isActive(), "off account owns no armed backstop");
    const auto missingBackstop = !one._backstop.isActive() || !two._backstop.isActive();
    if (missingBackstop) { Sessions.clear(); domain.entries.clear(); return 1; }
    const auto interval = crl::time(kSyncAutoCheckSeconds) * 1000;
    const auto arms = one._backstop.arms;
    StartAutomaticSettingsSync();
    domain.activeChanges.fire_copy(root); domain.accountChanges.fire({}); Pump();
    expect(one._backstop.arms == arms, "duplicate startup/subscriptions do not multiply or postpone timers");
    Deliver(one._backstop.callback); Pump();
    expect(one.checks == 0 && one._backstopDue == Now + interval,
        "early queued callback cannot trigger faster polling");
    Advance(interval - 1);
    expect(one.checks == 0 && two.checks == 0, "idle backstop never polls faster than fifteen minutes");
    Advance(1);
    expect(one.checks == 1 && two.checks == 1, "registered native queued callbacks check both opted-in sessions");
    Advance(interval * 10);
    expect(one.checks == 2 && two.checks == 2, "suspension coalesces overdue callbacks without replay storms");
    const auto stale = one._backstop.callback;
    SetAutomaticSettingsSync(root, false);
    expect(!one._backstop.isActive(), "disable retires the account timer");
    const auto before = one.checks;
    Deliver(stale); Pump();
    expect(one.checks == before, "late disabled callback cannot execute");
    SetAutomaticSettingsSync(root, true);
    Deliver(stale); Pump();
    expect(one.checks == before && one._backstop.isActive(), "old epoch cannot act after re-enable");
    auto prior = one.checks;
    root->context++;
    Advance(interval);
    expect(one.checks == prior && one._backstop.isActive(), "owner/file replacement retires old authority and schedules a fresh delayed generation");
    one.Event(SyncAutoEvent::SessionReady);
    expect(one._backstop.isActive(), "fresh lifecycle event admits the replacement context");
    root->account().token = "replacement";
    Advance(interval);
    expect(one.checks == prior && one._backstop.isActive(), "binding replacement rejects stale effects and retains an eligible fresh backstop");
    one.Event(SyncAutoEvent::SessionReady);
    root->key.environment++;
    Advance(interval);
    expect(one.checks == prior && !one._backstop.isActive(), "environment/login identity change fails closed");
    root->key.environment--;
    one.Event(SyncAutoEvent::SessionReady);
    root->account().record.retryAt = 42;
    Advance(interval);
    expect(one.checks == prior && one._backstop.isActive(), "backstop defers to existing retry budget and rearms");
    root->account().record.retryAt = 0;
    root->account().record.runStopped = true;
    Advance(interval);
    expect(one.checks == prior && root->account().record.runStopped, "idle backstop never renews a stopped run budget");
    root->account().record.runStopped = false;
    WriteGate.held = true; Advance(interval); WriteGate.held = false;
    expect(one.checks == prior && one._backstop.isActive(), "write fence defers effects without losing the backstop");
    Advance(interval);
    expect(one.checks == prior + 1, "eligible idle work resumes through the existing foreground event");
    prior = one.checks;
    Now += interval; QueueDue();
    Probe::App.deactivated = true; Probe::App.deactivated = false;
    Pump();
    expect(one.checks == prior + 1, "foreground replaces overdue queued timers without duplicate work");
    const auto dying = two._backstop.callback;
    const auto key = secondary;
    domain.entries[1].account->session.reset();
    expect(!Sessions.contains(key), "session disposal retires its registered service and timer");
    Deliver(dying); Pump();
    expect(!Sessions.contains(key), "late disposed callback cannot resurrect an account");
    Owner = false;
    Advance(interval);
    expect(!one._backstop.isActive(), "unanswered/replaced startup owner retires timer authority");
    Owner = true;
    one.Event(SyncAutoEvent::SessionReady);
    root->account().logout = true;
    Advance(interval);
    expect(!one._backstop.isActive(), "logout prevents timer effects and retires scheduling");
    std::cout << (checks - failures) << '/' << checks << " actual registered sync backstop checks passed\n";
    Sessions.clear(); domain.entries.clear();
    return failures ? 1 : 0;
}

