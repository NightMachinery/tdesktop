#include "purple/purple_settings_tree.h"

#include "base/basic_types.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <array>

namespace Purple::SettingsTree {
namespace {

class ColorSheet final : public QWidget {
public:
	ColorSheet(QWidget *parent, Rgb initial, Fn<void(Rgb)> stage)
	: QWidget(parent)
	, _value(initial)
	, _stage(std::move(stage))
	, _hex(new QLineEdit(FixtureColorHex(initial), this))
	, _apply(new QPushButton(u"Stage"_q, this)) {
		setAccessibleName(u"Fixture colour"_q);
		auto layout = new QVBoxLayout(this);
		auto form = new QFormLayout();
		layout->addLayout(form);
		form->addRow(u"Hex"_q, _hex);
		const auto labels = std::array{ u"Red"_q, u"Green"_q, u"Blue"_q };
		for (auto i = 0; i != int(_channels.size()); ++i) {
			const auto field = _channels[i] = new QSpinBox(this);
			field->setRange(0, 255);
			form->addRow(labels[i], field);
			connect(field, &QSpinBox::valueChanged, this, [this, i](int next) {
				const auto values = std::array{ _value.red, _value.green, _value.blue };
				_value = EditFixtureColor(_value, i, next - values[i]);
				_hex->setText(FixtureColorHex(_value));
				updatePreviews();
			});
		}
		for (auto i = 0; i != int(_samples.size()); ++i) {
			const auto sample = _samples[i] = new QLabel(
				i == 0 ? u"Light preview"_q : u"Dark preview"_q, this);
			sample->setAutoFillBackground(true);
			layout->addWidget(sample);
		}
		layout->addWidget(_apply);
		connect(_hex, &QLineEdit::textChanged, this, [this] {
			if (const auto color = ParseFixtureColor(_hex->text())) {
				_value = *color;
				updateChannels();
				updatePreviews();
			}
			_apply->setEnabled(ParseFixtureColor(_hex->text()).has_value());
		});
		connect(_apply, &QPushButton::clicked, this, [this] {
			if (const auto color = ParseFixtureColor(_hex->text()); color && _stage) {
				_stage(*color);
			}
		});
		updateChannels();
		updatePreviews();
	}

private:
	void updateChannels() {
		const auto values = std::array{ _value.red, _value.green, _value.blue };
		for (auto i = 0; i != int(_channels.size()); ++i) {
			const auto blocker = QSignalBlocker(_channels[i]);
			_channels[i]->setValue(values[i]);
		}
	}

	void updatePreviews() {
		for (auto i = 0; i != int(_samples.size()); ++i) {
			const auto sample = _samples[i];
			auto colors = sample->palette();
			colors.setColor(QPalette::Window, i == 0 ? Qt::white : Qt::black);
			colors.setColor(QPalette::WindowText, QColor(_value.red, _value.green, _value.blue));
			sample->setPalette(colors);
		}
	}

	Rgb _value;
	Fn<void(Rgb)> _stage;
	QLineEdit *_hex;
	QPushButton *_apply;
	std::array<QSpinBox*, 3> _channels = {};
	std::array<QLabel*, 2> _samples = {};
};

} // namespace

[[nodiscard]] QWidget *CreateSettingsColorFixture(
		QWidget *parent,
		Rgb initial,
		Fn<void(Rgb)> stage) {
	return new ColorSheet(parent, initial, std::move(stage));
}

} // namespace Purple::SettingsTree
