#pragma once

#include "purple/purple_sync.h"
#include "purple/purple_ui_witness_protocol.h"
#include "ui/text/text.h"

#include <QPointer>
#include <QRegion>
#include <memory>

class QAction;
class QPainter;

namespace Purple {

struct UiWitnessView {
	ImportSettingsViewState native;
	HistoryView::Element *view = nullptr;
	HistoryItem *focused = nullptr;
};
using UiWitnessLookup = Fn<UiWitnessView(HistoryItem*)>;

struct UiWitnessPaintState;
struct UiWitnessTextState;

class UiWitnessPaint final {
public:
	UiWitnessPaint();
	explicit UiWitnessPaint(std::unique_ptr<UiWitnessPaintState> state);
	UiWitnessPaint(UiWitnessPaint &&other) noexcept;
	~UiWitnessPaint();

private:
	std::unique_ptr<UiWitnessPaintState> _state;

};

class UiWitnessText final {
public:
	UiWitnessText();
	explicit UiWitnessText(std::unique_ptr<UiWitnessTextState> state);
	UiWitnessText(UiWitnessText &&other) noexcept;
	~UiWitnessText();
	[[nodiscard]] Ui::Text::HighlightInfoRequest *highlight();

private:
	std::unique_ptr<UiWitnessTextState> _state;

};

void StartUiWitness();
[[nodiscard]] bool UiWitnessEnabled();
void InvalidateUiWitness(QWidget *receiver);
[[nodiscard]] UiWitnessPaint ObserveUiWitnessPaint(
	QWidget *receiver,
	Window::SessionController *controller,
	UiWitness::ReceiverKind kind,
	const QRegion &paintRegion,
	UiWitnessLookup lookup);
[[nodiscard]] UiWitnessText ObserveUiWitnessText(
	QPainter &painter,
	HistoryItem *container,
	HistoryItem *owner,
	const Ui::Text::String &text,
	bool caption,
	QRect textBounds,
	bool blocked);
void RecordUiWitnessImportAction(
	Ui::PopupMenu *menu,
	Window::SessionController *controller,
	HistoryItem *item,
	DocumentData *document,
	QAction *action);
void RegisterUiWitnessMenu(
	QWidget *receiver,
	Ui::PopupMenu *menu,
	int inputReason,
	bool blocked);

namespace UiWitness {

struct NativeWindow {
	quint32 number = 0;
	QRect frame;
	QPoint origin;
	int scale = 0;
	friend bool operator==(const NativeWindow&, const NativeWindow&) = default;
};
[[nodiscard]] std::optional<NativeWindow> ResolveNativeWindow(QWidget *widget);

}
}
