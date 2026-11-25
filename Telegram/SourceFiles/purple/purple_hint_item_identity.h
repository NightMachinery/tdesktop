#pragma once

#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPointer>
#include <QtWidgets/QAbstractItemView>

namespace Purple {

struct HintItemIdentity {
	QPointer<QAbstractItemView> view;
	QPointer<QWidget> viewport;
	QPersistentModelIndex index;
	QPoint origin;
	QPoint point;

	[[nodiscard]] bool current() const {
		return view && viewport && index.isValid() && index.flags().testFlag(Qt::ItemIsEnabled)
			&& view->viewport() == viewport && viewport->mapToGlobal(QPoint()) == origin
			&& view->indexAt(point) == index;
	}
};

} // namespace Purple
