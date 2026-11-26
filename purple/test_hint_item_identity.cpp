#include "purple/purple_hint_item_identity.h"
#include "purple/purple_window_hint_geometry.h"

#include <QtGui/QStandardItemModel>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>

#include <iostream>
#include <memory>

namespace {

class HintOpening final : public QObject {
public:
	HintOpening(QWidget &root, QWidget &candidate)
	: _root(root)
	, _candidate(candidate) {
		_root.installEventFilter(this);
		_candidate.installEventFilter(this);
	}

	bool check() {
		_overlay = std::make_unique<QWidget>(&_root);
		_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
		_overlay->setGeometry(_root.rect());
		_overlay->show();
		_overlay->raise();
		qApp->processEvents();
		const auto point = _candidate.mapToGlobal(_candidate.rect().center());
		if (!_sawLayoutRequest || _invalidated || !_overlay->isVisible()
			|| !Purple::WindowHintExposed(&_candidate, point)) {
			return false;
		}
		_candidate.move(_candidate.pos() + QPoint(0, 2));
		return _invalidated;
	}

private:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (_overlay && event->type() == QEvent::LayoutRequest && watched == &_root) {
			_sawLayoutRequest = true;
		}
		if (_overlay && Purple::WindowHintGeometryEvent(event->type())) {
			_invalidated = true;
		}
		return false;
	}

	QWidget &_root;
	QWidget &_candidate;
	bool _sawLayoutRequest = false;
	bool _invalidated = false;
	std::unique_ptr<QWidget> _overlay;
};

bool CheckHintOpenings() {
	auto window = QWidget();
	window.resize(480, 320);
	auto root = QWidget(&window);
	root.setGeometry(window.rect());
	auto dialogs = QListView(&root);
	auto model = QStandardItemModel();
	model.appendRow(new QStandardItem(QStringLiteral("Chat")));
	dialogs.setModel(&model);
	dialogs.setGeometry(0, 0, 160, 320);
	auto history = QPushButton(QStringLiteral("Message link"), &root);
	history.setGeometry(170, 30, 140, 32);
	auto layer = QWidget(&window);
	layer.setGeometry(10, 10, 380, 250);
	auto edit = QPushButton(QStringLiteral("Edit"), &layer);
	edit.setGeometry(10, 10, 100, 30);
	layer.hide();
	window.show();
	qApp->processEvents();

	if (!HintOpening(root, *dialogs.viewport()).check()
		|| !HintOpening(root, history).check()) {
		return false;
	}
	layer.show();
	layer.raise();
	qApp->processEvents();
	return HintOpening(layer, edit).check();
}

} // namespace

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	auto model = QStandardItemModel();
	model.appendRow(new QStandardItem(QStringLiteral("First")));
	model.appendRow(new QStandardItem(QStringLiteral("Second")));
	auto list = std::make_unique<QListView>();
	list->setModel(&model);
	list->show();
	app.processEvents();
	const auto capture = [&](int row) {
		const auto index = QPersistentModelIndex(model.index(row, 0));
		return Purple::HintItemIdentity{
			list.get(), list->viewport(), index, list->viewport()->mapToGlobal(QPoint()),
			list->visualRect(index).center(),
		};
	};
	const auto first = capture(0);
	if (!first.current()) return 1;
	model.item(0)->setEnabled(false);
	if (first.current()) return 2;
	model.item(0)->setEnabled(true);
	model.sort(0, Qt::DescendingOrder);
	app.processEvents();
	if (!first.index.isValid() || first.current()) return 3;
	const auto moved = capture(1);
	if (!moved.current()) return 4;
	model.removeRow(1);
	if (moved.current()) return 5;
	const auto second = capture(0);
	if (!second.current()) return 6;
	list.reset();
	if (second.current()) return 7;
	std::cout << "7 live and stale Qt item identity checks, 0 failures\n";
	if (!CheckHintOpenings()) {
		std::cerr << "A shown overlay must survive its own layout request and keep the target exposed.\n";
		return 8;
	}
	std::cout << "3 hint openings with exposed dialog, history, and layer targets, 0 failures\n";
}
