# Focused Peek and Markdown cases

Both genuine production-function programs were compiled and executed on 20 October
with the configured patched Qt and production libraries. The file program passed
17 checks; the ordinary/article program passed 36 checks. Eight affected production
translation units also compiled. No fake Qt, app API, file loader, article model,
parser or formatter was used. The initial two whitespace failures and subsequent
repairs were retained before the final passing run.

`peek_file_cases.cpp` links the real `purple_config_file.cpp` and Qt Core. It uses
only its own QTemporaryDir and checks readable-empty, missing and read-error
states, exact current input, stale writes after an external save, preservation of
unrelated external changes, removal and a successful guarded save.

`copy_article_cases.cpp` calls the actual ordinary formatter and structural
article writer with the real RichPage model. It needs Qt Gui with an explicitly
selected headless platform, the real lib_ui text/HTML implementation, and the
actual IV inline-object and link-codec implementations. Qualification must link
those real project implementations, not handwritten replacement functions. Use
the configured project compiler/Qt bindings. This source does not introduce or
admit a new build runner. The program checks exact plain-text roundtrip through
Qt Markdown for repeated/leading spaces, tabs, blank lines, punctuation and mixed
entities, plus structural headings, child blocks, numbering, spans, closed details,
code fences, math, captions, safe placeholders and partial/unsupported refusal.

Separate native cases remain necessary for countdown timing, synchronous reload
reentrancy, context lifetime and atomic multi-message clipboard behavior. See
`docs/purple/peek_copy_regressions.md`. Source checks and these prepared programs
do not establish those runtime observations.

The next delta program retains the original 36 checks and adds real production
copy-generation/translated-page selection, parsed anchored block structure and
identity, widened ordered-list boundary cases and all native numbering aliases.
The old overflowing implementation is not executed by those new boundary cases.
Native pending-copy/translation and final synchronous Peek-state callbacks remain
separate cases, not conclusions from these pure production helpers.

Preceding measured six-repair delta: four changed production units compiled. The expanded
program ran with 66 strict checks, three failing (anchored list/code/quote parsed
structure). The original 36 checks remain unchanged. Empty id/name anchors were
dropped by fragmented Markdown import; complete HTML fragments retained anchor
identity but caused later block merging. Primary failures and diagnostics remain
retained. A rejected assertion-changing proposal was not executed or retained in
canonical source. Further repair/qualification is required before a passing claim.

The balanced-island production-only repair was compiled and linked against this
UNCHANGED program. Result: 66 checks, one failure (anchored code fence property).
Heading/list/quote and all anchor checks now pass. The actual Qt document keeps
the code in a separate preformatted block outside the list. The fence assertion
remains present and failing; no alternative predicate has been inserted.

A strict semantic successor replaces only the proven fence-origin criterion.
Its helper combines Qt XML source-tree validation with the actual Qt Markdown
importer, exact code text/language/range, list/quote separation and node-specific
anchors. The other 65 checks remain byte-identical. Expanded positive and negative
cases are prepared, not yet executed. Initial compilation diagnosed a QVariant
font-family accessor mismatch; the mechanical QStringList conversion is prepared
and still requires actual compiler/link/run qualification. The historical failed
66-check result remains valid and unchanged.

The corrected strict semantic successor now compiled, linked and executed with
88 checks and zero failures. Its accounting is 65 unchanged original checks, one
strict replacement for the proven fence-provenance bug, four added positive
cases and 18 negative controls. The original failed 66-check packet remains
unchanged. Exact leading/repeated spaces, tabs, blank and final code lines, source
language metadata, distinct adjacent lists and node-specific anchors passed.
Negative cases rejected flattened or absorbed blocks, merged/reordered lists,
altered code/language, styled prose, false quotes and absent/duplicate/moved or
decoy anchors/code tags. Imported language is checked when Qt exposes it; exact
escaped source language remains authoritative, without a claim that every reader
retains a Qt language property. No production sources or unchanged suites were
recompiled for this successor. Independent source/native gates remain separate.

Every program here now has a host gate (`cases-<name>` in `purple/host_gates.json`,
built by `purple/test_cases.py`). On 27 October the successor above ran there
again with 88 checks and zero failures.
