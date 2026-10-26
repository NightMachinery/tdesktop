# Copy as Markdown

Message context menus offer Copy as Markdown beside Copy, including captions,
albums and selected text/messages. Ordinary content uses the existing rich-text
HTML exporter and Qt's GitHub-flavoured Markdown writer. The import uses
whitespace-preserving CSS for literal spaces, tabs and blank lines. The formatter
checks the generated Markdown's plain-text roundtrip. If Qt would lose literal
text, it retains the genuine rich HTML fragment in a preformatted block with an
inherited font and literal newline separators. This avoids Qt's measured loss of
leading spaces and repeated blank lines. The genuine Qt/file qualification passed
17 file checks and 36 ordinary/article checks on 20 October.

Whole article messages use the complete RichPage tree through
`Iv::Instance::resolveRichMessage`. A partial preview is never substituted for a
full response. `purple_article_markdown.cpp` reconstructs Markdown from headings,
paragraphs, all list/quote/detail children, tables, code, math, bylines, footers,
related articles and media captions. Complex ordered numbering, table spans and
closed details retain escaped HTML structure. Inline formats without a Markdown
representation retain escaped rich HTML. Unsupported blocks or invalid contexts
fail the whole action, rather than silently dropping content.

Media remain descriptive placeholders with supplied external URLs or an actual
source-message permalink where available. Copying does not download media or
export files, inject raw embed HTML, invent media URLs, or expose local cache
paths. A received article supplies structure, not the author's original Markdown
syntax. This reconstruction is not labelled original source.

Mixed whole-message selections and albums preserve production display order.
Every article must resolve and serialize before one final clipboard write. The
operation rechecks its weak list/session/window/controller ownership, account,
selection/group membership, full message identity and article generation. Message
deletion, context replacement, account changes, stale results and errors prevent
the write. A later click invalidates only the previous copy operation, while the
existing article resolver retains its own request coalescing. Ordinary selections
and albums without articles retain their original clipboard-text fast paths.
Selected text fragments remain the separate ordinary formatter action.

Purple permits local copying and selection of protected content. Its copy and
media-copy checks return unrestricted, including final action checks. The frozen
session selection guard remains. Forwarding flags, server permissions and media
availability are unchanged. No message is sent or changed by Copy.

Upstream hooks are limited to Purple menu calls and early policy checks in
history_view_context_menu.cpp and history_view_list_widget.cpp. The ListWidget
header adds a read-only selection-order accessor implemented in Purple source.
The structural writer, action ownership, strings and build registrations remain
in Purple files. See [peek_copy_regressions.md](peek_copy_regressions.md) for the
real Qt results and remaining native qualification cases. The eight affected
production units and both host programs passed. Countdown/context/clipboard native
behavior, the complete app and release acceptance remain separate gates.

Every Markdown menu action advances one shared production copy generation,
including ordinary message/group/selection/fragment paths and other windows.
An older pending article result cannot regain ownership after a newer copy.
Article batches bind the displayed translation identity, language target, used
state and immutable page pointer. A complete active translation is exported
without resolving the original language; an unavailable or partial active
translation refuses the whole copy. Current translation/target changes invalidate
pending work while shared resolver requests remain available to other consumers.

Anchors are separated from Markdown block markers; item anchors follow list/task
markers. Ordered counters use a widened checked range. Starts or sequences outside
the zero-through-nine-digit Markdown marker domain use HTML ordered lists.
Native alphabetic/Roman aliases normalize to HTML numbering types while raw item
markers, numeric overrides, reversal and start values remain represented.
Table content, spans, headers and alignment are retained. Native table border and
alternating-row presentation flags are a documented low-priority limitation.
These repairs still require their affected-TU/expanded-Qt qualification and the
same reviewer's focused source closure; no native behavior is established here.

The preceding expanded qualification was PARTIAL: four changed production units compile,
and the genuine program reports 66 checks with three parsed-structure failures.
The final anchor candidate retains identities, but Qt merges following code/quote
fragments into the preceding HTML list. Anchor structure remains unresolved.
A proposed whole-article HTML alternative is retained privately, unqualified and
not promoted. The code/quote assertions and original 36 checks remain strict.

