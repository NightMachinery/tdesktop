#!/usr/bin/env python3
# Compiles one focused program under Telegram/SourceFiles/purple/tests, together
# with the production units it exercises, with the Debug recipe of an existing
# build, and runs it. Units listed here are compiled from this tree; the build
# supplies only its libraries and objects named under "objects". The recipe keeps
# the build's include roots, which its precompiled header was made from; under
# purple/run_host_gates.py they see the tree's version of every file first.
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

CORE = 'purple_core'
CASES = {
    'peek-file': {
        'program': 'peek_file_cases.cpp',
        'units': ['purple/purple_config_file.cpp'],
        'frameworks': ['QtCore'],
    },
    'peek-admission': {
        'program': 'peek_admission_cases.cpp',
        'units': ['purple/purple_account_config.cpp', 'purple/purple_config_file.cpp', 'purple/purple_wait_gate.cpp', CORE],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets'],
    },
    'account-routing-file': {
        'program': 'account_routing_file_cases.cpp',
        'units': ['purple/purple_account_config.cpp', 'purple/purple_config_file.cpp', 'purple/purple_sync_config_history.cpp',
            'purple/purple_sync_local_store.cpp', 'purple/purple_sync_account_recovery.cpp', 'purple/purple_sync_config_review.cpp', CORE],
        'frameworks': ['QtCore'],
    },
    'copy-article': {
        # The real ordinary formatter and article writer over the real RichPage
        # model, lib_ui text and the IV inline-object and link codecs.
        'program': 'copy_article_cases.cpp',
        'units': ['purple/purple_article_markdown.cpp', 'purple/purple_copy.cpp'],
        'objects': ['Telegram/CMakeFiles/td_iv.dir/SourceFiles/iv/markdown/iv_markdown_prepare_serialize.cpp.o',
            'Telegram/CMakeFiles/Telegram.dir/SourceFiles/iv/iv_rich_page.cpp.o',
            'Telegram/CMakeFiles/Telegram.dir/SourceFiles/iv/iv_rich_page_quote.cpp.o'],
        'libraries': ['Telegram/lib_ui/liblib_ui.a', 'Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'Foundation'],
        'link': ['-lobjc'],
    },
    'webp-paste': {
        # The marked-WebP reader and both paste routes, with the attach,
        # sticker and editor pieces they rely on taken verbatim from production
        # (see slices below). The program writes only to a pasteboard it creates
        # with a unique name, never to the general one.
        'program': 'webp_paste_cases.mm',
        'units': ['purple/purple_webp_paste.cpp', 'purple/purple_webp_paste_mac.mm', 'purple/purple_webp_paste_route.cpp',
            'core:purple/purple_sync_json.cpp', 'slice:attach-lifetime', 'slice:sticker-dimensions', 'slice:photo-lifetime'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'QtNetwork', 'AppKit'],
        'arguments': ['{fixtures}/emoji.webp', '{fixtures}/emoji.png'],
    },
    'webp-paste-flow': {
        'program': 'webp_paste_flow_cases.cpp',
        'units': ['purple/purple_webp_paste.cpp', 'purple/purple_webp_paste_mac.mm', 'purple/purple_webp_paste_route.cpp',
            'core:purple/purple_sync_json.cpp', 'slice:attach-lifetime', 'slice:sticker-dimensions', 'slice:photo-lifetime',
            'ui/chat/attach/attach_send_files_way.cpp'],
        'generated': ['webp-paste-flow.inc'],
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'QtNetwork', 'AppKit'],
        'arguments': ['{fixtures}/emoji.webp'],
    },
    'sync-diag': {
        'program': 'sync_diag_cases.cpp',
        'units': ['purple/purple_sync_diag.cpp'],
        'frameworks': ['QtCore'],
    },
    'sync-magic-chat': {
        'program': 'sync_magic_chat_cases.cpp',
        'units': ['purple/purple_sync_magic_chat.cpp', 'purple/purple_sync_diag.cpp', CORE],
        'frameworks': ['QtCore'],
    },
    'storage-passcode': {
        # The Persian-keyboard passcode decision that storage_domain.cpp's
        # derivation hook makes, over the real purple-core mapper.
        'program': 'storage_passcode_cases.cpp',
        'units': ['purple/purple_storage_passcode.cpp', 'core:purple/purple_passcode.cpp'],
        'frameworks': ['QtCore'],
    },
    'emoji-export': {
        'program': 'emoji_export_cases.cpp',
        'units': ['purple/purple_emoji_export_store.cpp', 'purple/purple_emoji_large.cpp', 'purple/purple_emoji_large_store.cpp', 'purple/purple_emoji_export_queue.cpp', 'purple/purple_emoji_export_worker.cpp'],
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'Foundation'],
        'link': ['-lobjc'],
    },
    'emoji-export-async': {
        'program': 'emoji_export_async_cases.cpp',
        'units': ['purple/purple_emoji_export_store.cpp', 'purple/purple_emoji_large.cpp', 'purple/purple_emoji_large_store.cpp', 'purple/purple_emoji_export_queue.cpp', 'purple/purple_emoji_export_worker.cpp'],
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'Foundation'],
        'link': ['-lobjc'],
    },
    'emoji-export-shutdown': {
        # The worker is compiled with its condition variable replaced by the
        # program's instrumented one, as the shutdown cases require.
        'program': 'emoji_export_shutdown_cases.cpp',
        'units': ['purple/purple_emoji_export_store.cpp', 'purple/purple_emoji_large.cpp', 'purple/purple_emoji_large_store.cpp', 'purple/purple_emoji_export_worker.cpp'],
        'shim': {'header': 'purple/purple_emoji_export_worker.h', 'prefix': '#include "purple/tests/emoji_export_shutdown_cv.h"\n',
            'replace': ['std::condition_variable _ready;', 'Purple::Tests::EmojiShutdownCondition _ready;']},
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'Foundation'],
        'link': ['-lobjc'],
    },
    'media-restart': {
        'program': 'media_restart_cases.cpp',
        'units': ['purple/purple_media_rules.cpp'],
        'frameworks': ['QtCore'],
    },
    'media-restart-fifo': {
        'program': 'media_restart_fifo_cases.cpp',
        'units': ['purple/purple_media_restart.cpp', 'purple/purple_media_rules.cpp', 'data/data_audio_msg_id.cpp',
            '../lib_ui/ui/click_handler.cpp', '../lib_crl/crl/dispatch/crl_dispatch_async.cpp'],
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'Foundation', 'CoreFoundation'],
        'link': ['-lobjc'],
    },
    'media-size': {
        'program': 'media_size_cases.cpp',
        'units': ['purple/purple_media_rules.cpp'],
        'frameworks': ['QtCore'],
    },
    'recording': {
        # F9's seek step and clamping, and F10's encrypted files, voice
        # mirror, manifest and voice recovery over purple-core's Ogg finaliser.
        'program': 'recording_cases.cpp',
        'units': ['purple/purple_seek_rules.cpp', 'purple/purple_recording_crypt.cpp', 'purple/purple_recording_mirror.cpp',
            'purple/purple_recording_registry.cpp', 'purple/purple_recording_recovery.cpp',
            'core:purple/purple_ogg_repair.cpp', 'core:purple/purple_recording_file.cpp'],
        'build_libraries': ['libcrypto.a'],
        'frameworks': ['QtCore'],
    },
    'recording-bin': {
        'program': 'recording_bin_cases.cpp',
        'syntax_units': ['purple/purple_media_seek.cpp', 'purple/purple_recording_dock.cpp', 'purple/purple_round_recording.cpp'],
        'generated': ['recording-bin.inc'],
        'units': ['ui/text/format_values.cpp', '../lib_crl/crl/dispatch/crl_dispatch_async.cpp'],
        'build_libraries': ['libcrypto.a', 'libxxhash.dylib'],
        'libraries': ['Telegram/lib_ui/liblib_ui.a', 'Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'QtGui', 'QtWidgets', 'QtSvg', 'QtTest', 'Foundation', 'AppKit', 'Metal'],
        'link': ['-lobjc'],
    },
    'recording-store-startup': {
        'program': 'recording_store_startup_cases.cpp',
        'units': [],
        'generated': ['recording-store-startup.inc'],
        'libraries': ['Telegram/lib_base/liblib_base.a'],
        'frameworks': ['QtCore', 'Foundation'],
    },
    'recording-round': {
        # F10's round copy, remux, repair rebuild and AAC measure over the
        # build's own FFmpeg, on a round encoded at test time.
        'program': 'recording_round_cases.cpp',
        'generated': ['recording-round-encoder.inc'],
        'units': ['purple/purple_recording_round_media.cpp', 'purple/purple_recording_crypt.cpp',
            'purple/purple_recording_recovery.cpp', 'purple/purple_recording_registry.cpp',
            'core:purple/purple_mp4_repair.cpp', 'core:purple/purple_round_journal.cpp',
            'core:purple/purple_ogg_repair.cpp', 'core:purple/purple_recording_file.cpp'],
        'build_libraries': ['libavformat.a', 'libavcodec.a', 'libswresample.a', 'libavutil.a', '?libopenh264.a', 'libcrypto.a'],
        'frameworks': ['QtCore', 'VideoToolbox', 'AudioToolbox', 'CoreMedia', 'CoreVideo', 'CoreFoundation', 'CoreServices', 'Security'],
        'link': ['-lz', '-lbz2', '-liconv', '-lc++'],
    },
    'voice-queue': {
        # The confined walk and the manual-play tracker over the real
        # purple-core rule.
        'program': 'voice_queue_cases.cpp',
        'units': ['purple/purple_voice_queue_rules.cpp', 'core:purple/purple_playback.cpp'],
        'frameworks': ['QtCore'],
    },
}



