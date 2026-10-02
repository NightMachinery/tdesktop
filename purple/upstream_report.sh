#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: purple/upstream_report.sh [--near] [--worktree | REV]

Measures what the fork changes in upstream files: every path that exists at
the merge-base of REV (default HEAD) and upstream/dev, compared with REV, or
with the working tree under --worktree.

Prints lines added and removed per upstream file and their totals. The lib_ui
gitlink and any other submodule pointer, AGENTS.md and README.md, and binary
files are reported apart and stay out of the totals. Then two watch counts:
account-less Work Mode calls in upstream files (must reach 0 once the
refactor is done), and the callers of TopPeers::list(), which Purple filters
at the source.

--near adds near-hunk edits: for every Purple hunk, the upstream commits in
the year before the base that changed a base line within one line of it,
counted with git log -L. A whole-fork run takes under a minute.

Set PURPLE_UPSTREAM_REF to measure against another upstream branch.
See docs/purple/upstream_hooks.md.
EOF
}

near=0
worktree=0
target=HEAD
while [ $# -gt 0 ]; do
    case "$1" in
        --near) near=1 ;;
        --worktree) worktree=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) target="$1" ;;
    esac
    shift
done

cd "$(command git -C "$(dirname "$0")" rev-parse --show-toplevel)"

upstream="${PURPLE_UPSTREAM_REF:-upstream/dev}"
target_commit="$(command git rev-parse --verify --quiet "$target^{commit}")" || {
    echo "Not a commit: $target" >&2
    exit 2
}
base="$(command git merge-base "$target_commit" "$upstream")"
base_date="$(command git log -1 --format=%ci "$base")"
since="$(( ${base_date%%-*} - 1 ))-${base_date#*-}"

if [ "$worktree" = 1 ]; then
    range=("$base")
    grep_rev=()
    target_name="the working tree (tracked files)"
else
    range=("$base" "$target_commit")
    grep_rev=("$target_commit")
    target_name="$target $(command git rev-parse --short "$target_commit")"
fi

work="$(command mktemp -d "${TMPDIR:-/tmp}/purple-upstream-report.XXXXXX")"
trap 'command rm -rf "$work"' EXIT

command git ls-tree -r --full-tree "$base" \
    | awk -F'\t' '{ split($1, f, " "); print f[2] "\t" $2 }' > "$work/base"
command git diff --numstat --no-renames "${range[@]}" > "$work/numstat"

awk -F'\t' -v apart="AGENTS.md README.md" '
    BEGIN { n = split(apart, a, " "); for (i = 1; i <= n; i++) isApart[a[i]] = 1 }
    FILENAME == ARGV[1] { kind[$2] = $1; next }
    !($3 in kind) { next }
    kind[$3] == "commit" { print "gitlink\t" $3; next }
    $1 == "-" { print "binary\t" $3; next }
    ($3 in isApart) { print "apart\t" $3 "\t" $1 "\t" $2; next }
    { print "file\t" $3 "\t" $1 "\t" $2 }
' "$work/base" "$work/numstat" > "$work/paths"

