#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--output-dir', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
sources = repo / 'Telegram/SourceFiles'
build = args.build_dir.resolve()
work = args.output_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
records = []


def run(argv, **kwargs):
    result = subprocess.run(list(map(str, argv)), check=True, **kwargs)
    records.append({'command': list(map(str, argv)), 'exit': result.returncode})
    (work / 'inputs.json').write_text(json.dumps(records, indent=2) + '\n')
    return result


def definition(text, marker, body=False):
    start = text.index(marker)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[brace if body else start:end]


target = 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'
lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()
recipe = next(shlex.split(line) for line in lines if ' -c ' in line
    and shlex.split(line)[shlex.split(line).index('-c') + 1].endswith('/purple_config_file.cpp'))
cache = dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text().splitlines()
    if '=' in line and not line.startswith(('#', '//')))
original_sources = Path(cache['CMAKE_HOME_DIRECTORY:INTERNAL']) / 'Telegram/SourceFiles'
flags = []
i = 1
while i < len(recipe):
    flag = recipe[i]
    if flag == '-Xclang' and recipe[i + 1] in ('-include-pch', '-include'):
        i += 4
    elif flag in ('-c', '-o', '-MF', '-MT', '-include', '-include-pch'):
        i += 2
    elif flag in ('-MD', '-MMD') or flag.startswith(('-g', '-DLANG_KEYS_SUBSET=')):
        i += 1
    else:
        flags.append(flag)
        i += 1
(gen := work / 'gen').mkdir(exist_ok=True)
(work / 'lang.strings').write_bytes((repo / 'Telegram/Resources/langs/lang.strings').read_bytes()
    + b'\n' + (repo / 'Telegram/Resources/langs/purple.strings').read_bytes())
run([build / 'Telegram/codegen/codegen/lang/codegen_lang', '-o' + str(gen), work / 'lang.strings'])
(work / 'stdafx.hxx').write_text('#include "' + str(sources / 'stdafx.h') + '"\n')
flags = ['-I' + str(work), '-I' + str(gen), '-I' + str(sources),
    '-I' + str(repo / 'Telegram/ThirdParty/purple_core'), *flags,
    '-g0', '-include', str(work / 'stdafx.hxx')]

frontend = (sources / 'purple/purple_emoji_export.cpp').read_text()
settings = (sources / 'purple/purple_emoji_large_setting.cpp').read_text()
(work / 'emoji-large-metadata-handler.inc').write_text('\n'.join(definition(frontend, marker) for marker in [
    'void EmojiExporter::applySetTitle(', 'bool EmojiExporter::resolveSetTitle(',
    'void EmojiExporter::chooseDemand(', 'void EmojiExporter::holdDemandForMetadataWait(', 'void EmojiExporter::admitRequests(', 'void EmojiExporter::next(']))
resolve = definition(frontend, 'void EmojiExporter::resolve()')
(work / 'emoji-large-resolve-admission.inc').write_text(resolve[resolve.index('{') + 1:resolve.index('const auto token = _token;')])
(work / 'emoji-large-publication.inc').write_text(
    next(line for line in frontend.splitlines() if line.startswith('constexpr auto kDebounceMs')) + '\n'
    + definition(frontend, 'std::optional<qint64> NextEmojiExportRetry(') + '\n'
    + definition(frontend, 'void EmojiExporter::publish('))
(work / 'emoji-large-maintenance.inc').write_text(definition(frontend, 'void EmojiExporter::maintainRequests(')
    + '\nvoid EmojiExporter::capChanged() ' + definition(frontend,
        'EmojiLargeCacheCapValue() | rpl::on_next([=](qint64)', True) + '\n')
(work / 'emoji-large-recent.inc').write_text(definition(frontend,
    '[&](const Core::RecentEmoji &recent) -> std::optional<EmojiExportEntry>', True))
(work / 'emoji-large-preferences.inc').write_text('\n'.join(definition(settings, marker) for marker in [
    'qint64 EmojiLargeCacheCap()', 'void SetEmojiLargeCacheCap(qint64 bytes)',
    'rpl::producer<qint64> EmojiLargeCacheCapValue()']))


core_settings_header = (sources / 'core/core_settings.h').read_text()
(work / 'emoji-large-preference-api.inc').write_text('\n'.join(
    definition(core_settings_header, marker) for marker in [
        '\ttemplate <typename Type, typename Other>\n\tvoid writePref(',
        '\ttemplate <typename Type, typename Other = Type>\n\t[[nodiscard]] Type readPref(']))
(work / 'emoji-large-preference-storage.inc').write_text('\n'.join(
    definition(settings, marker) for marker in [
        'template <>\nstd::optional<qint64> Settings::readPrefImpl<qint64>(',
        'template <>\nvoid Settings::writePrefImpl<qint64>(']))


def compile(source):
    output = work / (source.stem + '.o')
    run([recipe[0], *(flags[:-2] if source.parent == sources / 'purple/tests' else flags), '-c', source, '-o', output, '-MD', '-MF', output.with_suffix('.d')])
    records[-1].update(sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        objectSha256=hashlib.sha256(output.read_bytes()).hexdigest())
    return output


base_units = ['purple_emoji_large', 'purple_emoji_large_store', 'purple_emoji_export_store',
    'purple_emoji_export_queue', 'purple_emoji_export_worker']
