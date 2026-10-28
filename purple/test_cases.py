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
            'Telegram/CMakeFiles/Telegram.dir/SourceFiles/iv/iv_rich_page.cpp.o'],
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


def flow_include(sources):
    # The confirmation-flow pieces webp_paste_flow_cases.cpp checks, each taken
    # from its production file, with the box and peer around them replaced by
    # the probes it needs.
    read = lambda relative: (sources / relative).read_text()
    attach = read('ui/chat/attach/attach_prepare.cpp')
    ui = 'namespace Ui {\nconstexpr auto kMaxAlbumCount = 10;\n' + function(attach, 'struct GroupRange {') + ';\n'
    for signature in ['[[nodiscard]] AlbumType GroupTypeForFile(', '[[nodiscard]] std::vector<GroupRange> GroupRanges(',
            'bool PreparedList::canBeSentInSlowmode()', 'bool PreparedList::canBeSentInSlowmodeWith(', 'std::vector<PreparedGroup> DivideByGroups(',
            'bool PreparedFile::isSticker()', 'AlbumType PreparedFile::albumType(', 'bool CanBeInAlbumType(',
            'bool PreparedFile::canBeInAlbumType(', 'bool PreparedList::canAddCaption(']:
        ui += function(attach, signature) + '\n'
    ui += '}\n'
    core = 'namespace Core {\n' + ''.join(function(read('core/mime_type.cpp'), signature) + '\n'
        for signature in ['bool IsMimeStickerAnimated(', 'bool IsMimeSticker(']) + '}\n'
    storage = ('namespace Storage {using Ui::PreparedList; using Ui::PreparedFile; using Image=Ui::PreparedFileInformation::Image;\n'
        + function(read('storage/storage_media_prepare.cpp'), 'bool ApplyModifications(') + '\n}\n')
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
    probes += 'bool ProbeContextEditor(const Ui::PreparedFile &file,bool compress) {\n' + condition + ' return canOpenPhotoEditor;\n}\n'
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
    return '#include "core/mime_type.h"\n#include "editor/photo_editor_common.h"\n' + core + ui + editor + storage + probes


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
    (work / name).write_text(flow_include(sources))
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
    link += ['-Wl,-dead_strip', *map(str, objects), *[str(build / name) for name in case.get('objects', []) + case.get('libraries', [])],
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
    'sources': {str(path.relative_to(repo) if repo in path.parents else path.relative_to(work)): hashlib.sha256(path.read_bytes()).hexdigest() for path in units}}, indent=2) + '\n')
arguments = [value.replace('{fixtures}', str(work / 'fixtures')) for value in case.get('arguments', [])]
result = subprocess.run([str(binary), *arguments], cwd=work, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
print(args.case + ': exit ' + str(result.returncode))
sys.exit(result.returncode)