def function(text, signature):
    # The definition that starts at signature, through its matching brace.
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def slices(sources):
    # Production definitions compiled on their own, so that the WebP cases need
    # neither the whole attach and editor units nor what those pull in.
    attach = (sources / 'ui/chat/attach/attach_prepare.cpp').read_text()
    start = attach.index('PreparedFile::PreparedFile(const QString &path)')
    types = (sources / 'data/data_types.cpp').read_text()
    editor = (sources / 'editor/photo_editor_common.cpp').read_text()
    return {
        'attach-lifetime': '#include "ui/chat/attach/attach_prepare.h"\n#include <QtCore/QFileInfo>\nnamespace Ui {\n'
            + attach[start:attach.index('bool PreparedFile::canBeInAlbumType', start)] + '\n}\n',
        'sticker-dimensions': '#include "media/media_common.h"\n' + function(types, 'bool GoodStickerDimensions(') + '\n',
        'photo-lifetime': '#include "editor/photo_editor_common.h"\n#include "editor/scene/scene.h"\nnamespace Editor {\n'
            + function(editor, 'PhotoModifications::~PhotoModifications()') + '\n}\n',
    }


def recording_startup_include(sources):
    store = (sources / 'purple/purple_recording_store.cpp').read_text()
    return '\n'.join(function(store, signature) for signature in (
        'void Refresh()', 'void StartRecordingStore()', 'bool RecordingStoreAvailable()'))


