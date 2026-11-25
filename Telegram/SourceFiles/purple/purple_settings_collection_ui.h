#pragma once

#include "purple/purple_settings_editor.h"
#include "purple/purple_settings_structures.h"

#include <QtGui/QIcon>

class QWidget;

namespace Purple {

struct SettingsPeerChoice {
	QString literal;
	QString label;
	QIcon avatar;
	Fn<bool()> current;
};

struct SettingsCollectionConfig {
	QString title;
	SettingAddress address;
	QByteArray bytes;
	std::optional<SettingsStructures::PeerField> peerField;
	std::vector<SettingsPeerChoice> peers;
	std::vector<SettingsEdit> seed;
	std::optional<SettingsEditorMembers> members;
	bool named = false;
	std::function<bool()> current;
	std::function<void(SettingsEditorIntent)> stage;
	std::function<void(SettingAddress)> inspect;
	std::function<void()> close;
};

[[nodiscard]] QWidget *CreateSettingsCollection(QWidget *parent, SettingsCollectionConfig config);
void BackFromSettingsCollection(QWidget *collection);
void RefreshSettingsCollectionPeers(QWidget *collection, bool sourceCleared = false);
[[nodiscard]] bool SettingsCollectionHasPreedit(QWidget *collection);

} // namespace Purple
