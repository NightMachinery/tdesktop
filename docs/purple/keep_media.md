# Keep Media scope

Keep Media controls how long locally cached media is retained. It does not
control automatic downloads or files explicitly saved outside the app cache.

Android is the current focus for per-chat retention. Its per-chat chat-menu
editor is committed in Android source `1593b8d1`. The signed `bce320af`
candidate showed the effective **1 week (default)** menu label, changed it to
**Forever (this chat)** after choosing Never, and restored the default label
after Delete Exception. Cache expiry over time has not been runtime verified.
The final signed APK from source `3559ef67` installed with the test account
intact and still showed the Keep Media chat-menu action.

Android's cleanup applies a chat exception only to files with a recorded chat
owner. Ordinary new message downloads typically record one, including the
pinned-music download path. Older files or files shared across chats may lack
the relevant attribution and follow general retention. The cache size limit
can also remove finite-retention files before their time limit. The current UI
check does not establish expiry behavior.

Desktop continues to use the global cache retention limit in Local Storage
settings. It has no per-chat Keep Media setting or policy enforcement. Desktop
per-chat retention is deferred; the unfinished cache policy, session override,
and ownership groundwork was removed from this fork. Any future desktop design
must account for cache keys shared between chats and for entries whose chat
ownership is unknown.