def recording_encoder_include(sources):
    native = (sources / 'ui/controls/round_video_recorder.cpp').read_text()
    body = function(native, 'bool RoundVideoRecorder::Private::initVideo()')
    start = body.index('\t_videoCodec->codec_id =')
    end = body.index('\n\tauto error =', start)
    setup = body[start:end].replace('_videoCodec.get()', 'video').replace('_videoCodec', 'video')
    return ('bool ConfigureNativeVideo(AVCodecContext *video, const AVCodec *videoCodec) {\n'
        + 'constexpr auto kVideoBitRate = 2 * 1024 * 1024;\n' + setup + '\nreturn true;\n}\n')


def flow_include(sources):
    # The confirmation-flow pieces webp_paste_flow_cases.cpp checks, each taken
    # from its production file, with the box and peer around them replaced by
    # the probes it needs.
    read = lambda relative: (sources / relative).read_text()
    attach = read('ui/chat/attach/attach_prepare.cpp')
    ui = 'namespace Ui {\nconstexpr auto kMaxAlbumCount = 10;\n' + function(attach, 'struct GroupRange {') + ';\n'
    for signature in ['[[nodiscard]] AlbumType GroupTypeForFile(', '[[nodiscard]] bool SendsAnimationAsGif(',
            'bool PreparedFile::sendsVideoAsGif()', '[[nodiscard]] std::vector<GroupRange> GroupRanges(',
            'bool PreparedList::canBeSentInSlowmode()', 'bool PreparedList::canBeSentInSlowmodeWith(', 'std::vector<PreparedGroup> DivideByGroups(',
            'bool PreparedFile::isSticker()', 'AlbumType PreparedFile::albumType(', 'bool CanBeInAlbumType(',
            'bool PreparedFile::canBeInAlbumType(', 'bool PreparedList::canAddCaption(']:
        ui += function(attach, signature) + '\n'
    ui += '}\n'
    core = 'namespace Core {\n' + ''.join(function(read('core/mime_type.cpp'), signature) + '\n'
        for signature in ['bool IsMimeStickerAnimated(', 'bool IsMimeSticker(']) + '}\n'
    # The probes never give an image a paint scene, so upstream's animated
    # sticker composition is cut out of the slice with the editor scene it needs.
    apply = function(read('storage/storage_media_prepare.cpp'), 'bool ApplyModifications(')
    animated = apply.index('\t\tconst auto &scene = image->modifications.paint;')
    apply = apply[:animated] + apply[apply.index('\t\timage->data = Editor::ImageModified(', animated):]
    apply = apply.replace(function(apply, '\t\tif (file.animationJob) {'), '', 1)
    storage = ('namespace Storage {using Ui::PreparedList; using Ui::PreparedFile; using Image=Ui::PreparedFileInformation::Image;\n'
        + apply + '\n}\n')
    editor = """namespace Editor {
bool PhotoModifications::empty() const {return !angle && !flipped && !crop.isValid() && !paint;}
PhotoModifications::operator bool() const {return !empty();}
QImage ImageModified(QImage image,const PhotoModifications &mods) {return mods.crop.isValid()?image.copy(mods.crop):image;}
}
"""
    body = function(read('editor/photo_editor_layer_widget.cpp'), 'void OpenWithPreparedFile(')
    prefix = body[body.index('\tusing ImageInfo'):body.index('\n\tsideLimit =')]
    probes = """bool ProbeEditorAdmission(Ui::PreparedFile &input) {
 auto admitted=true; auto file=&input;
 const auto doneCallback=[&](bool ok) {admitted=ok;};
 const auto run=[&] {
""" + prefix + """
}; run(); return admitted;
}
"""
    box = read('boxes/send_files_box.cpp')
    start = box.index('const auto canOpenPhotoEditor =')
    condition = box[start:box.index(';', start) + 1].replace('_sendWay.current().sendImagesAsPhotos()', 'compress')
    probes += 'bool ProbeContextEditor(const Ui::PreparedFile &file,bool compress) {\nconst auto compressed=compress;\n' + condition + ' return canOpenPhotoEditor;\n}\n'
    preview = function(read('ui/chat/attach/attach_abstract_single_media_preview.cpp'), 'bool AbstractSingleMediaPreview::isPhoto() const')
    background = function(read('ui/chat/attach/attach_single_media_preview.cpp'), 'bool SingleMediaPreview::drawBackground() const')
    probes += 'bool ProbePreviewBackground(bool _sticker) ' + background[background.index('{'):] + '\n'
    probes += ('bool ProbePreviewPhoto(bool background,bool ready,bool _animated) '
        + preview[preview.index('{'):].replace('drawBackground()', 'background').replace('isAnimatedPreviewReady()', 'ready') + '\n')
    permission = read('data/data_chat_participant_status.cpp')
    end = permission.index('&& bundle.groups.size() > 1)')
    start = permission.rfind('if (', 0, end)
    condition = permission[start + 4:end + len('&& bundle.groups.size() > 1')].replace('!ignoreRestrictions', 'true').replace('peer->slowmodeApplied()', 'true')
    probes += 'bool ProbeFinalSlowmode(const Ui::PreparedBundle &bundle) {return ' + condition + ';}\n'
    probes += """struct ProbeWay {
 Ui::SendFilesWay value;
 Ui::SendFilesWay current() const {return value;}
 void operator=(Ui::SendFilesWay next) {value=next;}
};
struct ProbeConfirmation {
 Ui::PreparedList _list;
 SendFilesLimits _limits=SendFilesAllow::OnlyOne;
 ProbeWay _sendWay;
 bool checkWithWay(Ui::SendFilesWay) {return true;}
 void addFile(Ui::PreparedFile &&file);
};
"""
    probes += function(box, 'void SendFilesBox::addFile(').replace('SendFilesBox::addFile', 'ProbeConfirmation::addFile') + '\n'
    return '#include "core/mime_type.h"\n#include "editor/photo_editor_common.h"\n#include "media/media_video_encode.h"\n' + core + ui + editor + storage + probes