near_for_path() {
    local path="$1" lines lo hi a b count total=0 hunks=0
    lines="$(command git cat-file -p "$base:$path" | awk 'END { print NR }')"
    : > "$work/hunks"
    while read -r a b added; do
        if [ "$b" -gt 0 ]; then
            lo=$(( a - 1 ))
            hi=$(( a + b ))
        else
            lo=$a
            hi=$(( a + 1 ))
        fi
        [ "$lo" -ge 1 ] || lo=1
        [ "$hi" -le "$lines" ] || hi=$lines
        count=0
        if [ "$lines" -ge 1 ]; then
            count="$(command git log --format=%H -s --since="$since" \
                -L "$lo,$hi:$path" "$base" | grep -c . || true)"
        fi
        printf '      base %d-%d  +%d -%d  near %d\n' \
            "$lo" "$hi" "$added" "$b" "$count" >> "$work/hunks"
        total=$(( total + count ))
        hunks=$(( hunks + 1 ))
    done < <(command git diff -U0 --no-renames "${range[@]}" -- "$path" | awk '
        /^@@ / {
            split($2, o, ","); split($3, n, ",")
            a = substr(o[1], 2); b = (2 in o) ? o[2] : 1
            d = (2 in n) ? n[2] : 1
            print a, b, d
        }')
    echo "$total $hunks"
}

print_file() {
    local path="$1" added="$2" removed="$3" result
    if [ "$near" = 1 ]; then
        result="$(near_for_path "$path")"
        printf '%6s %6s  near %3s over %2s hunks  %s\n' \
            "+$added" "-$removed" "${result% *}" "${result#* }" "$path"
        cat "$work/hunks"
        last_near=${result% *}
    else
        printf '%6s %6s  %s\n' "+$added" "-$removed" "$path"
    fi
}

echo "Base: $(command git rev-parse --short "$base") ($base_date), merge-base with $upstream"
echo "Compared with: $target_name"
[ "$near" = 1 ] && echo "Near-hunk window: since $since, one-line margin"
echo

echo "Upstream files changed:"
files=0 added_total=0 removed_total=0 near_total=0 last_near=0
while IFS=$'\t' read -r kind path added removed; do
    [ "$kind" = file ] || continue
    print_file "$path" "$added" "$removed"
    near_total=$(( near_total + last_near ))
    files=$(( files + 1 ))
    added_total=$(( added_total + added ))
    removed_total=$(( removed_total + removed ))
done < "$work/paths"
printf 'Total: %d files, +%d / -%d' "$files" "$added_total" "$removed_total"
[ "$near" = 1 ] && printf ', %d near-hunk edits' "$near_total"
echo
echo

echo "Reported apart, not in the total:"
while IFS=$'\t' read -r kind path added removed; do
    case "$kind" in
        gitlink)
            from="$(command git rev-parse --short "$base:$path")"
            if [ "$worktree" = 1 ]; then
                to="$(command git rev-parse --short "$(command git submodule status -- "$path" \
                    | awk '{ sub(/^[-+U]/, "", $1); print $1 }')")"
            else
                to="$(command git rev-parse --short "$target_commit:$path")"
            fi
            line="  gitlink $path: $from -> $to"
            if [ "$near" = 1 ]; then
                line="$line, upstream moved it $(command git rev-list --count \
                    --since="$since" "$base" -- "$path") times in the window"
            fi
            echo "$line"
            ;;
        apart) print_file "$path" "$added" "$removed" ;;
    esac
done < "$work/paths"
echo "  binary files: $(grep -c '^binary' "$work/paths" || true)"
echo

grep_upstream() {
    local pattern="$1"
    command git grep -nE "$pattern" ${grep_rev[@]+"${grep_rev[@]}"} \
        -- 'Telegram/SourceFiles/*.cpp' 'Telegram/SourceFiles/*.h' \
        | sed "s|^${grep_rev[0]:-}:||" \
        | awk -F: '
            FILENAME == ARGV[1] { split($0, f, "\t"); atBase[f[2]] = 1; next }
            !($1 in atBase) { next }
            { text = $0; sub(/^[^:]*:[^:]*:/, "", text) }
            text ~ /^[ \t]*(\/\/|\/\*|\*)/ { next }
            { print }
        ' "$work/base" -
}

work_mode_calls='(^|[^A-Za-z0-9_])(ActiveResolved|ExtraViews|ExtraViewPins|PresetPins|PresetOwnsPins|Filtering|Peeking|HideEverywhere|RecentStaySeconds|RecentMarkStyle|ShownFolders|FoldersRestricted|ExemptFolders|SilencedFolders|QuietFolders)\('
grep_upstream "$work_mode_calls" > "$work/workmode" || true
echo "Account-less Work Mode calls in upstream files: $(grep -c . "$work/workmode" || true)"
cut -d: -f1 "$work/workmode" | sort | uniq -c | sort -rn | sed 's/^ */  /'
echo

grep_upstream 'topPeers\(\)\.list\(\)' > "$work/toppeers" || true
echo "TopPeers::list() callers: $(grep -c . "$work/toppeers" || true)"
sed 's/^/  /' "$work/toppeers"
