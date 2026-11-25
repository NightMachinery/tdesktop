#pragma once

#include "base/basic_types.h"
#include "base/weak_ptr.h"
#include "data/data_types.h"
#include "purple/purple_keys_mx.h"
#include "purple/purple_keys_unread.h"
#include "purple/purple_notice_state.h"
#include "ui/layers/layer_widget.h"
#include "ui/rp_widget.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Purple::ModalUi {

enum class BadgeKind {
	Normal,
	Insert,
	Leader,
	Local,
	Hints,
	Palette,
	DescribeKey,
	Help,
};

struct StatusState {
	BadgeKind badge = BadgeKind::Normal;
	QString badgeText;
	QString paneLabel;
	QString contextChip;
	QString pendingText;
	QString pendingGroup;
	QString message;
	bool messageIsError = false;
	QString localleaderHint;
	QString rightCue;
	int unreadDm = 0;
	int unreadGp = 0;
	int unreadCh = 0;
	bool unreadKnown = false;
	bool zenMode = false;
};

class ModalStatusBar final : public Ui::RpWidget {
public:
	explicit ModalStatusBar(not_null<QWidget*> parent);

	static constexpr int kHeight = 28;

	void setState(StatusState state);
	[[nodiscard]] const StatusState &state() const { return _state; }

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	StatusState _state;
};

struct WhichKeyEntry {
	QStringList keys;
	QString label;
	bool isGroup = false;
};

struct WhichKeyState {
	QStringList prefixCaps;
	QString title;
	QString contextBadge;
	bool isLocal = false;
	std::vector<WhichKeyEntry> groups;
	std::vector<WhichKeyEntry> leaves;
};

class WhichKeyOverlay final : public Ui::RpWidget {
public:
	explicit WhichKeyOverlay(not_null<QWidget*> parent);

	void setState(WhichKeyState state);
	void updateBounds(QRect availableAboveStatusBar);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	[[nodiscard]] int computeColumns(int availWidth) const;
	[[nodiscard]] int computeHeight(int availWidth, int maxHeight) const;

	WhichKeyState _state;
	int _columns = 2;
	int _columnWidth = 220;
};

struct HelpRow {
	std::vector<QStringList> chords;
	QString label;
	QString commandId;
	QString contextTag;
};

struct HelpSection {
	QString title;
	std::vector<HelpRow> rows;
};

struct HelpData {
	bool startOnAllBindings = false;
	QString keymapPath;
	QString keymapError;
	std::vector<HelpSection> cheatsheetCards;
	std::vector<HelpSection> activeBindingGroups;
	Fn<void(bool showingAll)> onTabChanged;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateModalHelpLayer(
	HelpData data);

struct DescribeBinding {
	QStringList caps;
	QString where;
};

struct DescribeData {
	bool isKeyMode = false;
	QStringList queriedChord;
	QString title;
	QString commandId;
	bool found = false;
	bool availableHere = false;
	bool keylessOnPurpose = false;
	QString unavailableReason;
	std::vector<DescribeBinding> bindings;
	std::vector<DescribeBinding> elsewhere;
	Fn<void()> onDescribeCommand;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateDescribeLayer(
	DescribeData data);

struct PaletteItem {
	int actionId = 0;
	QString id;
	QString label;
	QString category;
	QString aliases;
	QString unavailableReason;
	std::vector<QStringList> chords;
	bool available = true;
	base::weak_ptr<Main::Session> avatarSession;
	PeerId avatarPeerId = 0;
};

struct PaletteConfig {
	QString promptBadge;
	QString placeholder;
	bool describeMode = false;
	std::vector<PaletteItem> items;
	ModalMx::History history;
	Fn<void(int actionId, QString query)> onPick;
	Fn<void()> onCancel;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateCommandPaletteLayer(
	PaletteConfig config);

struct UnreadBufferConfig {
	base::weak_ptr<Main::Session> session;
	std::vector<ModalUnread::BufferItem> items;
	Fn<std::vector<ModalUnread::BufferItem>()> onRefresh;
	Fn<void(PeerId peerId)> onOpenChat;
	Fn<void(std::vector<PeerId> peerIds)> onMarkRead;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateUnreadBufferLayer(
	UnreadBufferConfig config);

struct TopicItem {
	MsgId rootId = 0;
	QString title;
	QString preview;
	DocumentId iconId = 0;
	int32 colorId = 0;
	int unreadCount = 0;
	int mentions = 0;
	bool muted = false;
	bool pinned = false;
	bool closed = false;
	bool isGeneral = false;
	bool active = false;
	int64_t lastTimestamp = 0;
};

struct TopicPickerSnapshot {
	std::vector<TopicItem> items;
	bool hasMore = false;
};

struct TopicPickerConfig {
	base::weak_ptr<Main::Session> session;
	PeerId parentPeerId = 0;
	QString parentTitle;
	bool isBot = false;
	bool startUnreadOnly = false;
	QString actionLabel;
	std::vector<TopicItem> items;
	bool hasMore = false;
	Fn<TopicPickerSnapshot()> onRefresh;
	Fn<void()> onLoadMore;
	Fn<void()> onReload;
	rpl::producer<> pageUpdates;
	rpl::producer<> updates;
	rpl::producer<> destroyed;
	Fn<void(MsgId rootId)> onOpenTopic;
	Fn<void()> onCreateTopic;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateTopicPickerLayer(
	TopicPickerConfig config);

struct NoticeLayerConfig {
	ModalNotices::NoticeRecord record;
	QString primaryActionLabel;
	Fn<void()> onPrimaryAction;
	Fn<bool()> onCopy;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateNoticeLayer(
	NoticeLayerConfig config);

struct NoticeHistoryConfig {
	QString accountLabel;
	std::vector<ModalNotices::NoticeRecord> items;
	Fn<void(ModalNotices::NoticeRecord)> onInspect;
	Fn<void(const ModalNotices::NoticeRecord &)> onCopy;
	Fn<void()> onClear;
};

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateNoticeHistoryLayer(
	NoticeHistoryConfig config);

} // namespace Purple::ModalUi
