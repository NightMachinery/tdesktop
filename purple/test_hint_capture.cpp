#include "purple/purple_hint_capture.h"

#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <iostream>

class Track final : public QWidget {
public:
	using QWidget::QWidget;

protected:
	void paintEvent(QPaintEvent *) override {
		auto painter = QPainter(this);
		painter.translate(4, 6);
		Purple::RecordHintFace(painter, QRect(6, 16, 32, 32));
		Purple::RecordHintFace(painter, QRect(27, 36, 14, 14));
		painter.drawText(QPoint(46, 32), QStringLiteral("Track title"));
	}
};

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	auto root = QWidget();
	root.resize(300, 130);
	auto track = Track(&root);
	track.setGeometry(70, 30, 180, 70);
	root.show();
	app.processEvents();
	const auto visible = QRect(60, 10, 126, 100);
	const auto capture = Purple::CaptureHintGeometry(&root, visible);
	const auto face = QRect(80, 52, 32, 32);
	const auto download = QRect(101, 72, 14, 14);
	if (capture.faces != std::vector<QRect>{ download, face } || capture.text.empty()) return 1;
	const auto run = capture.text.front();
	if (run.left() != 120 || !run.contains(121, 64) || !visible.contains(run)) return 2;
	std::cout << "Overlapping controls, translated text and clipping checks passed\n";
}
