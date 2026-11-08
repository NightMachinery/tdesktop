#include "editor/video/video_segment_player.h"
#include "purple/purple_recording_registry.h"
#include "ui/integration.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
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
                    const auto actions = rows.add(object_ptr<Ui::RpWidget>(&rows));
                    const auto play = AddAction(actions, item ? u"Play"_q : u"Stop"_q, [&] { ++clicks; });
                    const auto save = AddAction(actions, u"Save"_q, [] {});
                    actions->resize(200, 40);
                    save->move(70, 0);
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
        AddPlayback(&rows, &player, Kind::Voice, 5000);
        player.restart(-1);
        expect(Progress(rows)->accessibilityName() == u"Playback failed"_q, "asynchronous decoder errors are visible");
        rows.clear();
    }
    style::StopManager();
    std::cout << (checks - failures) << '/' << checks << " recording bin presentation checks passed\n";
    return failures ? 1 : 0;
}
