# The fork's own defaults

Work Mode is a feature you turn on. This file is about the other thing a fork
does: the handful of values it decides differently from upstream for everyone,
before anybody configures anything.

Each of these is a **default and nothing more**. It is the answer given when the
user has never chosen. The moment they choose, the choice is written to a
preference and wins from then on, exactly as it does upstream - so none of this
takes a setting away, and none of it is worth a setting of its own.

On Android they live together in
`TMessagesProj/src/main/java/org/telegram/messenger/purple/PurpleDefaults.java`,
as compile-time constants, so a default that several places have to agree about
cannot drift between them and so that the whole set is one file to read. The
desktop has no equivalent file - its one changed default is a member
initialiser, `MainSession::Settings::_archiveInMainMenu = true`, because
nothing else reads it.

## The Archive row is not at the top of the chat list

The reasoning, and the desktop half, are in
[work_mode.md](work_mode.md#the-archive-row-is-hidden-by-default). What is
worth adding here is that the two clients do not reach the same set of
installs, and it is the *Android* one that behaves the way you would want.

The desktop's value lives in tdesktop's per-account settings blob, which is
serialised whether or not anyone ever chose anything, so an existing `tdata`
already has a value and keeps it: the default only ever reaches a fresh
account. Android writes `archiveHidden` only when the row is actually swiped,
so an install that has never touched it has no key at all - and the default
therefore reaches it too, on upgrade, without overriding anyone who did swipe.

Same intent, and by accident the weaker storage gives the better behaviour.

The observable, if this ever needs re-testing: the preference itself. With no
key stored the row is not drawn at all, and archiving the very first chat
writes `archiveHidden = false` from the `added == 2` branch of
`DialogsActivity`'s swipe handler - which upstream guards on `archiveHidden`
being true, so that key could not appear at all under upstream's default. Its
mere existence is the proof.

## The background connection is on

There is no FCM push in this fork and no way to get it back - Telegram's
servers deliver only through Telegram's own Firebase project, and this app's
registration is refused by package name. The background connection is
therefore the **only** way a notification ever arrives here.

Upstream defaults it off, because upstream has push. Inherited unchanged, that
default means a fresh install is silently unable to notify: everything looks
signed in and working, and nothing ever arrives until you find the switch. That
is the one default in this list that fixes a defect rather than a preference.

### Why it is not the preference's default

The obvious change is the wrong one. `ConnectionsManager.isPushConnectionEnabled()`
falls back to a preference called `backgroundConnection`, and that preference is
not ours: `background_connection` arrives in Telegram's server-pushed app config
and is written straight into it by `MessagesController.applyAppConfig`. A
default placed there survives until the first config fetch and then quietly
becomes whatever the server says.

So the fork's answer has to be its own constant, consulted where the preference
is *absent*, rather than a different default for a key somebody else owns.

The same trap caught a *test* later the same day: `dialogFiltersLimitDefault`
was written by hand to make a folder limit easy to exceed, and the app config
put it back within seconds. Worth stating as a rule rather than an anecdote - **a
preference the server writes is neither a place to put a default nor a fixture
to test with.** `git grep applyAppConfig` names the ones it owns.

### Three places have to agree

This is the part worth writing down, because the seam looks like one line and
is three. `isPushConnectionEnabled()` decides what actually happens - it is read
once and handed to the native layer at init. But `NotificationsSettingsActivity`
works the same answer out twice more for itself: once to draw the checkbox, and
once to read the current state back before writing the toggle.

Change only the first and the result is not "half done", it is incoherent: the
connection runs while the switch renders off, and tapping the switch to turn it
on reads "off", writes "on", and leaves it on. The switch looks stuck. All
three read `PurpleDefaults.PUSH_CONNECTION` for that reason.

The user's own choice still wins everywhere, because all three look for the
`pushConnection` key first and only fall back when it is missing.

The observable is the network log, which names this connection directly. With
no key stored it carries "send ping to push connection" and connections of
`type 8`; with `pushConnection = false` it carries neither. The switch is the
other half: it renders checked by default while **Keep-Alive Service** beside
it - same widget, default untouched - renders unchecked, and one tap from the
default state turns the connection off rather than appearing to do nothing.

Keep-Alive Service is deliberately not changed. It is a permanent notification,
and whether that is worth paying depends on the phone; the background
connection costs nothing visible.

### One limit, inherited

With more than one account signed in, the switch and the connection read
different stores: `isPushConnectionEnabled()` reads account 0's notification
preferences, and the settings screen reads and writes the current account's. So
on a second account the switch remembers its own position while the connection
keeps following account 0.

That is upstream's, unchanged here, and it is left alone deliberately - pointing
the screen at the connection's store would make a second account's switch
appear not to respond to its own writes, which is worse. It matters less than it
did before this change, since the default the two now fall back to is the same
one.

## The Keep-Alive Service is on

The switch next to the background connection, and on for the same reason: with
no push, the process has to be alive for anything to arrive, and this is the
one switch that asks the OS to bring it back after killing it. Upstream's
fallback is `keepAliveService`, a flag the server sets for the few vendors it
knows kill background processes and leaves false for everyone else; it is still
honoured, but it can no longer turn the default off.

What it actually costs is less than its name suggests. `NotificationsService`
never calls `startForeground()`: it is a sticky service, plus a broadcast from
`onDestroy()` that starts it again. So there is no permanent notification - the
earlier note here that there was one was wrong, and was the reason this default
was left off at first. What it can and cannot do is the OS's decision - a
vendor battery manager that kills the app will kill the relaunch too, and the
setting is not a substitute for excluding the app from that.

Same seam as the connection, three places: `ApplicationLoader.startPushService()`
decides, and `NotificationsSettingsActivity` draws the switch and reads it back
before toggling. All three read `PurpleDefaults.KEEP_ALIVE_SERVICE` when the
`pushService` key is absent, and the user's own choice wins once it exists.

## The app calls itself by its own name

`strings.xml` in this fork has said `AppName = "Purple Telegram"` for a long
time, and the launcher has always shown it - `android:label` is resolved from
resources when the package is installed. The running app disagreed with its own
launcher entry and said **Telegram** in the chat list header and in every
notification.

The cause is not that the rename was forgotten. It is that
`LocaleController.getStringInternal()` reads the downloaded language pack
*before* the app's resources:

    String value = BuildVars.USE_CLOUD_STRINGS ? localeValues.get(key) : null;
    if (value == null) { ... getString(res) ... }

Telegram's English pack carries `AppName = "Telegram"`, so the fork's own string
was shadowed at runtime by a value fetched from Telegram's servers. Any fork
that renames itself only in `strings.xml` has this bug and will not see it in
the launcher, which is the obvious place to look.

The fix is to exempt the two keys that name the app - `AppName` and
`AppNameBeta` - from the language-pack lookup. That is not a general distrust of
cloud strings; it is that these two keys are statements about *this build*,
which a server has no standing to make. Everything else still comes from the
pack, including every translation of every other string.

It is deliberately not a setting. A switch here would mean choosing to be called
Telegram in the app while the launcher entry underneath says Purple Telegram,
which is the inconsistency this removes rather than a preference worth offering.

### The one surface the fix cannot reach: the header is a picture

Driving this on a real screen turned up something the earlier `uiautomator`
check could not see. The chat list header still *renders* the word Telegram,
and the fix is not at fault - the header is not text:

```java
SpannableStringBuilder ssb = new SpannableStringBuilder(getString(R.string.AppName));
ssb.setSpan(new ImageSpan(logoDrawable), 0, ssb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
actionBar.setTitle(ssb, statusDrawable);
```

`AppName` supplies the span's *length* and nothing else; every pixel drawn comes
from `R.drawable.telegram_logo_2`, Telegram's wordmark. So the accessibility
text really does read `Purple Telegram` - which is exactly what the dump
reported, correctly - while what a person sees is the stock logo. A string fix
can never change that, and no amount of re-checking the string will reveal it.

The lesson generalises past this fork: **a `uiautomator` dump reports the text a
view declares, not the pixels it paints.** Where the two can disagree - an
`ImageSpan`, a custom-drawn cell, an icon carrying a `contentDescription` - only
a screenshot settles it.

Left as it is for now, deliberately. Changing it means either dropping the span,
which costs the logo the header was designed around, or drawing a Purple
wordmark to replace it - a piece of artwork, not a bug fix, and a decision about
how far the fork wants to look like itself. What is *not* acceptable is the
current documentation implying the header was fixed, so it says this instead.

### The launcher icon is the fork's own

The name change does nothing for the thing you actually see on a home screen, so
the icon is its own piece - and, since 2026-09-08, it is on by default. It
started off, on the grounds that looking like stock is sometimes the point;
the stock icon is still there for that, as the first tile of the App Icon
picker, and the choice is remembered the way every icon choice is.

How the default is expressed is worth a line, because there is no preference
behind it. An alias nobody has chosen sits in `COMPONENT_ENABLED_STATE_DEFAULT`,
which resolves to whatever `android:enabled` says in the manifest - so the
manifest enables `PurpleIcon` and disables `DefaultIcon`, and
`LauncherIconController.isEnabled()` reads the unset state as Purple rather
than as Default. An install that never opened the picker therefore switches to
the purple icon on upgrade, and one that did keeps what it picked, because a
picked alias is in an explicit state the manifest no longer speaks for. The
launcher itself decides what happens to the tile on the home screen when the
entry underneath it changes; most drop the old one and add the new one at the
end.

It needed no setting. Upstream already ships alternative launcher icons: an
`activity-alias` per icon, all but one disabled, and
`LauncherIconController.setIcon()` enabling exactly one and disabling the rest.
A Purple entry rides that, appears in Telegram's own **App Icon** picker where
somebody would look for it, and inherits the mutual exclusion for free. Its
state is `isEnabled(PURPLE)` and nothing else, so there is no preference that
can disagree with what the launcher is showing - which is the failure mode a
separate switch would have introduced the moment anyone picked a different icon
from the same list.

The artwork is deliberately unoriginal: the stock foreground over a purple
gradient, built exactly the way the Aqua icon is built, and cut to the stock
silhouette. It should read as Telegram at a glance and never be confused with
the official app sitting next to it. The pre-adaptive bitmaps for API 21-25 are
generated from those same pieces rather than drawn, so they cannot drift from
the adaptive version.

It is not Premium-gated, unlike three of upstream's. It is ours to give.

Verified on a screen: the **Purple** tile is the second in the picker, straight
after Default, drawn from the gradient behind the stock foreground. Tapping it
leaves `resolve-activity` answering
`org.purple.telegram/org.telegram.messenger.PurpleIcon`, and a query for every
enabled launcher entry in the package returns exactly one - which is the
guarantee that stops two Purple Telegram entries appearing on a home screen at
once. The padlocked Premium tiles in the same row stay locked, which is correct:
Local Premium covers four named features and app icons are not among them.

## Also on by default, elsewhere

**Local Premium** is on by default too, and is its own document:
[premium.md](premium.md). It is not listed here because it is a feature rather
than a value - the client declining to withhold things it already holds, without
claiming to the server that the session is Premium.

## Still to come

Nothing is on the list. The Purple identity - name and launcher icon - that
used to sit here is above.
