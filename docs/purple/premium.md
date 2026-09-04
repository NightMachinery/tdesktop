# Local Premium features

Purple Telegram unlocks the Telegram Premium features that Telegram Desktop
gates on the client alone. Nothing here defeats a server check, and nothing here
is visible to Telegram or to other clients.

The switch lives in Settings > Advanced > Purple > "Local Premium features", and
is on by default. It is stored in `settings.toml` (see [config.md](config.md)).


## Why only some features

Premium enforcement is split between the client and the server, and the split is
not obvious from the outside. For most features the server is the one saying no,
and the client's check exists only to show a friendly box instead of an opaque
error. Flipping those gates would be a downgrade: you would trade
"this needs Premium" for `PREMIUM_ACCOUNT_REQUIRED` on send.

For four features, the desktop app is the only thing saying no. It either
withholds data it already has, or declines to do something it is perfectly
capable of doing. Those are the ones this unlocks.

`Main::Session::premium()` deliberately still tells the truth. Making it return
`true` would light up roughly two hundred other call sites, offering things the
server refuses and putting a Premium badge on your own profile that nobody else
can see. Instead a small `Purple::` helper is consulted at exactly the handful of
places listed below, which also means `git grep Purple::` finds the entire
fork-specific diff when rebasing onto upstream.


## What is unlocked

### No sponsored messages

The server hands sponsored messages to whoever asks for them; Telegram Desktop's
Premium behaviour is literally to stop asking and drop what it already has
(`data/components/sponsored_messages.cpp`). Three surfaces go through that one
component and are covered by early returns in `canHaveFor()` and `isTopBarFor()`:
sponsored messages in channels, the sponsored top bar in bot chats, and the video
ads in the media viewer.

Sponsored results in chat search come from a different path,
`api/api_peer_search.cpp`, and are skipped there.

### Exact "last seen" times

If you hide your own last seen without Premium, Telegram hides other people's
from you. On desktop that hiding is done locally: `ApiWrap::updatePrivacyLastSeens()`
in `apiwrap.cpp` walks every loaded user and rounds a real `online_till` down to
"recently", "within week" or "within month". The precise value is already in the
client, so the unlock is simply not discarding it.

Two things follow from *how* it is hidden, and both have caught me out:

**It does nothing unless you have hidden your own last seen.** The whole feature
is the removal of a reciprocity penalty. With your own setting on "Everybody"
there is no penalty to remove: people who share theirs already show exact times,
and people who hide theirs are hidden on the server, where the fork cannot
reach. If nothing looks different after switching this on, check
Settings > Privacy > Last Seen & Online first.

**Values coarsened before the switch was on need a refetch.** The precise time is
overwritten in memory and in local storage, so flipping the switch cannot bring
it back on its own. Upstream handles this for real Premium in
`Info::Profile::TopBar::setupShowLastSeen()`, which calls `updateFullForced()`
when it sees a status hidden by us; that gate reads local premium too, so opening
someone's profile re-resolves them. The `contacts.GetStatuses` request that
follows the privacy update refreshes contacts, and the rest re-resolve as they
load.

#### Measured, and it is worse than the description above

A temporary probe over the account's own contacts, 2026-08-24, with the unlock
on and the user's own last seen set to "Nobody":

    1913 contacts (0 unloaded), exact 1, localvalue 0,
    hidden by me 536, hidden by server 1376.

`localvalue` counts `LastseenStatus::isLocalOnlineValue()` - a status marked
unavailable that nevertheless still carries a real timestamp. That is the exact
shape of "the precise value is already in the client", and **it is zero**. Not
one contact of 1913 is holding a real time behind a bucket.

So the first paragraph of this section overstates the case. The server is not
sending `was_online` and leaving the client to round it off; for these contacts
it is sending `userStatusRecently` and friends outright, with the `by_me` flag
set - which is what the 536 are. `by_me` means *the server* withheld it because
you hide your own, not that this client coarsened anything. There is nothing
local to reveal, and no client-side change can produce one.

The 1376 are people who hide their last seen from everybody, which was never
recoverable. The single exact one is the whole of what the unlock yields here.