objects = [compile(sources / 'purple' / (name + '.cpp')) for name in base_units]
for name in ['purple_emoji_export', 'purple_emoji_large_setting']:
    compile(sources / 'purple' / (name + '.cpp'))

cap_symbols = run(['nm', '-gU', work / 'purple_emoji_large_setting.o'],
    stdout=subprocess.PIPE, text=True).stdout
cap_definitions = run(['c++filt'], input=cap_symbols, stdout=subprocess.PIPE, text=True).stdout
for member in ['readPrefImpl', 'writePrefImpl']:
    assert any(' T ' in line and 'Core::Settings::' + member + '<long long>' in line
        for line in cap_definitions.splitlines()), 'Missing integer preference definition: ' + member
records.append({'capPreferenceDefinitions': [line for line in cap_definitions.splitlines()
    if 'Core::Settings::' in line and 'PrefImpl<long long>' in line]})

link_line = next(line for line in subprocess.check_output(
    ['ninja', '-C', str(build), '-t', 'commands', 'Telegram'], text=True).splitlines()
    if 'libavformat' in line and 'libexternal_rlottie' in line)
link_recipe = shlex.split(link_line)
ffmpeg_libraries = [token for token in link_recipe if Path(token).name.startswith(
    ('libavformat.', 'libavcodec.', 'libswscale.', 'libswresample.', 'libavutil.'))]
assert len(ffmpeg_libraries) >= 5, 'FFmpeg link recipe missing'
ffmpeg = Path(next(token for token in ffmpeg_libraries if Path(token).name.startswith('libavformat.'))).parent.parent / 'bin/ffmpeg'
assert ffmpeg.is_file(), 'FFmpeg fixture encoder is unavailable'
run([ffmpeg, '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=100x60:d=0.08:r=25',
    '-c:v', 'libvpx-vp9', '-threads', '1', '-f', 'webm', work / 'native.webm'])
run([ffmpeg, '-nostdin', '-v', 'error', '-display_rotation:v:0', '90',
    '-i', work / 'native.webm', '-c', 'copy', '-f', 'mp4', work / 'rotated.mp4'])
probe = json.loads(subprocess.check_output([str(ffmpeg.with_name('ffprobe')), '-v', 'error',
    '-show_entries', 'stream_side_data=rotation', '-of', 'json', str(work / 'rotated.mp4')], text=True))
assert any(abs(item.get('rotation', 0)) == 90 for stream in probe['streams']
    for item in stream.get('side_data_list', [])), 'fixture lost its rotation metadata'
renderer = compile(sources / 'purple/purple_emoji_large_render.cpp')
render_objects = [renderer]
for unit in ['ffmpeg/ffmpeg_frame_generator.cpp', 'ffmpeg/ffmpeg_utility.cpp']:
    render_objects.append(compile(sources / unit))
for unit in ['lottie/lottie_frame_generator.cpp', 'lottie/lottie_common.cpp']:
    current = repo / 'Telegram/lib_lottie' / unit
    render_objects.append(compile(current if current.is_file() else original_sources.parent / 'lib_lottie' / unit))

prefix = [recipe[0]]
for flag in ('-arch', '-isysroot'):
    if flag in recipe:
        prefix += [flag, recipe[recipe.index(flag) + 1]]
for value in recipe:
    if value.startswith('-F'):
        prefix += [value, '-Wl,-rpath,' + value[2:]]
base = build / 'Telegram/lib_base/liblib_base.a'
environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
for value in flags:
    if value.endswith('/QtCore.framework/Headers'):
        plugins = Path(value).parents[2] / 'share/qt/plugins'
        if plugins.is_dir():
            environment['QT_PLUGIN_PATH'] = str(plugins)
for program in ['emoji_large_bounds_cases.cpp', 'emoji_large_policy_cases.cpp', 'emoji_large_store_cases.cpp', 'emoji_large_metadata_cases.cpp',
        'emoji_large_maintenance_cases.cpp', 'emoji_large_demand_cases.cpp', 'emoji_large_recent_cases.cpp', 'emoji_large_preferences_cases.cpp', 'emoji_large_render_cases.cpp']:
    name = Path(program).stem
    test = compile(sources / 'purple/tests' / program)
    binary = work / name
    extra = []
    if name == 'emoji_large_render_cases':
        extra = [*render_objects, build / 'Telegram/lib_ui/liblib_ui.a',
            build / 'cmake/external/rlottie/libexternal_rlottie_bundled.a', *ffmpeg_libraries, '-lz']
    run([*prefix, '-Wl,-dead_strip', test, *objects, *extra, base,
        '-framework', 'QtCore', '-framework', 'QtGui', '-framework', 'QtWidgets',
        '-framework', 'QtSvg', '-framework', 'Foundation', '-o', binary])
    run([binary, *([work / 'native.webm', work / 'rotated.mp4'] if name == 'emoji_large_render_cases' else [])], env=environment, timeout=180)
records.append({'literalSourcesSha256': {'exporter': hashlib.sha256(frontend.encode()).hexdigest(),
    'settings': hashlib.sha256(settings.encode()).hexdigest()}})
(work / 'inputs.json').write_text(json.dumps(records, indent=2) + '\n')
print('EMOJI LARGE native suites PASS')
