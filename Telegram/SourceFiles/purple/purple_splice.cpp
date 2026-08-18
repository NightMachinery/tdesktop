/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_splice.h"

#include <algorithm>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

constexpr auto kDefaultIndent = "  ";

struct Position {
	int line = 0; // 1-based, matching toml++ source positions.
	int column = 0; // 1-based.
};

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

// A display name is arbitrary user-controlled text and this ends up on a line
// of TOML, so anything that could break out of the comment has to go. Only line
// breaks can: everything after '#' is comment until end of line.
[[nodiscard]] QString CommentText(const MemberTitle &title, PeerIdValue id) {
	if (!title) {
		return QString();
	}
	auto result = title(id);
	if (result.isEmpty()) {
		return QString();
	}
	for (auto &ch : result) {
		if (ch == '\n' || ch == '\r' || ch == '\t') {
			ch = ' ';
		}
	}
	result = result.simplified();
	return result.isEmpty() ? QString() : (u" # "_q + result);
}

[[nodiscard]] QString Indentation(const QString &line) {
	auto i = 0;
	while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
		++i;
	}
	return line.left(i);
}

// True for a line inside the array that carries no id: blank, or the user's own
// comment. Those have to survive a member being removed next to them.
[[nodiscard]] bool BlankOrComment(const QString &line) {
	const auto trimmed = line.trimmed();
	return trimmed.isEmpty() || trimmed.startsWith('#');
}

// The id on a canonical member line, if the line is one. Anything else - two
// ids on a line, a trailing expression, a nested array - returns nothing, and
// the caller falls back to rewriting the whole array.
[[nodiscard]] std::optional<PeerIdValue> MemberOnLine(const QString &line) {
	auto rest = line.trimmed();
	const auto hash = rest.indexOf('#');
	if (hash >= 0) {
		rest = rest.left(hash).trimmed();
	}
	if (rest.endsWith(',')) {
		rest = rest.left(rest.size() - 1).trimmed();
	}
	if (rest.isEmpty()) {
		return std::nullopt;
	}
	auto ok = false;
	const auto id = rest.toLongLong(&ok);
	return ok ? std::make_optional(PeerIdValue(id)) : std::nullopt;
}

// Scans for the ']' that closes the '[' at `from`, skipping comments and
// counting nesting. Done by hand rather than read off toml++'s source region so
// the arithmetic here does not depend on whether that region's end is
// inclusive; the only thing between the brackets is integers and comments.
[[nodiscard]] std::optional<Position> FindClosing(
		const QStringList &lines,
		Position from) {
	auto depth = 0;
	for (auto line = from.line; line <= lines.size(); ++line) {
		const auto &text = lines[line - 1];
		const auto start = (line == from.line) ? (from.column - 1) : 0;
		for (auto i = start; i < text.size(); ++i) {
			const auto ch = text[i];
			if (ch == '#') {
				break;
			} else if (ch == '[') {
				++depth;
			} else if (ch == ']') {
				if (--depth == 0) {
					return Position{ line, i + 1 };
				}
			}
		}
	}
	return std::nullopt;
}