Balanced anchor-requiring sibling runs now use one escaped HTML island with
explicit paragraph children. The production article unit compiled; the unchanged
66-check genuine Qt fixture passed 65 checks. Heading/list/quote structure and
all five anchor identities pass, and code is imported as a separate preformatted
block outside the list. The remaining fence-property assertion records Markdown
syntax provenance and still fails. It has not been changed or waived. Independent
source closure and the code-oracle decision remain separate pending gates.

The strict semantic successor passed 88 genuine Qt checks without production
changes or weakening the other 65 existing checks. It combines the balanced
source tree with actual imported structure, exact code content/range and anchors,
including whitespace and corruption controls. Source language metadata is exact;
reader-specific language-property retention remains a presentation limitation.
The old fence-origin failure remains retained as historical evidence.

Optional genuine source is captured from now onward. Ordinary compose captures
only unstyled raw input whose Markdown parsing matches the outgoing message.
A bounded, expiring token follows that exact request and random/local identity.
Only a complete ordinary acknowledgement and its complete initial retained
baseline can attach source. Partial short-send snapshots keep their existing
semantics and use the structural fallback. Received ordinary text and rich-editor
blocks never establish the author's Markdown syntax.

Already acquired Markdown documents use authenticated owned archive bytes, at
most 1 MiB. Filename/MIME, exact document identity and length must agree; captions
or additional displayed content prevent substitution. Invalid UTF-8 remains
exact bytes in storage and refuses textual source decoding. Source stays inside
the encrypted version, under existing purge/logout ownership. Attachment checks
the captured journal and generations through the real SQLite precommit boundary.

Whole current-message and album actions now try the retained source through the
shared core after matching the displayed original body, revision metadata and
Markdown document identity. Text fragments and selections keep their displayed
content. Empty genuine source remains distinguishable from absent source.
Document context menus also expose the action for Markdown files; unretained
bytes never cause a download.

A copy captures its journal/store/login lifetime and publication stamp. Final
clipboard publication uses a nonblocking gate shared with archive commits. A
committed write or purge changes the stamp; store closure revokes the gate.
The UI never waits for archive SQL. Message edits, deletion, migration, album
membership, translation, account/controller changes and a newer copy invalidate
pending work. Every source lookup releases its read snapshot on the worker
before posting an immutable result.

Partial original article Copy captures its own bounded raw preview at the start.
Responses must preserve every observed TL block and media object, match the
opening peer/body/date/edit/group metadata, and preserve parsed display structure.
Missing preview evidence refuses only that Copy. The shared resolver continues
viewing, editing and export normally after preview eviction. A Copy that already
captured its opening snapshot can still validate a coherent full response after
the registry evicts that preview. A complete displayed translation
uses its own page; a partial translation refuses. Successful copies queue full
observed rich structure as exact-version Hydration after clipboard publication;
core preservation rules can refuse it without inventing a new author edit.

Markdown document recognition matches Android: `.md`, `.markdown`, `.mkd`,
`.mdown`, `.mkdn` and `.mdwn` suffixes ignore case. MIME recognition takes the
first semicolon-delimited value, trims surrounding whitespace and ignores case
for `text/markdown`, `text/x-markdown` and `text/x-web-markdown`. The exact stored
document identity and verified bytes still determine source eligibility.

A present empty genuine source writes an explicit empty plain-text MIME value.
Absent output remains a no-op, and stale results never reach delivery. This
Purple delivery boundary avoids the ordinary UI helper's empty-text no-op.

Cached full articles pass the same Copy admission as a fresh response. Copy owns
the raw opening and completed message values, checks their revision and native
prefix, and binds the decoded completed page to the selected cached page. Missing
evidence refuses Copy. The bounded observation registry includes retained full
responses; eviction cannot revoke snapshots already owned by an active Copy.