Which leaves one thing genuinely unsettled: whether some of the 536 are stale
values this client coarsened during an earlier run - `updatePrivacyLastSeens()`
also passes `by_me = true` when it rounds, so a locally-rounded status and a
server bucket are indistinguishable once written. Settling it needs a forced
`contacts.GetStatuses` and a second probe. **Do not settle it by toggling
`enabled_p` off and on**: with the unlock off the coarsening loop runs and
persists, destroying any real values still held, which is the trap this section
already warns about one paragraph up.

### Real-time chat translation

Three separate gates, which is why the feature looks server-side at first glance:

- the "Translate chats" master switch in Settings > Language
  (`boxes/language_box.cpp`), which is itself Premium-locked, so without this one
  the other two are unreachable;
- the translate bar at the top of a chat
  (`history/view/history_view_translate_tracker.cpp`);
- the "Translate" item in the chat context menu (`window/window_peer_menu.cpp`).

What makes this credible rather than hopeful: the `messages.translateText` call
underneath is already used with no Premium check at all when you translate a
single message from the message context menu. The API is open; only the
whole-chat UI was fenced off.

### Six accounts instead of three

`Main::Domain::maxAccounts()` in `main/main_domain.cpp` normally returns
`min(premiumAccounts + 3, 6)`. It now returns 6 outright.

Multi-account is purely a client-side concept: each account is its own MTProto
authorization, so the server cannot tell that several of them share an app. The
storage layer already accepts six (`storage/storage_domain.cpp`), so accounts
four through six persist across restarts with no other change.

Do not raise this above six. `Domain::add()` carries
`Expects(_accounts.size() < kPremiumMaxAccounts)`, so a seventh account would
abort the app rather than be refused.


## What is not unlocked, and why

All of these are enforced by the server, so unlocking the client gate would only
replace a clear explanation with a failed request:

large uploads; faster downloads (the server answers `FLOOD_PREMIUM_WAIT`, see
`mtproto/mtp_instance.cpp`); voice-to-text transcription; custom emoji; emoji
status; the profile badge and profile colors; animated profile pictures; message
effects; todo lists; tags in Saved Messages; more than one reaction per message;
stories stealth mode; the Premium privacy settings (last seen, read time, voice
messages, restricting non-contacts); per-chat wallpapers; gifts; AI compose; and
every business feature.

Similar channels deserve a specific mention because the client gate looks
promising: the server truncates the recommendation list before sending it, and
the blurred "+N more" rows in the client are placeholders for chats that were
never delivered. There is nothing local to reveal.

Two candidates were left undecided rather than guessed at:

- **Sending Premium stickers** is blocked client-side in
  `window/section_widget.cpp`, but whether the server also refuses is unknown.
  It is a one-line change to find out, and the worst case is an error on send.
- **Folder tags in the chat list** are rendered entirely client-side, but turning
  them on goes through `messages.toggleDialogFilterTags` and the per-folder
  colors ride along in `messages.updateDialogFilter`. Making that work locally
  means a client-side override for both, and the colors may not survive a sync.


## Turning it off

Flip the toggle in Settings > Advanced, or set `enabled_p = false` under
`[premium]` in the settings file and restart. (`enabled` without the suffix is
the retired spelling; the parser warns and ignores it, so a file using it is
still unlocked.)

Everything reverts, with two rough edges worth knowing:

- Sponsored messages already fetched for an open chat persist until you reopen
  it. Search ads come back immediately.
- Accounts beyond the third stay logged in and fully usable, but switching to
  one shows Telegram's "accounts limit" box each time. That box only offers the
  subscription page and Cancel; it never logs anything out.


## On Android

The same settings key governs the same idea, so one `settings.toml` still means
one thing in both clients: `PurpleGate.localPremium()` is `Purple::LocalPremium()`'s
opposite number, reading `[premium] enabled_p` through the bridge.

One difference in the accessor is worth stating, because it inverts this port's
usual rule. Every other gate in `PurpleGate` answers "stock behaviour" until the
settings have been read - that is what makes the fork cost nothing when it is
not running. This one answers **true** before anything is loaded and after a
load that fails, because the key's own default is on, and a gate that defaulted
the other way would withhold on a fresh install exactly the thing the settings
it has not read yet would grant.

There is no toggle in the Android UI. The file is the switch here, and the
desktop already has the checkbox that writes it.

### What ported