# A synthetic sticker for the WebP cases: 512 by 512 with a transparent
# border, written as WebP and PNG by Qt's own image plugins.
FIXTURE = r"""#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
int main(int argc, char **argv) {
	auto app = QGuiApplication(argc, argv);
	auto image = QImage(512, 512, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto painter = QPainter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setBrush(QColor(128, 64, 192, 220));
	painter.setPen(Qt::NoPen);
	painter.drawEllipse(QRectF(48, 48, 416, 416));
	painter.end();
	return image.save(QString::fromLocal8Bit(argv[1]), "WEBP", 100) && image.save(QString::fromLocal8Bit(argv[2]), "PNG") ? 0 : 1;
}
"""


def recording_controls_include(sources):
    dock = (sources / 'purple/purple_recording_dock.cpp').read_text()
    strip = function(dock, 'class Strip final')
    stock = 'Ui::LinkButton _title, _pause, _preview, _send, _discard;' in strip
    fields = ('Ui::LinkButton _title, _pause, _preview, _send, _discard;' if stock
        else 'QLabel _title; QPushButton _pause, _preview, _send, _discard;')
    names = ['_title', '_pause', '_preview', '_send', '_discard']
    init = ', '.join(name + '(this' + (', QString()' if stock else '') + ')' for name in names)
    layout = function(strip, 'void resizeEvent(')
    if 'void layoutButtons()' in strip:
        layout += '\n' + function(dock, 'void Strip::layoutButtons(').replace('Strip::layoutButtons', 'layoutButtons')
    refresh = function(strip, 'void refresh()')
    refresh_layout = 'layoutButtons();' if 'layoutButtons();' in refresh else ''
    disable = refresh[refresh.index('for (const auto button : { &_pause, &_preview, &_send, &_discard })'):
        refresh.index('\n\t\tlayoutButtons();')]
    wiring = '\n'.join(line.strip() for line in strip.splitlines() if '.setClickedCallback(' in line)
    paint = function(strip, 'paintRequest() | rpl::on_next([=] ') + ', lifetime());'
    enable_helper = (function(dock, 'void SetStripActionEnabled(') + '\n'
        if 'void SetStripActionEnabled(' in dock else '')

    result = (enable_helper + 'class StripLayoutProbe final : public Ui::RpWidget {\npublic:\n'
        'StripLayoutProbe() : RpWidget(nullptr), ' + init + ' { ' + wiring + paint + ' }\n'
        'void applyInputState() { const auto &dock = TheDock(); ' + disable + ' }\n'
        'void labels(const QString &text) { ' + ' '.join(name + '.setText(text);' for name in names)
        + refresh_layout + ' }\n'
        'std::array<QWidget*, 5> controls() { return { &_title, &_pause, &_preview, &_send, &_discard }; }\n'
        + layout + '\nprivate:\n' + fields + '\n};\n')
    round_source = (sources / 'purple/purple_round_recording.cpp').read_text()
    place = function(round_source, 'void Place(')
    block = function(place, 'if (state->pauseButton)')
    icon = 'Ui::IconButton *pauseButton' in round_source
    button_type = 'Ui::IconButton' if icon else 'QPushButton'
    construct = ('new Ui::IconButton(this, st::defaultIconButton)' if icon
        else 'new QPushButton(QString(), this)')
    result += ('class BubbleControlsProbe final : public Ui::RpWidget {\npublic:\n'
        'BubbleControlsProbe() : RpWidget(nullptr), pauseButton(' + construct
        + '), cameraButton(' + construct + ') {}\n'
        'void place() { const auto side = width(); const auto state = this; ' + block + ' }\n'
        + button_type + ' *pauseButton, *cameraButton;\n};\n')
    return result