// Canonical form is one id per line with a trailing comma, the '[' alone at the
// end of its line and the ']' alone on its own. Only then can we edit single
// lines and leave the user's comments between them untouched; anything else
// gets rewritten wholesale, which is the largest blast radius we allow.
[[nodiscard]] bool IsCanonical(
		const QStringList &lines,
		Position open,
		Position close) {
	if (close.line <= open.line) {
		return false;
	}
	const auto afterOpen = lines[open.line - 1].mid(open.column);
	if (!BlankOrComment(afterOpen)) {
		return false;
	}
	const auto beforeClose = lines[close.line - 1].left(close.column - 1);
	if (!beforeClose.trimmed().isEmpty()) {
		return false;
	}
	for (auto line = open.line + 1; line < close.line; ++line) {
		const auto &text = lines[line - 1];
		// Commas need no checking: we only get here from a document that
		// already parsed, so they are wherever TOML requires them to be.
		if (!BlankOrComment(text) && !MemberOnLine(text)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString MemberLine(
		const QString &indent,
		PeerIdValue id,
		const MemberTitle &title) {
	return indent + QString::number(id) + u","_q + CommentText(title, id);
}

[[nodiscard]] const toml::table *FindListTable(
		const toml::table &root,
		const QString &list,
		QString &error) {
	const auto lists = root.get("lists");
	if (!lists || !lists->as_table()) {
		error = u"settings.toml has no [lists] tables."_q;
		return nullptr;
	}
	const auto utf8 = list.toUtf8();
	const auto node = lists->as_table()->get(
		std::string_view(utf8.constData(), utf8.size()));
	if (!node || !node->as_table()) {
		error = u"settings.toml has no [lists.%1] table."_q.arg(list);
		return nullptr;
	} else if (node->as_table()->is_inline()) {
		error = u"[lists.%1] is written inline; rewrite it as a table "
			"before editing its members from the app."_q.arg(list);
		return nullptr;
	}
	return node->as_table();
}

[[nodiscard]] std::vector<PeerIdValue> IdsOf(const toml::array &array) {
	auto result = std::vector<PeerIdValue>();
	for (auto &&element : array) {
		if (const auto id = element.value<int64>()) {
			result.push_back(*id);
		}
	}
	return result;
}

// Re-reads what we just wrote and refuses to hand it back unless it parses and
// the list holds exactly the ids we intended. Should be unreachable; it exists
// because the alternative failure mode is a corrupted file the user has to
// repair by hand.
[[nodiscard]] QString VerifySplice(
		const QString &text,
		const QString &path,
		const QString &list,
		const std::vector<PeerIdValue> &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	auto ignored = QString();
	const auto table = FindListTable(parsed.table(), list, ignored);
	if (!table) {
		return u"the edit lost [lists.%1]"_q.arg(list);
	}
	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	const auto ids = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	if (ids != expected) {
		return u"the edit left [lists.%1] holding the wrong members"_q
			.arg(list);
	}
	return QString();
}

[[nodiscard]] SpliceResult Refuse(const QString &text, const QString &error) {
	auto result = SpliceResult();
	result.text = text;
	result.error = error;
	return result;
}

[[nodiscard]] SpliceResult Unchanged(const QString &text) {
	auto result = SpliceResult();
	result.text = text;
	return result;
}

[[nodiscard]] SpliceResult Splice(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title,
		bool add) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto table = FindListTable(parsed.table(), list, error);
	if (!table) {
		return Refuse(text, error);
	}

	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	if (members && !array) {
		return Refuse(text, u"[lists.%1] has a 'members' that is not an "
			"array."_q.arg(list));
	}
	const auto before = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	const auto known = std::find(before.begin(), before.end(), id)
		!= before.end();
	if (add == known) {
		return Unchanged(text);
	}
	auto expected = before;
	if (add) {
		expected.push_back(id);
	} else {
		expected.erase(
			std::remove(expected.begin(), expected.end(), id),
			expected.end());
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	if (!array) {
		// The key is gone - the user deleted it, or the list was written
		// without one. Put a canonical array right under the table header.
		const auto header = int(table->source().begin.line);
		if (header < 1 || header > lines.size()) {
			return Refuse(text, u"could not locate [lists.%1]."_q.arg(list));
		}
		const auto indent = Indentation(lines[header - 1]);
		const auto inner = indent + QString::fromLatin1(kDefaultIndent);
		lines.insert(header, indent + u"]"_q + ending);
		lines.insert(header, MemberLine(inner, id, title) + ending);
		lines.insert(header, indent + u"members = ["_q + ending);
	} else {
		const auto open = Position{
			int(array->source().begin.line),
			int(array->source().begin.column),
		};
		if (open.line < 1 || open.line > lines.size()) {
			return Refuse(text, u"could not locate the members of "
				"[lists.%1]."_q.arg(list));
		}
		const auto close = FindClosing(lines, open);
		if (!close) {
			return Refuse(text, u"the members array of [lists.%1] is not "
				"closed."_q.arg(list));
		}
		const auto keyIndent = Indentation(lines[open.line - 1]);
		if (IsCanonical(lines, open, *close)) {
			auto indent = keyIndent + QString::fromLatin1(kDefaultIndent);
			for (auto line = open.line + 1; line < close->line; ++line) {
				if (MemberOnLine(lines[line - 1])) {
					indent = Indentation(lines[line - 1]);
					break;
				}
			}
			if (add) {
				lines.insert(
					close->line - 1,
					MemberLine(indent, id, title) + ending);
			} else {
				for (auto line = close->line - 1; line > open.line; --line) {
					if (MemberOnLine(lines[line - 1]) == id) {
						lines.removeAt(line - 1);
					}
				}
			}
		} else {
			// Squashed onto one line, or otherwise not something we can edit a
			// line at a time. Rewrite exactly the bracketed span - that is the
			// whole permissible blast radius - and leave the rest alone.
			const auto indent = keyIndent + QString::fromLatin1(kDefaultIndent);
			const auto head = lines[open.line - 1].left(open.column - 1);
			const auto tail = lines[close->line - 1].mid(close->column);
			auto replacement = QStringList{ head + u"["_q + ending };
			for (const auto member : expected) {
				replacement.push_back(
					MemberLine(indent, member, title) + ending);
			}
			replacement.push_back(keyIndent + u"]"_q + tail);

			auto rebuilt = lines.mid(0, open.line - 1);
			rebuilt += replacement;
			rebuilt += lines.mid(close->line);
			lines = std::move(rebuilt);
		}
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySplice(result.text, path, list, expected);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

} // namespace

SpliceResult AddListMember(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Splice(text, path, list, id, title, true);
}

SpliceResult RemoveListMember(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Splice(text, path, list, id, title, false);
}

std::vector<PeerIdValue> ListMembers(
		const QString &text,
		const QString &path,
		const QString &list) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return {};
	}
	auto ignored = QString();
	const auto table = FindListTable(parsed.table(), list, ignored);
	if (!table) {
		return {};
	}
	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	return array ? IdsOf(*array) : std::vector<PeerIdValue>();
}

} // namespace Purple