**No sponsored messages**, and the reasoning survives intact even though the
code does not look the same. Upstream Android skips them only when
`getUserConfig().isPremium() && getMessagesController().isSponsoredDisabled()` -
that second half reads `userFull.sponsored_enabled`, so the ability to turn ads
off is itself a *server-side* Premium setting rather than a client decision. But
`messages.getSponsoredMessages` carries no Premium check of any kind, so the
client is still the only thing that can decline to ask, which is the criterion.
Two sites: `ChatActivity.addSponsoredMessages()`, which both a channel's
sponsored posts and a bot chat's sponsored top bar hang off, and
`VideoAds.load()` for the media viewer.

The desktop's fourth sponsored surface has no counterpart. Search ads reach the
desktop through `api/api_peer_search.cpp`; on Android nothing ever sends
`contacts.getSponsoredPeers`. The rendering exists - `ProfileSearchCell` can
draw a `TL_sponsoredPeer` - but no code path fetches one, so there is nothing to
skip.

**Real-time chat translation**, with one gate fewer than the desktop needed. The
desktop had to unlock the "Translate chats" master switch as well, because that
switch is itself Premium-locked in `language_box.cpp`. Android's equivalent, the
"Show Translate Chat Button" row, is not gated at all, so
`TranslateController.isFeatureAvailable()` and its per-dialog overload are the
whole fence.

Android also has a gate the desktop document never listed:
`isLanguageRestricted()` gives Premium accounts their "Do Not Translate" list
and everyone else a hardcoded "your own language only". That is a local
preference being ignored, so it is unlocked too.

### What is new here

**Locked folders.** `MessagesController.lockFiltersInternal()` marks every
folder past the non-Premium limit `locked`, which greys its tab and puts an
upsell behind it. The folders are already in hand - the server sent them, they
are in the local database, and the preset machinery in A3 and A4 resolves them
normally - so this is the client refusing to use what it holds, which is exactly
the criterion the desktop's four were picked on. It matters more here than it
would upstream: a `folders` preset naming a locked folder is otherwise asking
for a tab the app will not draw.

This does **not** touch the folder *count* limit, which the server enforces on
creation. The expected shape is that folders already in the account become
usable and new ones past the limit are still refused, which is the honest
outcome rather than a workaround.

The locks settle on restart rather than on a hot reload of `settings.toml`.
Everything else here follows the file immediately, but the `locked` flags are
computed in a pass that only runs when the filter list changes; re-running it
from the reload path would be a second mechanism for a switch that is flipped
about once.

### What did not port, and why

**Six accounts: there is no gate to remove.** All three places that decide
whether "Add Account" is offered - `UserInfoActivity`, `MainTabsActivity`,
`LogoutActivity` - test `getActivatedAccountsCount() < UserConfig.MAX_ACCOUNT_COUNT`,
and that constant is 4 with no Premium check anywhere near it. Everyone already
gets four accounts. `UserConfig.getMaxAccountCount()`, which does read Premium
and returns 3 or 5, feeds nothing but a hint string - and note that its Premium
answer of 5 exceeds the array bound of 4 that every per-account array is sized
by, which is upstream's inconsistency and a good reason not to build on it.

Going past four would mean resizing every one of those arrays. That is *adding
capacity*, not declining to withhold something already held, and it fails this
document's own criterion. It is a separate decision if it is ever wanted.

**Exact "last seen": measured, and not worth porting.** The measurement above
settles this. On the desktop, with the unlock on, exactly one contact in 1913
was holding a real timestamp behind a bucket; the server simply does not send
`was_online` for the rest. Android would need its own coarsening loop found and
its own unlock written, and the *off* path of such a loop destroys real values
- the trap this document already warns about. Writing a destructive loop to
recover one contact's timestamp is a bad trade, so it is deliberately skipped
rather than pending.

## Verifying

- Open a large public channel that reliably shows sponsored posts and scroll to
  the bottom. Search for a term that normally yields a sponsored chat result.
- With your own last seen hidden in Privacy settings, check a non-contact who has
  not hidden theirs; they should show an exact time.
- Settings > Language, turn on "Translate chats" - no upsell box. Open a chat in
  another language and translate something, which confirms the server does not
  refuse `messages.translateText` for a non-Premium account.
- Add a fourth account.

Then confirm nothing leaked into the Premium-adjacent paths: your own profile
carries no Premium badge, and Settings still offers to subscribe.