def recording_bin_include(sources):
    text = (sources / 'purple/purple_recording_bin_box.cpp').read_text()
    result = text[text.index('class BinActions final'):text.index('[[nodiscard]] QString MarkText(')]
    result += text[text.index('Ui::LinkButton *BinActions::add('):text.index('void AddItem(')]
    seek = (sources / 'purple/purple_media_seek.cpp').read_text()
    result += function(seek, '[[nodiscard]] QString SeekText(') + '\n'
    result += recording_controls_include(sources)
    return result.replace('st::boxLabel', 'st::defaultFlatLabel').replace(
        'st::boxRowPadding', 'QMargins(8, 8, 8, 8)')


parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output-dir', type=Path)
parser.add_argument('--case', choices=sorted(CASES), required=True)
args = parser.parse_args()
case = CASES[args.case]
repo = Path(__file__).resolve().parent.parent
sources = repo / 'Telegram/SourceFiles'
build = args.build_dir.resolve()
owned = tempfile.TemporaryDirectory(prefix='purple-cases.') if not args.output_dir else None
work = args.output_dir.resolve() if args.output_dir else Path(owned.name)
work.mkdir(parents=True, exist_ok=True)

target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
recipes = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
recipe = next(shlex.split(line) for line in recipes.splitlines() if ' -c ' in line
    and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_config_file.cpp'))


def flags_of(recipe):
    result, skip = [], False
    for value in recipe[1:]:
        if skip:
            skip = False
        elif value in ('-o', '-MF', '-MT', '-c'):
            skip = True
        elif value not in ('-MD', '-MMD') and not value.startswith(('-g', '-DLANG_KEYS_SUBSET=')):
            result.append(value)
    return result


flags = {'.cpp': flags_of(recipe)}
if any(unit.endswith('.mm') for unit in [*case['units'], case['program']]):
    # Objective-C++ units take the recipe, and precompiled header, of one.
    native = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_webp_paste_mac.mm.o'
    lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', native], text=True).splitlines()
    flags['.mm'] = flags_of(next(shlex.split(line) for line in lines if ' -c ' in line
        and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_webp_paste_mac.mm')))
include = []
if case.get('shim'):
    shim = case['shim']
    header = (sources / shim['header']).read_text()
    assert header.count(shim['replace'][0]) == 1, shim['header']
    (work / 'shim' / shim['header']).parent.mkdir(parents=True, exist_ok=True)
    (work / 'shim' / shim['header']).write_text(shim['prefix'] + header.replace(*shim['replace']))
    include = ['-I' + str(work / 'shim'), *include]

if case['program'].endswith('.mm'):
    # The cases may touch only a pasteboard of their own: the program creates
    # one with a unique name, and runs under a QCoreApplication, for which the
    # production reader returns before it reaches the general pasteboard.
    program = (sources / 'purple/tests' / case['program']).read_text()
    assert 'generalPasteboard' not in program and 'pasteboardWithUniqueName' in program, case['program']
    assert 'QCoreApplication(' in program and 'QGuiApplication(' not in program and 'QApplication(' not in program, case['program']
    reader = (sources / 'purple/purple_webp_paste_mac.mm').read_text()
    guard = reader[reader.index('ReadMacMarkedWebpPaste('):]
    assert guard.index('qobject_cast<QGuiApplication*>') < guard.index('generalPasteboard'), 'purple_webp_paste_mac.mm'
pieces = slices(sources) if any(unit.startswith('slice:') for unit in case['units']) else {}
for name in case.get('generated', []):
    generate = {
        'recording-bin.inc': recording_bin_include,
        'recording-store-startup.inc': recording_startup_include,
        'recording-round-encoder.inc': recording_encoder_include,
    }.get(name, flow_include)
    (work / name).write_text(generate(sources))
    include.append('-I' + str(work))
units = []
for unit in [*case['units'], 'purple/tests/' + case['program']]:
    if unit == CORE:
        units += sorted((repo / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp'))
    elif unit.startswith('core:'):
        units.append(repo / 'Telegram/ThirdParty/purple_core' / unit[5:])
    elif unit.startswith('slice:'):
        units.append(work / (unit[6:] + '.cpp'))
        units[-1].write_text(pieces[unit[6:]])
    else:
        units.append(sources / unit)
objects = []
with (work / 'build.log').open('w') as log:
    for unit in case.get('syntax_units', []):
        command = [recipe[0], *include, *flags['.cpp'], '-fsyntax-only', str(sources / unit),
            '-MD', '-MF', str(work / (Path(unit).stem + '-syntax.d'))]
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    for source in units:
        objects.append(work / (source.stem + '.o'))
        toml = ['-I' + str(repo / 'Telegram/ThirdParty/purple_core/tomlplusplus')] if 'purple_core' in source.parts else []
        command = [recipe[0], *include, *toml, *flags.get(source.suffix, flags['.cpp']), '-g0',
            '-c', str(source), '-o', str(objects[-1]), '-MD', '-MF', str(objects[-1].with_suffix('.d'))]
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    link = [recipe[0]]
    for flag in ['-arch', '-isysroot']:
        if flag in recipe:
            link += [flag, recipe[recipe.index(flag) + 1]]
    for value in recipe:
        if value.startswith('-F'):
            link += [value, '-Wl,-rpath,' + value[2:]]
    binary = work / Path(case['program']).stem
    found = []
    if case.get('build_libraries'):
        manifest = (build / 'build.ninja').read_text(errors='replace')
        for name in case['build_libraries']:
            optional, name = name.startswith('?'), name.lstrip('?')
            names = {name, name[:-2] + '.dylib'} if name.endswith('.a') else {name}
            hits = sorted({value for value in manifest.split() if Path(value).name in names},
                key=lambda value: (not value.endswith('/' + name), value))
            assert hits or optional, 'no ' + name + ' in build.ninja'
            found += hits[:1]
    link += ['-Wl,-dead_strip', *map(str, objects), *[str(build / name) for name in case.get('objects', []) + case.get('libraries', [])], *found,
        *[value for name in case['frameworks'] for value in ('-framework', name)], *case.get('link', []), '-o', str(binary)]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    if any('{fixtures}' in value for value in case.get('arguments', [])):
        (work / 'fixtures').mkdir(exist_ok=True)
        (work / 'fixture.cpp').write_text(FIXTURE)
        subprocess.run([recipe[0], *flags['.cpp'], '-g0', '-c', str(work / 'fixture.cpp'), '-o', str(work / 'fixture.o')],
            cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([*link[:link.index('-Wl,-dead_strip')], str(work / 'fixture.o'), '-framework', 'QtCore', '-framework', 'QtGui',
            '-o', str(work / 'fixture')], cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(work / 'fixture'), str(work / 'fixtures/emoji.webp'), str(work / 'fixtures/emoji.png')],
            cwd=work, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'), stdout=log, stderr=subprocess.STDOUT, check=True)
(work / 'inputs.json').write_text(json.dumps({'case': args.case, 'link': link,
    'syntaxSources': {unit: hashlib.sha256((sources / unit).read_bytes()).hexdigest() for unit in case.get('syntax_units', [])},
    'sources': {str(path.relative_to(repo) if repo in path.parents else path.relative_to(work)): hashlib.sha256(path.read_bytes()).hexdigest() for path in units}}, indent=2) + '\n')
arguments = [value.replace('{fixtures}', str(work / 'fixtures')) for value in case.get('arguments', [])]
result = subprocess.run([str(binary), *arguments], cwd=work, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
print(args.case + ': exit ' + str(result.returncode))
sys.exit(result.returncode)
