#include "editor/video/video_segment_player.h"
#include "purple/purple_recording_registry.h"
#include "ui/integration.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_direction.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>
#include <QtGui/QTextLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <iostream>

namespace crl { rpl::producer<> on_main_update_requests() { return rpl::never<>(); } }
namespace Media::Streaming { class Instance {}; }
namespace Editor {
SegmentPlayer::SegmentPlayer(QString path, QByteArray content, SegmentPlayerOptions options)
: _path(std::move(path)), _content(std::move(content)), _options(options) {}
SegmentPlayer::~SegmentPlayer() = default;
bool SegmentPlayer::valid() const { return !_soundFailed; }
bool SegmentPlayer::ready() const { return valid(); }
QImage SegmentPlayer::frame(QSize size) { return QImage(size, QImage::Format_ARGB32_Premultiplied); }
void SegmentPlayer::restart(crl::time position) {
    if (position < 0) { _soundFailed = true; _repaints.fire({}); return; }
    _position = position;
    _positionUpdates.fire_copy(position);
    _repaints.fire({});
}
} // namespace Editor
namespace tr {
inline constexpr auto now = 0;
QString lng_purple_recording_bin_play_failed(int) { return u"Playback failed"_q; }
} // namespace tr
namespace {
class Integration final : public Ui::Integration {
public:
    void postponeCall(FnMut<void()> &&callable) override { callable(); }
    void registerLeaveSubscription(not_null<QWidget*>) override {}
    void unregisterLeaveSubscription(not_null<QWidget*>) override {}
    QString emojiCacheFolder() override { return {}; }
    QString fontsCacheFolder() override { return {}; }
    QString openglCheckFilePath() override { return {}; }
    QString angleBackendFilePath() override { return {}; }
    void touchCounterIncrement() override {}
    int touchCounterNow() override { return 0; }
};
using Kind = Purple::RecordingRegistry::Kind;
constexpr auto kRoundPreview = 160;
Ui::FlatLabel *Progress(Ui::VerticalLayout &rows) {
    for (const auto child : rows.children()) {
        if (const auto label = dynamic_cast<Ui::FlatLabel*>(child)) return label;
    }
    return nullptr;
}
#include "recording-bin.inc"
} // namespace

