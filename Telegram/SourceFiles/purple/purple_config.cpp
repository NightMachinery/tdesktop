/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_config.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

// Return parse errors instead of throwing, so a hand-edited file with a typo
// degrades to "keep the defaults and log it" rather than to an exception
// crossing a Qt event handler.
#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

constexpr auto kPremiumTable = "premium";
constexpr auto kPremiumEnabledKey = "enabled";
constexpr auto kPremiumEnabledDefault = true;

// Written once, on first run. Everything the user may want to change should be
// present and commented, because an empty file teaches nobody what is possible.
constexpr auto kStarterSettings = R"([premium]
# Unlock the Telegram Premium features that Telegram Desktop gates on the
# client alone: no sponsored messages, exact "last seen" times, real-time chat
# translation, and up to six accounts instead of three.
#
# Features the server enforces - large uploads, faster downloads, voice-to-text,
# custom emoji, emoji status, message effects and the rest - are unaffected by
# this and stay locked. See docs/purple/premium.md.
enabled = true
)";

// Everything here runs on the main thread, in response to either startup or a
// click in Settings, so no locking.
class Config final {
public:
	Config();

	[[nodiscard]] bool localPremium() const;
	[[nodiscard]] rpl::producer<bool> localPremiumValue() const;
	void setLocalPremium(bool value);

private:
	void load();
	bool parse();
	void createStarterFile();
	[[nodiscard]] bool writeBool(const char *table, const char *key, bool value);
	[[nodiscard]] bool replaceValueOnLine(int line, bool value);
	void save();

	QString _text;
	toml::table _parsed;

	// False when the file on disk could not be read or does not parse. We never
	// write to such a file: the user may be halfway through editing it, and a
	// blind append would leave them with a duplicate table to untangle.
	bool _valid = false;

	rpl::variable<bool> _localPremium = kPremiumEnabledDefault;

};

Config::Config() {
	load();
}

void Config::load() {
	const auto path = SettingsFilePath();
	auto file = QFile(path);
	if (!file.exists()) {
		createStarterFile();
		return;
	} else if (!file.open(QIODevice::ReadOnly)) {
		LOG(("Purple Error: Could not read %1.").arg(path));
		return;
	}
	_text = QString::fromUtf8(file.readAll());
	file.close();
	if (parse()) {
		_localPremium = _parsed[kPremiumTable][kPremiumEnabledKey].value_or(
			kPremiumEnabledDefault);
	}
}

bool Config::parse() {
	const auto utf8 = _text.toUtf8();
	auto result = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		SettingsFilePath().toStdString());
	if (!result) {
		const auto &error = result.error();
		LOG(("Purple Error: %1:%2:%3: %4."
			).arg(SettingsFilePath()
			).arg(error.source().begin.line
			).arg(error.source().begin.column
			).arg(QString::fromStdString(std::string(error.description()))));
		_valid = false;
		return false;
	}
	_parsed = std::move(result).table();
	_valid = true;
	return true;
}

void Config::createStarterFile() {
	const auto directory = ConfigDirectory();
	if (!QDir().mkpath(directory)) {
		LOG(("Purple Error: Could not create %1.").arg(directory));
		return;
	}
	_text = QString::fromUtf8(kStarterSettings);
	save();
	parse();
}

bool Config::localPremium() const {
	return _localPremium.current();
}

rpl::producer<bool> Config::localPremiumValue() const {
	return _localPremium.value();
}

void Config::setLocalPremium(bool value) {
	if (_localPremium.current() == value) {
		return;
	} else if (!writeBool(kPremiumTable, kPremiumEnabledKey, value)) {
		// Keep memory and disk agreeing. Changing the setting anyway would
		// apply now and silently revert on the next start.
		return;
	}
	_localPremium = value;
}

// Rewrites a single scalar in place, leaving every comment and every other line
// of the file untouched. The user owns this file; we only ever edit the exact
// token we are responsible for.
bool Config::writeBool(const char *table, const char *key, bool value) {
	if (!_valid) {
		if (!_text.isEmpty()) {
			LOG(("Purple Error: Not writing to %1, it does not parse."
				).arg(SettingsFilePath()));
			return false;
		}
		createStarterFile();
		if (!_valid) {
			return false;
		}
	}
	const auto node = _parsed[table][key].node();
	if (node && replaceValueOnLine(node->source().begin.line, value)) {
		save();
		parse();
		return true;
	}

	// The key is missing - add it, either under the existing table header or in
	// a fresh table appended to the end of the file.
	const auto line = u"%1 = %2"_q
		.arg(QString::fromLatin1(key))
		.arg(value ? u"true"_q : u"false"_q);
	const auto existing = _parsed[table].as_table();
	if (existing && !existing->is_inline()) {
		auto lines = _text.split('\n');
		const auto after = int(existing->source().begin.line);
		if (after > 0 && after <= lines.size()) {
			lines.insert(after, line);
			_text = lines.join('\n');
			save();
			parse();
			return true;
		}
	}
	if (!_text.isEmpty()) {
		if (!_text.endsWith('\n')) {
			_text += '\n';
		}
		_text += '\n';
	}
	_text += u"[%1]\n%2\n"_q.arg(QString::fromLatin1(table)).arg(line);
	save();
	parse();
	return true;
}

// Replaces the value token on a "key = value" line, keeping the key, the
// spacing and any trailing comment exactly as the user wrote them.
bool Config::replaceValueOnLine(int line, bool value) {
	auto lines = _text.split('\n');
	if (line < 1 || line > lines.size()) {
		return false;
	}
	auto &text = lines[line - 1];
	const auto assign = text.indexOf('=');
	if (assign < 0) {
		return false;
	}
	auto from = assign + 1;
	while (from < text.size() && text[from].isSpace()) {
		++from;
	}
	auto till = from;
	while (till < text.size()
		&& !text[till].isSpace()
		&& text[till] != '#') {
		++till;
	}
	if (till == from) {
		return false;
	}
	text.replace(from, till - from, value ? u"true"_q : u"false"_q);
	_text = lines.join('\n');
	return true;
}

void Config::save() {
	const auto path = SettingsFilePath();
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Purple Error: Could not write %1.").arg(path));
		return;
	}
	file.write(_text.toUtf8());
	if (!file.commit()) {
		LOG(("Purple Error: Could not commit %1.").arg(path));
	}
}

[[nodiscard]] Config &Instance() {
	static auto result = Config();
	return result;
}

} // namespace

QString ConfigDirectory() {
	const auto xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
	return xdg.isEmpty()
		? (QDir::homePath() + u"/.purple-telegram"_q)
		: (xdg + u"/purple-telegram"_q);
}

QString SettingsFilePath() {
	return ConfigDirectory() + u"/settings.toml"_q;
}

bool LocalPremium() {
	return Instance().localPremium();
}

rpl::producer<bool> LocalPremiumValue() {
	return Instance().localPremiumValue();
}

void SetLocalPremium(bool value) {
	Instance().setLocalPremium(value);
}

} // namespace Purple
