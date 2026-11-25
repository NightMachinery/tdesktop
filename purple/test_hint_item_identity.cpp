#include "purple/purple_hint_item_identity.h"

#include <QtGui/QStandardItemModel>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListView>

#include <iostream>
#include <memory>

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
}