int main(int argc, char **argv) {
    auto app = QApplication(argc, argv);
    auto integration = Integration();
    Ui::Integration::Set(&integration);
    style::StartManager(100);
    auto checks = 0, failures = 0;
    const auto expect = [&](bool value, const char *name) {
        ++checks;
        if (!value) { ++failures; std::cerr << name << '\n'; }
    };
    {
        auto rows = Ui::VerticalLayout();
        rows.resizeToWidth(480);
        rows.show();
        auto player = Editor::SegmentPlayer(QString(), QByteArray());
        for (const auto kind : { Kind::Voice, Kind::Round }) {
            for (auto rebuild = 0; rebuild != 3; ++rebuild) {
                rows.clear();
                player.restart(0);
                AddPlayback(&rows, &player, kind, 5000);
                auto clicks = 0;
                for (auto item = 0; item != 2; ++item) {
                    const auto actions = rows.add(object_ptr<BinActions>(&rows));
                    const auto play = actions->add(item ? u"Play"_q : u"Stop"_q, [&] { ++clicks; });
                    const auto save = actions->add(u"Save"_q, [] {});
                    rows.resizeToWidth(480);
                    QApplication::processEvents();
                    expect(actions->width() > 0 && actions->height() >= play->height()
                        && actions->rect().contains(play->geometry())
                        && actions->rect().contains(save->geometry()),
                        "production action layout retains painted clickable space after a rebuild");
                    rows.hide();
                    const auto wrapWidth = std::max(play->width(), save->width());
                    rows.resizeToWidth(wrapWidth);
                    expect(actions->height() >= play->height() + save->height()
                        && actions->rect().contains(play->geometry())
                        && actions->rect().contains(save->geometry()),
                        "hidden action row measures its wrapped height synchronously");
                    rows.resizeToWidth(480);
                    rows.show();
                    expect(!play->isHidden() && !save->isHidden(), "rebuilt rows keep both items' actions visible");
                    play->clicked(Qt::NoModifier, Qt::LeftButton);
                }
                expect(clicks == 2, "Stop and other row's Play remain actionable");
                rows.resizeToWidth(480);
                auto progress = Progress(rows);
                expect(progress && progress->accessibilityName().contains(u"0:00"_q), "playback starts with visible elapsed time");
                player.restart(2000);
                expect(progress && progress->accessibilityName().contains(u"0:02"_q), "voice and round position events update elapsed time");
                expect(progress && progress->accessibilityName().contains(u"0:05"_q), "total duration remains visible");
                rows.clear();
                player.restart(3000);
                expect(!Progress(rows), "stopped row unsubscribes before later position events");
            }
        }
        for (const auto direction : { Qt::LeftToRight, Qt::RightToLeft }) {
            rows.setLayoutDirection(direction);
            style::SetRightToLeft(direction == Qt::RightToLeft);
            for (const auto width : { 120, 240, 480 }) {
                rows.clear();
                rows.resizeToWidth(width);
                const auto actions = rows.add(object_ptr<BinActions>(&rows));
                const auto text = u"ارسال به گفتگوی دیگری Send to another conversation "_q.repeated(4);
                auto clicks = 0;
                const auto button = actions->add(text, [&] { ++clicks; });
                rows.resizeToWidth(width);
                expect(actions->rect().contains(button->geometry()), "long translated action stays inside its row");
                expect(button->accessibilityName() == text, "elided action retains its full accessible name");
                button->clicked(Qt::NoModifier, Qt::LeftButton);
                expect(clicks == 1, "narrow translated action remains actionable");
                rows.resizeToWidth(button->naturalWidth() + 32);
                expect(button->width() == button->naturalWidth(), "widening restores the natural translated action width");
            }
            rows.clear();
            rows.resizeToWidth(480);
            AddPlayback(&rows, &player, Kind::Voice, 65000);
            player.restart(2000);
            const auto progressText = Progress(rows)->accessibilityName();
            auto progressLayout = QTextLayout(progressText);
            auto progressOption = QTextOption();
            progressOption.setTextDirection(direction);
            progressLayout.setTextOption(progressOption);
            progressLayout.beginLayout();
            const auto progressLine = progressLayout.createLine();
            progressLayout.endLayout();
            expect(progressLine.cursorToX(progressText.indexOf(u"0:02"_q))
                < progressLine.cursorToX(progressText.indexOf(u"1:05"_q)),
                "elapsed time stays before total time in LTR and RTL");
            for (const auto sign : { -1, 1 }) {
                const auto text = SeekText(sign, 10);
                auto layout = QTextLayout(text);
                auto option = QTextOption();
                option.setTextDirection(direction);
                layout.setTextOption(option);
                layout.beginLayout();
                layout.createLine();
                layout.endLayout();
                const auto signIndex = text.indexOf(sign < 0 ? QChar(0x2212) : QChar('+'));
                const auto digitIndex = text.indexOf(QChar('1'));
                const auto glyphX = [&](int index) {
                    for (const auto &run : layout.glyphRuns(0, -1,
                            QTextLayout::RetrieveGlyphPositions | QTextLayout::RetrieveStringIndexes)) {
                        for (auto i = 0; i != run.stringIndexes().size(); ++i) {
                            if (run.stringIndexes()[i] == index) return run.positions()[i].x();
                        }
                    }
                    return qreal(-1);
                };
                expect(glyphX(signIndex) >= 0 && glyphX(signIndex) < glyphX(digitIndex),
                    "seek sign glyph stays before the number in LTR and RTL");
            }
        }
        for (const auto rtl : { false, true }) {
            style::SetRightToLeft(rtl);
            auto strip = StripLayoutProbe();
            const auto text = u"پیش‌نمایش ضبط Pause and preview this recording"_q;
            strip.labels(text);
            strip.show();
            for (const auto width : { 120, 320, 640 }) {
                strip.resize(width + 1, 48);
                strip.resize(width, 48);
                QApplication::processEvents();
                const auto controls = strip.controls();
                for (const auto control : controls) {
                    expect(control->width() == 0 || strip.rect().contains(control->geometry()),
                        "recording strip controls fit a narrow or wide parent");
                    const auto stock = dynamic_cast<Ui::LinkButton*>(control);
                    expect(stock && stock->accessibilityName() == text
                        && stock->accessibilityRole() == QAccessible::Link,
                        "recording strip uses stock accessible action links");
                }
                expect(rtl ? controls[1]->x() > controls[4]->x() : controls[1]->x() < controls[4]->x(),
                    "recording strip action order mirrors in RTL");
                strip.labels(text.repeated(3));
                for (const auto control : strip.controls()) {
                    expect(control->width() == 0 || strip.rect().contains(control->geometry()),
                        "live longer recording labels keep their allocated bounds");
                }
                strip.labels(text);
            }
            if (rtl) {
                auto image = QImage(strip.size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                strip.render(&image);
                expect(image.save(u"recording-strip-rtl.png"_q), "synthetic RTL strip rendering is saved");
            }
            auto bubble = BubbleControlsProbe();
            for (const auto side : { 64, 128, 160, 320 }) {
                bubble.resize(side, side);
                bubble.place();
                expect(bubble.rect().contains(bubble.pauseButton->geometry())
                    && bubble.rect().contains(bubble.cameraButton->geometry()),
                    "floating recorder controls stay inside their scaled bubble");
                expect(dynamic_cast<Ui::IconButton*>(bubble.pauseButton)
                    && dynamic_cast<Ui::IconButton*>(bubble.cameraButton),
                    "floating recorder uses stock icons instead of clipped translated text");
                expect(rtl ? bubble.pauseButton->x() > bubble.cameraButton->x()
                    : bubble.pauseButton->x() < bubble.cameraButton->x(),
                    "floating recorder actions mirror in RTL");
            }
        }
        style::SetRightToLeft(false);
        rows.clear();
        AddPlayback(&rows, &player, Kind::Voice, 5000);
        player.restart(-1);
        expect(Progress(rows)->accessibilityName() == u"Playback failed"_q, "asynchronous decoder errors are visible");
        rows.clear();
    }
    style::StopManager();
    std::cout << (checks - failures) << '/' << checks << " recording bin presentation checks passed\n";
    return failures ? 1 : 0;
}
