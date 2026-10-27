# Marked WebP sticker paste

An emoji chooser can opt into lossless WebP paste with this raw macOS pasteboard
marker type: `org.nightmachinery.emoji-export`. Its UTF-8 JSON is:

```json
{"version":1,"format":"webp","intent":"sticker","preserveAlpha":true}
```

All four keys and these values are required. Unknown keys, duplicate keys,
malformed UTF-8/JSON and other versions or values take the ordinary unmarked
paste path. No account identity or emoji ID belongs in the marker.

The chooser writes `org.webmproject.webp` with the original WebP bytes,
`public.file-url` with a local immutable WebP file URL, and `public.png` as an
ordinary image fallback. The reader prefers the raw WebP. Only when that type
is absent does it read the single local regular file once, under the sticker
byte cap. The opened handle must be a regular file; POSIX opens refuse symlinks
and use nonblocking mode before the descriptor check. A present invalid raw representation refuses the marked route.
This file-only fallback is an approved bounded GUI-thread read: it is initiated
by the user once per paste, capped at sticker size, and restricted to a local
regular file with a no-follow, nonblocking open. FIFOs and symlinks are refused.
Making this capture asynchronous would change its lifetime and revision rules.
The analogous upstream GUI-thread drop validation is
`editor/photo_editor_content.cpp:289`, calling
`Storage::ValidatePhotoEditorMediaDragData`; that function synchronously calls
`QImageReader(file).canRead()` in `storage/storage_media_prepare.cpp:93`.

The marker and representations must be in one pasteboard item; a change during
reading refuses the marked route. Tests use a private named pasteboard and do
not inspect or change the general clipboard.

Validation requires matching RIFF/WEBP magic and RIFF length, successful WebP
decoding, the existing `GoodStickerDimensions` rule, bytes strictly below the
existing `Storage::kMaxStickerBytesSize`, and a decoded alpha channel. The
producer's 512 px longest-side images meet these rules. A failed check returns
to the unchanged unmarked paste behavior.

Valid marked content is captured in memory before the send confirmation opens.
The prepared file keeps `image/webp`, its decoded image and the original bytes.
It is prepared as a standalone, non-groupable sticker rather than a photo.
Its original-byte flag overrides photo compression and applies the actual
sticker restrictions. The sticker preview remains available with either
checkbox state. Photo-editor and drawing actions are disabled: marked content
is immutable, so confirmation cannot show an accepted edit that sending would
discard. Captions and rename controls follow the sticker policy.

Each marked item forms its own send group. Slow-mode admission refuses any
second attachment alongside a marked sticker, including additions to an existing
confirmation. The final bundle check therefore counts its individual sends. The existing
WebP file loader creates the usual sticker/document attributes and thumbnails;
the upload payload never goes through `Images::Opaque` or JPEG encoding.
“Send as a document” works checked or unchecked. Unmarked files in the same
confirmation keep their own existing routing. Normal, scheduled, secondary
chat and confirmation-box pastes use the same preparation hook. Media
replacement also uses the per-file upload override.

Qt producers may use `application/x-purple-emoji-export` with the same marker
and `image/webp` bytes or a single local file URL. A marker-only admission check
allows this Qt path without a PNG fallback; file content is read only on Insert.

## Cocoa and Qt mapping

The vendored Qt source routes `QCocoaClipboard::mimeData` through
`QMacPasteboard::formats` and `retrieveData` in `qmacclipboard.mm`, then
`QMacMimeRegistry::flavorToMime` and the registered UTI converters.
`QMacMimeAny` in `qutimimeconverter.mm` only maps
`com.trolltech.anymime.<MIME with / encoded as -->`; it does not expose an
arbitrary raw `org.nightmachinery.emoji-export` type as a MIME format.
`public.file-url` has its own `text/uri-list` converter. Purple therefore reads
the raw Cocoa marker and WebP directly in its small macOS hook. That fallback
is used only for the actual current Qt clipboard object, avoiding an unrelated
clipboard marker being applied to drag data.

The standalone fixture writes the exact four producer types with
`NSPasteboard setData:forType:` and calls the production native reader on that
private pasteboard. It also covers the file-only fallback, invalid native
markers and representations, Qt-produced data, validation and routing choices.
No Telegram client or message sending is required.

## Upstream hooks

The three chat paste entries and `SendFilesBox::addFiles` have an early call
into Purple before normal URL/image selection. `Ui::PreparedFile` gains a
false-default original-byte flag; only the validated Purple preparer sets it.
`ApiWrap` has per-file upload hooks for sends and media replacement.
Attachment grouping and restriction checks call Purple for the marked file's
actual route. The image-modification boundary preserves its original content.
All parsing, byte capture and routing decisions live in `purple_webp_paste*`.
