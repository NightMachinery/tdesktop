"""Exercise the real round bubble route and native pause publication contract."""
import pathlib, re, subprocess

def run(root, output):
    source = root / 'Telegram/SourceFiles'
    native = (source / 'history/view/controls/history_view_voice_record_bar.cpp').read_text()
    dock = (source / 'purple/purple_recording_dock.cpp').read_text()
    match = re.search(r'Purple::BindRoundRecorder\(_videoRecorder.get\(\), this, \[=\] \{ (.*?) \}, _show\);', native)
    assert match
    callback = match.group(1).replace('_lock->clicked(Qt::NoModifier, Qt::LeftButton)', 'nativeToggle()')
    route = ''
    if 'Purple::ToggleRoundRecordingPause(this)' in callback:
        start = dock.index('void ToggleRoundRecordingPause(')
        end = dock.index('\n}\n', start) + 2
        route = dock[start:end].replace('not_null<HistoryView::Controls::VoiceRecordBar*>', 'Bar*')
    discard = dock[dock.index('void Discard()'):dock.index('void Discard()') + 500]
    prompt = re.search(r'\.text = (.*?),\n', discard, re.S).group(1)
    text = r'''#include "purple/purple_recording_dock_model.h"
#include <cassert>
#include <cstdio>
#include <optional>
struct Bar;
namespace { Bar *roundBar; int pauseAdapterCalls; void Pause(); }
struct Bar {
 bool floating = true, listen = true;
 int resumes = 0;
 void nativeToggle() { if (Purple::RecordingComposeActive(floating, listen)) { listen=false; ++resumes; } else listen=true; }
 void purpleTogglePause() { nativeToggle(); }
 void bubble();
};
namespace {
struct Ready { bool video = false; };
std::optional<Ready> ready;
struct Dock { Bar *roundBar; std::optional<Ready> ready; };
Dock TheDock() { return {roundBar, ready}; }
void Pause() { ++pauseAdapterCalls; roundBar->floating=false; roundBar->purpleTogglePause(); roundBar->floating=true; }
}
namespace tr {
int lng_record_lock_cancel_sure() { return 1; }
int lng_record_lock_cancel_sure_round() { return 2; }
int lng_record_listen_cancel_sure() { return 3; }
int lng_record_listen_cancel_sure_round() { return 4; }
}
int DiscardPrompt() { return @DISCARD@; }
namespace Purple { ROUTE }
void Bar::bubble() { CALLBACK }
int main() {
 Bar bar; roundBar=&bar; bar.bubble();
 assert(bar.resumes==1); assert(!bar.listen); assert(bar.floating); assert(pauseAdapterCalls==1);
 assert(DiscardPrompt()==2); roundBar=nullptr; assert(DiscardPrompt()==1);
 ready=Ready{true}; assert(DiscardPrompt()==4); ready=Ready{false}; assert(DiscardPrompt()==3);
 std::puts("ROUND_RESUME_CHECKS=4 ROUND_DISCARD_PROMPT_CHECKS=4");
}
'''.replace('ROUTE', route).replace('CALLBACK', callback).replace('@DISCARD@', prompt)
    fixture = output / 'round-resume.cpp'; fixture.write_text(text)
    exe = output / 'round-resume'
    subprocess.run(['clang++', '-std=c++20', '-I'+str(source), str(fixture), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    assert 'return _paused.value();' in native.split('VoiceRecordBar::purplePauseChanges() const',1)[1].split('}',1)[0]
    watch = dock.split('bar->purplePauseChanges()',1)[1].split('dock.capture);',1)[0]
    assert 'bool paused' in watch and 'generation(), paused)' in watch
    print('ROUND_PAUSE_PUBLICATION_CHECKS=2')
