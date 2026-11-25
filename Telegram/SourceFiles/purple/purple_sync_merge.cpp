#include "purple/purple_sync_merge.h"

#include "purple/purple_settings.h"
#include "purple/purple_sync_config_flow.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

using Kind = SyncMergeDifference::Kind;
using Choice = SyncMergeChoice;
using Node = toml::node;

constexpr auto kMaximumDifferences = 512;

struct Merge {
	std::vector<SyncMergeDifference> *differences = nullptr;
	const std::vector<Choice> *choices = nullptr;
	size_t index = 0;
	bool valid = true;

	Choice select(Kind kind, const QString &path, const Node *local, const Node *remote);
	toml::table table(const toml::table *local, const toml::table *remote, const QString &path);
	void value(toml::table &out, std::string_view key, const Node *local, const Node *remote, const QString &path);
};

template <typename Type>
std::string Format(const Type &value) {
	auto stream = std::ostringstream();
	stream << toml::toml_formatter{ value };
	return stream.str();
}

QString Display(const Node *node) {
	return node ? QString::fromStdString(Format(*node)) : QString();
}

QString ChildPath(const QString &parent, std::string_view key) {
	const auto property = QString::fromUtf8(key.data(), qsizetype(key.size()));
	const auto quoted = key.empty() || key.find_first_not_of(
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos;
	const auto label = quoted
		? u"["_q + QString::fromStdString(Format(toml::value(std::string(key)))) + u"]"_q
		: property;
	return parent.isEmpty() ? label : parent + (quoted ? QString() : u"."_q) + label;
}

Choice Merge::select(Kind kind, const QString &path, const Node *local, const Node *remote) {
	if (++index > kMaximumDifferences) {
		valid = false;
		return Choice::Drop;
	}
	if (differences) {
		differences->push_back({ kind, path, Display(local), Display(remote) });
		return Choice::Drop;
	}
	const auto choice = (index <= choices->size()) ? (*choices)[index - 1] : Choice::Unset;
	const auto side = choice == Choice::Local || choice == Choice::Remote;
	valid = valid && ((kind == Kind::Addition)
		? choice == Choice::Keep || choice == Choice::Drop
		: side || (kind == Kind::Conflict && choice == Choice::Drop));
	return choice;
}

bool UnionArray(const Node *node) {
	if (!node) {
		return true;
	}
	const auto array = node->as_array();
	return array && std::all_of(array->begin(), array->end(), [](const Node &value) {
		return value.is_string() || value.is_integer();
	});
}

using ElementKey = std::pair<std::string, size_t>;
using Elements = std::vector<std::pair<ElementKey, const Node*>>;

Elements IndexArray(const Node *node) {
	auto result = Elements();
	auto occurrences = std::map<std::string, size_t>();
	if (node) {
		for (const auto &value : *node->as_array()) {
			const auto s = value.as_string();
			const auto key = s
				? "s" + s->get()
				: "i" + std::to_string(value.as_integer()->get());
			result.push_back({ { key, occurrences[key]++ }, &value });
		}
	}
	return result;
}

std::set<ElementKey> ArrayKeys(const Elements &elements) {
	auto keys = std::set<ElementKey>();
	for (const auto &element : elements) {
		keys.insert(element.first);
	}
	return keys;
}

toml::array MergeArray(Merge &merge, const Node *local, const Node *remote, const QString &path) {
	const auto left = IndexArray(local);
	const auto right = IndexArray(remote);
	const auto leftKeys = ArrayKeys(left);
	const auto rightKeys = ArrayKeys(right);
	auto leftShared = std::vector<ElementKey>();
	auto rightShared = std::vector<ElementKey>();
	auto retained = std::map<ElementKey, const Node*>();
	const auto collect = [&](const Elements &elements, const std::set<ElementKey> &other,
			std::vector<ElementKey> &shared, bool fromLocal) {
		for (const auto &[key, value] : elements) {
			if (!merge.valid) {
				return;
			} else if (other.contains(key)) {
				shared.push_back(key);
				retained.emplace(key, value);
			} else if (merge.select(Kind::Addition, path + u"[]"_q,
					fromLocal ? value : nullptr, fromLocal ? nullptr : value) == Choice::Keep) {
				retained.emplace(key, value);
			}
		}
	};
	collect(left, rightKeys, leftShared, true);
	collect(right, leftKeys, rightShared, false);
	const auto order = (merge.valid && leftShared != rightShared)
		? merge.select(Kind::Order, path, local, remote)
		: Choice::Local;
	auto result = toml::array();
	const auto append = [&](const Elements &elements) {
		for (const auto &[key, value] : elements) {
			if (retained.erase(key)) {
				result.push_back(*value);
			}
		}
	};
	append(order == Choice::Remote ? right : left);
	append(order == Choice::Remote ? left : right);
	return result;
}

void Merge::value(
		toml::table &out,
		std::string_view key,
		const Node *local,
		const Node *remote,
		const QString &path) {
	const auto left = local ? local->as_table() : nullptr;
	const auto right = remote ? remote->as_table() : nullptr;
	if ((left || right) && (left || !local) && (right || !remote)
		&& ((left && !left->empty()) || (right && !right->empty()))) {
		auto merged = table(left, right, path);
		if ((left && right) || !merged.empty()) {
			out.insert(key, std::move(merged));
		}
		return;
	}
	if (local && remote && local->type() == remote->type() && Format(*local) == Format(*remote)) {
		out.insert(key, *local);
		return;
	}
	if (UnionArray(local) && UnionArray(remote)
		&& ((local && !local->as_array()->empty()) || (remote && !remote->as_array()->empty()))) {
		auto merged = MergeArray(*this, local, remote, path);
		if ((local && remote) || !merged.empty()) {
			out.insert(key, std::move(merged));
		}
		return;
	}
	const auto choice = select((local && remote) ? Kind::Conflict : Kind::Addition, path, local, remote);
	const auto source = (choice == Choice::Local) ? local
		: (choice == Choice::Remote) ? remote
		: (choice == Choice::Keep) ? (local ? local : remote)
		: nullptr;
	if (source) {
		out.insert(key, *source);
	}
}

toml::table Merge::table(const toml::table *local, const toml::table *remote, const QString &path) {
	auto keys = std::set<std::string_view>();
	const auto add = [&](const toml::table *source) {
		if (source) {
			for (const auto &item : *source) {
				keys.insert(item.first.str());
			}
		}
	};
	add(local);
	add(remote);
	auto result = toml::table();
	result.is_inline((local && local->is_inline()) || (!local && remote && remote->is_inline()));
	for (const auto key : keys) {
		if (!valid) {
			break;
		}
		const auto left = local ? local->get(key) : nullptr;
		const auto right = remote ? remote->get(key) : nullptr;
		if (path.isEmpty() && key == "version") {
			if (left) {
				result.insert(key, *left);
			}
		} else {
			value(result, key, left, right, ChildPath(path, key));
		}
	}
	return result;
}

bool RunMerge(Merge &merge, const QByteArray &left, const QByteArray &right, toml::table &out) {
	if (left.size() > kSyncSettingsMaximumBytes || right.size() > kSyncSettingsMaximumBytes) {
		return false;
	}
	const auto view = [](const QByteArray &bytes) {
		return std::string_view(bytes.constData(), size_t(bytes.size()));
	};
	const auto local = toml::parse(view(left));
	const auto remote = toml::parse(view(right));
	if (!local || !remote) {
		return false;
	}
	const auto version = [](const toml::table &table) {
		const auto node = table.get("version");
		const auto value = node ? node->as_integer() : nullptr;
		return value ? value->get() : node ? 0 : kDefaultSettingsVersion;
	};
	const auto current = version(local.table());
	if (current < kDefaultSettingsVersion || current > kHighestSettingsVersion
		|| current != version(remote.table())) {
		return false;
	}
	out = merge.table(&local.table(), &remote.table(), QString());
	return merge.valid;
}

} // namespace

std::optional<SyncMergeReview> ReviewSyncMerge(const QByteArray &local, const QByteArray &remote) {
	auto result = SyncMergeReview{ local, remote, {} };
	auto merge = Merge{ .differences = &result.differences };
	auto tree = toml::table();
	return (RunMerge(merge, local, remote, tree) && !result.differences.empty())
		? std::make_optional(std::move(result))
		: std::nullopt;
}

std::optional<QByteArray> BuildSyncMerge(
		const SyncMergeReview &review,
		const std::vector<SyncMergeChoice> &choices) {
	auto merge = Merge{ .choices = &choices };
	auto tree = toml::table();
	if (!RunMerge(merge, review.local, review.remote, tree) || merge.index != choices.size()) {
		return std::nullopt;
	}
	auto text = QByteArray::fromStdString(Format(tree)) + '\n';
	return text.size() <= kSyncSettingsMaximumBytes
		? std::make_optional(std::move(text))
		: std::nullopt;
}

} // namespace Purple
