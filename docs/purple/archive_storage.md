# Local message archive

Open **Settings > Purple > Archive & storage** for retained deleted messages,
quota and cleanup controls. Use the message context menu for that message's
History or a chat's menu for its History and Deleted list. M-x provides
`archive-history`, `archive-deleted` and `archive-storage`. History renders
the saved text and formatting and can export a complete saved attachment.
Select a retained message in **Deleted messages** to open its History and
export any saved file, including files from expired or consumed messages.
In modal mode, use **j/k** to move, **Return** to open the focused row and
**/** to filter the loaded rows. **Escape** clears the filter and restores
row focus. **Older messages** appends to the list. The records reflect
observations delivered to this Desktop profile.

## Policy

Set `days`, `total_mib`, `timed_media_mib` and `ordinary_file_mib` under
`[archive]`. Their defaults are 90 days and 512, 256 and 16 MiB respectively.
Timed media uses its own allowance. The ordinary file limit must fit inside
the ordinary allowance. Zero timed MiB disables timed file storage.
`[archive.days_by_class]` accepts optional `direct`, `bot`, `group` and
`channel` overrides.

Timed media defaults to `on_open`. Set `timed_download` to `off` or
`on_open` under `[archive]` or a typed chat table such as
`[archive.chats."user:17"]`. The optional `[archive.timed_download_by_class]`
table uses the same four chat classes. A chat value wins over its class, then
the app-wide value. Clear an override to inherit. Customize reports the
effective value and where it comes from. The parser warns and uses `on_open`
for `on_arrival` until the two-account receipt check is qualified.
Only a chat's own account file may opt it in with `ordinary_on_arrival = true`.

### Encryption and reset

Desktop creates the archive under `content-archive-v2` in its configuration
directory. A profile identity and typed account determine its private directory.
The protected pointer authenticates the active generation and recorded retirements.
It holds a random data key wrapped with keys derived from the unlocked profile
and a persisted Telegram authorization, using a fresh salt. Separate derived keys seal records and generate lookup tokens.
SQLite receives ciphertext for message data, descriptors and blob parts.
An authenticated reference binds a saved copy to its specific version and media row.

Reset reviews the account's active generation and measures its owned directory.
Confirmation rechecks its identity and size before retiring the active
generation. A fresh archive opens when the signed-in session resumes
capture. Logout leaves any replacement unopened.
Confirmed reset can retire an unreadable database when the profile can prove
ownership of its pointer and directory. An early logout without a reported
generation also proves it can unwrap that generation against its own login.
Recovery resumes deletion for the directory identity recorded by the pointer.
Normal quit drains pending text for up to 220 ms through the event loop.

### Whole files and space limits

A whole-file completion must match the admitted account generation, message
version and source. Changing archive policy cancels pending attachment work.
History shows whether a copy is in progress when the page opens. Explicit open preserves the normal viewer and receipt
sequence before scheduling archive work. Timed arrival and unrelated cache
completion do not create a request. Each account queues up to 32 admitted media targets and saves one file at a
time. It fetches one whole file per account at a time using Telegram's
normal MTProto downloader. Larger files queue bounded part writes to a
private downloader temporary file off the UI thread. The writer gives ready
timed files priority and keeps FIFO order within each class.
The writer commits at most four 1 MiB encrypted parts per batch and drains
queued text between batches. The final transaction verifies the complete
file and publishes its attachment. Reopening removes incomplete staged files.

Timed and ordinary pressure release copies from their own allowance.
A blob referenced by both is charged once to timed media. SQLite updates both
totals alongside changes to their records. A lower limit schedules bounded
trim batches, while text can still use physical room already available.

The physical ceiling combines both allowances with 64 MiB of headroom. It
includes 16 MiB for WAL frames and 48 MiB for framing, fragmentation,
SQLite's page layout and sidecars. Both buckets charge for payload and record
layout. Up to 4 MiB of measured file framing per bucket counts toward the
shared headroom so a file can use its full default payload allowance. When
fragmented pages leave too little space for a write, reclaim from that
write's bucket. Storage shows actual physical usage alongside the allowance
charges and overhead. Diagnostics records the individual file sizes. Successful manual cleanup
refreshes Storage and reports physical bytes freed when measurable.

Confirmed **Clear edit history** and **Empty deleted messages** use SQLite
secure deletion and truncate the WAL after each write. If the selected
versions are the last references to a saved file, its parts are scrubbed in
bounded transactions. Pending scrubs survive restart. A shared file with a
remaining reference stays readable. Automatic eviction keeps the cheaper
SQLite deletion mode.

## Previous archive

Desktop v2 starts with an empty store. **Archive storage** shows the previous
archive's measured size if it can prove the existing profile, account and
login binding. **Delete previous archive** reviews that size, then deletes
only the reviewed account directory after explicit confirmation. The worker
can resume a published deletion intent after restart. A missing or changed
ownership record leaves the data in place and gives a reason in diagnostics.

A regular v2 launch, capture, reset or logout leaves the old Desktop data in
place. Android retains the shared v1 archive model and its existing writer.

Copy as Markdown in live chats uses displayed formatting and the existing
verified full rich-page response. A complete local Markdown document can
supply its text from Telegram's cache or its current downloaded file.
Archive v2 stores observable text and formatting rather than original compose
source.
