#!/usr/bin/env python3
# Runs every Purple Desktop host gate listed in purple/host_gates.json against
# one source tree and writes results.json, one row per configuration. See
# "Running every host gate" in docs/purple/agents.md.
import argparse
import collections
import concurrent.futures
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import time

script = Path(__file__).resolve()
COMPILERS = ['c++', 'g++', 'clang++', 'clang', 'cc', 'gcc']
HEADERS = ('.h', '.hh', '.hpp', '.hxx', '.inc', '.ipp')
GENERATOR_INPUTS = ('.style', '.palette', '.strings', '.tl')
PCH = 'Telegram/CMakeFiles/Telegram.dir/cmake_pch.hxx.pch'
SOURCES = ('.c', '.cc', '.cpp', '.cxx', '.m', '.mm')
VALUED = ('-o', '-MF', '-MT', '-MQ', '-include', '-imacros', '-x', '-isysroot', '-arch', '-I', '-isystem',
    '-iquote', '-F', '-iframework', '-Xclang', '-Xlinker', '-L', '-framework')


def under(value, root):
    return root is not None and (value == root or value.startswith(root + '/'))


def exec_compile(shim_file, argv):
    # Every compiler or linker call of a gate lands here: one at a time, inside
    # the compile wrapper, with the shim directory off PATH so nothing re-enters.
    # A call that compiles also leaves a copy of its dependency file, so that the
    # runner can see every file the compiler actually read.
    shim = json.loads(Path(shim_file).read_text())
    path = [p for p in os.environ.get('PATH', '').split(os.pathsep) if p and p != shim['bin']]
    env = dict(os.environ, PATH=os.pathsep.join(path))
    if '/' not in argv[0]:
        found = shutil.which(argv[0], path=env['PATH'])
        if not found:
            sys.exit('run_host_gates: ' + argv[0] + ' is not on PATH')
        argv = [found, *argv[1:]]
    sources = [i for i, token in enumerate(argv) if i and token.endswith(SOURCES) and argv[i - 1] not in VALUED]
    # -MMD leaves system headers, and with them the -isystem roots inside the
    # build's checkout, out of the dependency file; -MD builds the same object.
    argv = ['-MD' if token == '-MMD' else token for token in argv]
    deps = Path(shim['deps'])
    with open(shim['lock'], 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stem = deps / (str(time.time_ns()) + '-' + str(os.getpid()))
        extra, depfiles = [], []
        if sources and not {'-M', '-MM', '-E'} & set(argv):
            if '-MF' in argv:
                depfiles.append(os.path.join(os.getcwd(), argv[argv.index('-MF') + 1]))
            elif len(sources) == 1:
                argv = [*argv, '-MD', '-MF', str(stem) + '-0.d']
                depfiles.append(str(stem) + '-0.d')
            else:
                # One -MF for several sources keeps only the last one's list, so
                # each source gets a dependency-only pass of its own.
                for index, source in enumerate(sources):
                    skip = {i for i in sources if i != source} | {i for i, t in enumerate(argv) if t == '-c'}
                    skip |= {i + d for i, t in enumerate(argv) if t == '-o' for d in (0, 1)}
                    extra.append([t for i, t in enumerate(argv) if i not in skip] + ['-M', '-MF', str(stem) + '-' + str(index) + '.d'])
                    depfiles.append(str(stem) + '-' + str(index) + '.d')
        start = time.monotonic()
        code = subprocess.run([*shim['wrapper'], *argv], env=env).returncode
        for command in extra if code == 0 else []:
            code = code or subprocess.run([*shim['wrapper'], *command], env=env).returncode
        kept = []
        for index, depfile in enumerate(depfiles):
            if code == 0 and os.path.isfile(depfile):
                copy = str(stem) + '-' + str(index) + '.kept.d'
                shutil.copyfile(depfile, copy)
                kept.append(copy)
        with open(shim['calls'], 'a') as log:
            log.write(json.dumps({'gate': os.environ.get('PURPLE_HOST_GATE'), 'argv': argv, 'exit': code, 'cwd': os.getcwd(),
                'depfiles': kept, 'missingDepfiles': len(depfiles) - len(kept) if code == 0 else 0, 'seconds': round(time.monotonic() - start, 3)}) + '\n')
    sys.exit(code)


def depfile_paths(text, cwd):
    # The prerequisites of a make-style dependency file, as absolute paths.
    text = text.replace('\\\n', ' ')
    result = []
    for line in text.splitlines():
        if ':' not in line:
            continue
        rest = re.split(r'(?<!\\):(\s|$)', line, maxsplit=1)
        for token in re.findall(r'(?:\\.|[^\s\\])+', rest[-1] if len(rest) > 1 else ''):
            token = re.sub(r'\\(.)', r'\1', token)
            result.append(os.path.normpath(token if os.path.isabs(token) else os.path.join(cwd, token)))
    return result


def exec_driver(shim_file, argv):
    # Runs one Python driver in this process with two adaptations: compiler
    # calls go through exec_compile, and a Debug recipe read from ninja compiles
    # the tree's source. Its include roots stay those of the build's checkout,
    # which its precompiled header was made from, each preceded by an overlay
    # holding the tree's files that differ there.
    shim = json.loads(Path(shim_file).read_text())
    tree, native, build = shim['tree'], shim['native'], shim['build']
    overlays = dict(shim['overlays'])
    real_popen, real_output = subprocess.Popen, subprocess.check_output

    def adapt(tokens):
        result = []
        for index, token in enumerate(tokens):
            if token.startswith('-I') and token[2:] in overlays:
                result.append('-I' + overlays[token[2:]])
            if (index and tokens[index - 1] == '-c' and under(token, native) and not under(token, build)
                    and not any(under(token, root) for root in shim['unpopulated'])):
                token = str(Path(tree) / os.path.relpath(token, native))
            result.append(token)
        return result

    def output(args, *rest, **kw):
        result = real_output(args, *rest, **kw)
        if native and native != tree and isinstance(args, (list, tuple)) and Path(str(args[0])).name == 'ninja' and 'commands' in map(str, args):
            text = result if isinstance(result, str) else result.decode()
            text = '\n'.join(shlex.join(adapt(shlex.split(line))) if ' -c ' in line else line
                for line in text.splitlines()) + '\n'
            result = text if isinstance(result, str) else text.encode()
        return result

    class Popen(real_popen):
        def __init__(self, args, *rest, **kw):
            if isinstance(args, (list, tuple)) and args:
                name = Path(str(args[0])).name
                if name in COMPILERS or (name == 'xcrun' and len(args) > 1 and str(args[1]) in COMPILERS):
                    args = [sys.executable, str(script), '--exec-compile', shim_file, *map(str, args)]
            super().__init__(args, *rest, **kw)

    subprocess.Popen, subprocess.check_output = Popen, output
    driver = Path(argv[0])
    sys.argv = [str(driver), *argv[1:]]
    sys.path.insert(0, str(driver.parent))
    runpy.run_path(str(driver), run_name='__main__')


def comparer(tree, native, build, unpopulated):
    # Whether a file of the build's checkout matches the tree: 'same',
    # 'differs', 'deleted', or 'unchecked' when the tree does not say (outside
    # that checkout, generated by the build). A submodule the tree leaves
    # unpopulated is judged by its gitlink: 'differs' when the build's copy is
    # at another commit or has local changes, 'unchecked' otherwise.
    cache = {}
    distinct = native is not None and Path(native).resolve() != tree

    def state(path):
        if path not in cache:
            root = next((root for root in unpopulated if under(path, root)), None)
            if not distinct or not under(path, native) or under(path, build):
                cache[path] = 'unchecked'
            elif root is not None:
                cache[path] = 'differs' if unpopulated[root] else 'unchecked'
            else:
                mine, other = tree / os.path.relpath(path, native), Path(path)
                if not mine.is_file():
                    cache[path] = 'deleted' if other.is_file() else 'unchecked'
                elif not other.is_file() or other.stat().st_size != mine.stat().st_size or other.read_bytes() != mine.read_bytes():
                    cache[path] = 'differs'
                else:
                    cache[path] = 'same'
        return cache[path]
    return state


def build_overlays(tree, native, build, out, state):
    # One overlay per include root of the Debug recipe that the tree populates.
    # It links the tree's new or divergent files, and holds an #error stub for
    # every header the tree deleted, so that a stale include cannot fall
    # through to the build's checkout.
    ninja = ['ninja', '-C', str(build), '-t']
    lines = subprocess.check_output([*ninja, 'commands', 'Telegram/CMakeFiles/Telegram.dir/SourceFiles/purple/purple_config_file.cpp.o'], text=True).splitlines()
    recipe = next(tokens for tokens in map(shlex.split, (line for line in lines if ' -c ' in line))
        if tokens[tokens.index('-c') + 1].endswith('/purple_config_file.cpp'))
    roots = [token[2:] for token in recipe if token.startswith('-I/') and under(token[2:], native) and not under(token[2:], str(build))]
    overlays, changed = [], set()

    def files(folder):
        for base, dirs, names in os.walk(folder):
            dirs[:] = [d for d in dirs if d not in ('.git', 'build') and not under(os.path.join(base, d), str(build))]
            for name in names:
                yield (Path(base) / name).relative_to(folder)
    for index, root in enumerate(roots):
        source = tree / os.path.relpath(root, native)
        if not source.is_dir() or not any(source.iterdir()):
            continue
        overlay = out / 'shim/overlay' / str(index)
        for relative in files(source):
            if state(str(Path(root) / relative)) == 'differs':
                (overlay / relative).parent.mkdir(parents=True, exist_ok=True)
                (overlay / relative).symlink_to(source / relative)
                changed.add(str((source / relative).relative_to(tree)))
        for relative in files(Path(root)):
            if relative.suffix in HEADERS and state(str(Path(root) / relative)) == 'deleted' and not (overlay / relative).exists():
                (overlay / relative).parent.mkdir(parents=True, exist_ok=True)
                (overlay / relative).write_text('#error "' + str(relative) + ' was deleted or renamed in the tree under test"\n')
                changed.add(str((source / relative).relative_to(tree)))
        overlays.append([root, str(overlay)])
    record = subprocess.run([*ninja, 'deps', PCH], capture_output=True, text=True).stdout.splitlines()
    members = [line.strip() for line in record[1:] if line.strip()]
    if not members:
        sys.exit('run_host_gates: ninja has no dependency record for ' + PCH + '; rebuild it before running the gates')
    divergent = sorted(os.path.relpath(path, native) for path in members if state(path) in ('differs', 'deleted'))
    return overlays, sorted(changed), divergent


def query_inputs(build, targets):
    # The explicit and implicit inputs ninja records for each target, or None
    # for a target the build does not know. Order-only inputs are left out.
    def run(names):
        result = subprocess.run(['ninja', '-C', str(build), '-t', 'query', *names], capture_output=True, text=True)
        found, current, section = {}, None, None
        for line in result.stdout.splitlines():
            if line and not line.startswith(' '):
                current, section = line[:-1] if line.endswith(':') else line, None
                found[current] = []
            elif line.strip().endswith(':') and current is not None:
                section = line.strip().split(':', 1)[0]
            elif line.strip().startswith('input:') and current is not None:
                section = 'input'
            elif section == 'input' and current is not None and not line.strip().startswith('||'):
                found[current].append(line.strip().lstrip('| ').strip())
        return result.returncode == 0, found
    ok, found = run(targets) if targets else (True, {})
    if not ok:
        found = {}
        for target in targets:
            found.update(run([target])[1])
    return {target: found.get(target) for target in targets}


def stale_reasons(calls, build, state, cache):
    # Why the Debug objects a gate linked or force-included may not match the
    # tree, by ninja's own rule (an output older than one of its recorded
    # inputs, or an input gone) and by content (an input the tree changes or
    # deletes), with no dependency record counted as stale too.
    targets = set()
    for argv in calls:
        for index, token in enumerate(argv):
            if under(token, str(build)) and token.endswith(('.o', '.a', '.pch')) and not (index and argv[index - 1] in ('-o', '-MF', '-MT')):
                targets.add(os.path.relpath(token, build))
    unknown = sorted(target for target in targets if target not in cache)
    if unknown:
        for target, inputs in query_inputs(build, unknown).items():
            cache[target] = {'inputs': inputs}
        members = sorted({m for t in unknown if t.endswith('.a') for m in cache[t]['inputs'] or []} - set(cache))
        for member, inputs in query_inputs(build, members).items():
            cache[member] = {'inputs': inputs}
        objects = [t for t in unknown + members if not t.endswith('.a') and cache[t]['inputs'] is not None]
        if objects:
            text = subprocess.run(['ninja', '-C', str(build), '-t', 'deps', *objects], capture_output=True, text=True).stdout
            records, current = {}, None
            for line in text.splitlines():
                if line and not line.startswith(' '):
                    current = line.split(':', 1)[0]
                    # ninja marks a record STALE when the output changed after
                    # it was written; its list then says nothing about the output.
                    records[current] = None if 'deps not found' in line or '(STALE)' in line else []
                elif line.strip() and current is not None and records[current] is not None:
                    records[current].append(line.strip())
            for target in objects:
                cache[target]['deps'] = records.get(target) or None
    reasons = set()

    def check(target):
        entry, output = cache[target], build / target
        if entry['inputs'] is None:
            reasons.add(target + ' is not a target of the build')
            return
        if not output.exists():
            reasons.add(target + ' has not been built')
            return
        built = output.stat().st_mtime_ns
        if target.endswith('.a'):
            for member in entry['inputs']:
                if (build / member).exists() and (build / member).stat().st_mtime_ns > built:
                    reasons.add(target + ' predates its member ' + member)
                check(member)
            return
        if not entry.get('deps'):
            reasons.add(target + ' has no dependency record in the build')
        for value in [*entry['inputs'], *(entry.get('deps') or [])]:
            if value.startswith('CXX_COMPILER_') or value.startswith('C_COMPILER_'):
                continue
            path = os.path.normpath(value if os.path.isabs(value) else str(build / value))
            if not os.path.exists(path):
                reasons.add(target + ' was built from ' + path + ', which no longer exists')
                continue
            if os.stat(path).st_mtime_ns > built:
                reasons.add(target + ' predates ' + path)
            if state(path) == 'differs':
                reasons.add(target + ' was built from ' + path + ', which differs in the tree')
            elif state(path) == 'deleted':
                reasons.add(target + ' was built from ' + path + ', which the tree deletes')
    for target in sorted(targets):
        check(target)
    return targets, sorted(reasons)


def plan_manifest(build, out):
    # A copy of the build's manifest for dry runs, kept outside the build. No
    # edge produces the copy, so ninja never tries to regenerate it, and its
    # order-only dependencies are dropped: those only order a first build (CMake
    # hangs every target's custom commands on them, some of which run on every
    # build), so a target's own inputs and recorded dependencies are what decide
    # whether it is current. The edge that regenerates build.ninja loses its
    # CONFIGURE_DEPENDS glob check, which is dirty on every run and would stop
    # ninja from comparing the CMake files themselves; a dry run of build.ninja
    # then says whether the manifest is behind them.
    lines, logical = [], ''
    for line in (build / 'build.ninja').read_text().split('\n'):
        logical += line
        if re.search(r'(?<!\$)(\$\$)*\$$', logical):
            logical = logical[:-1]
            continue
        if logical.startswith('build ') and ' || ' in logical:
            logical = logical[:logical.index(' || ')]
        if logical.startswith('build build.ninja '):
            outputs, inputs = logical.split(': ', 1)
            inputs = [value for value in inputs.split(' ') if not value.endswith('/CMakeFiles/cmake.verify_globs')]
            assert inputs[0] == 'RERUN_CMAKE', 'unexpected regeneration edge in build.ninja'
            logical = outputs + ': ' + ' '.join(inputs)
        lines.append(logical)
        logical = ''
    plan = out / 'shim/plan/build.ninja'
    plan.parent.mkdir(parents=True, exist_ok=True)
    plan.write_text('\n'.join(lines))
    return plan


def pending_work(build, plan, targets):
    # ninja's own answer to whether these targets are current, from a dry run of
    # the plan manifest, which writes nothing: the steps it would run, and why.
    # Only "no work to do" is clean. The status format is pinned, and output of
    # any other shape is an error rather than an empty plan.
    result = subprocess.run(['ninja', '-C', str(build), '-f', str(plan), '-n', '-d', 'explain', *sorted(targets)],
        capture_output=True, text=True, env=dict(os.environ, NINJA_STATUS='[%f/%t] '))
    if result.returncode:
        raise RuntimeError('the ninja dry run failed: ' + (result.stderr.strip() or result.stdout.strip())[-500:])
    lines = [line for line in result.stdout.splitlines() if not line.startswith('ninja: Entering directory')]
    explained = [line.split('explain: ', 1)[1] for line in result.stderr.splitlines() if 'explain: ' in line]
    if lines == ['ninja: no work to do.'] and not explained:
        return [], []
    steps = [re.sub(r'^\[\d+/\d+\] ', '', line) for line in lines]
    if not steps or any(not re.match(r'\[\d+/\d+\] ', line) for line in lines):
        raise RuntimeError('the ninja dry run printed output of an unexpected form: ' + '\n'.join(lines + explained)[-500:])
    return steps, explained or ['ninja plans ' + str(len(steps)) + ' steps without explaining them']


def blob(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def file_mode(path):
    return '100755' if path.stat().st_mode & 0o100 else '100644'


def git_tree(entries):
    # The git tree object id of {path: (mode, sha)}.
    root = {}
    for path, value in entries.items():
        node = root
        *parents, name = path.split('/')
        for parent in parents:
            node = node.setdefault(parent, {})
        node[name] = value

    def write(node):
        items = []
        for name, value in node.items():
            raw = name.encode('utf-8', 'surrogateescape')
            if isinstance(value, dict):
                items.append((raw + b'/', b'40000', raw, write(value)))
            else:
                items.append((raw, value[0].encode(), raw, value[1]))
        body = b''.join(mode + b' ' + raw + b'\0' + bytes.fromhex(sha) for _, mode, raw, sha in sorted(items))
        return hashlib.sha1(b'tree %d\0' % len(body) + body).hexdigest()
    return write(root)


def toplevel(folder):
    try:
        top = subprocess.run(['git', '-C', str(folder), 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return False
    return bool(top) and Path(top).resolve() == folder.resolve()


def tree_hash(tree):
    # The hash `git add -A && git write-tree` would give, computed without
    # writing to the repository; a sha256 manifest when the tree is not git.
    # Like write-tree, it records a submodule only by its commit; submodule
    # contents are hashed separately by submodules().
    if not toplevel(tree):
        digest = hashlib.sha256()
        for path in sorted(p for p in tree.rglob('*') if p.is_file() and '.git' not in p.relative_to(tree).parts):
            digest.update(str(path.relative_to(tree)).encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
        return 'sha256-manifest:' + digest.hexdigest()

    def names(*args):
        out = subprocess.run(['git', '-C', str(tree), *args], capture_output=True, check=True).stdout
        return [value.decode('utf-8', 'surrogateescape') for value in out.split(b'\0') if value]

    entries = {}
    for line in names('ls-files', '-s', '-z'):
        meta, path = line.split('\t', 1)
        mode, sha, _ = meta.split()
        entries[path] = (mode, sha)
    for path in names('ls-files', '-z', '--deleted'):
        entries.pop(path, None)
    for path, (mode, sha) in list(entries.items()):
        if mode == '160000' and (tree / path / '.git').exists():
            entries[path] = (mode, subprocess.run(['git', '-C', str(tree / path), 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip())
    changed = [path for path in names('ls-files', '-z', '--modified', '--others', '--exclude-standard')
        if not path.endswith('/') and entries.get(path, ('',))[0] != '160000' and os.path.lexists(tree / path)]
    paths = []
    for path in changed:
        if os.path.islink(tree / path):
            entries[path] = ('120000', blob(os.readlink(tree / path).encode('utf-8', 'surrogateescape')))
        else:
            entries[path] = (file_mode(tree / path), None)
            paths.append(path)
    if paths:
        shas = subprocess.run(['git', '-C', str(tree), 'hash-object', '--stdin-paths'], input='\n'.join(paths) + '\n',
            capture_output=True, text=True, check=True).stdout.split()
        for path, sha in zip(paths, shas, strict=True):
            entries[path] = (entries[path][0], sha)
    return git_tree(entries)


def submodules(tree, native):
    # Every submodule of the tree. A populated one is hashed like a git tree
    # from its working files and compared with the tree its gitlink names;
    # without a .git of its own, the build's checkout of the same submodule
    # supplies the ignore rules and that tree. An unpopulated one records
    # whether the build's copy sits at the gitlink without local changes.
    if not toplevel(tree):
        return {}
    listing = subprocess.run(['git', '-C', str(tree), 'ls-files', '-s', '-z'], capture_output=True, check=True).stdout
    result = {}
    for line in [value.decode() for value in listing.split(b'\0') if value]:
        meta, path = line.split('\t', 1)
        mode, link, _ = meta.split()
        if mode != '160000':
            continue
        folder = tree / path
        other = Path(native) / path if native and Path(native).resolve() != tree and (Path(native) / path / '.git').exists() else None
        if not folder.is_dir() or not any(folder.iterdir()):
            entry = {'gitlink': link, 'state': 'unpopulated'}
            if other:
                head = subprocess.run(['git', '-C', str(other), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
                dirty = subprocess.run(['git', '--no-optional-locks', '-C', str(other), 'diff', '--quiet', 'HEAD', '--ignore-submodules=all'],
                    capture_output=True).returncode != 0
                entry.update(buildHead=head, buildDirty=dirty)
            result[path] = entry
            continue
        if (folder / '.git').exists():
            repo, content = folder, tree_hash(folder)
        else:
            repo = other
            names = [str(p.relative_to(folder)) for p in folder.rglob('*') if (p.is_file() or p.is_symlink()) and '.git' not in p.relative_to(folder).parts]
            ignored = set()
            if repo:
                ignored = set(subprocess.run(['git', '-C', str(repo), 'check-ignore', '--no-index', '--stdin'], input='\n'.join(names) + '\n',
                    capture_output=True, text=True).stdout.splitlines())
            entries = {}
            for name in names:
                item = folder / name
                if name in ignored:
                    continue
                entries[name] = ('120000', blob(os.readlink(item).encode())) if item.is_symlink() else (file_mode(item), blob(item.read_bytes()))
            content = git_tree(entries)
        expected = None
        if repo:
            found = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--verify', '-q', link + '^{tree}'], capture_output=True, text=True)
            expected = found.stdout.strip() or None
        result[path] = {'gitlink': link, 'state': 'matches gitlink' if content == expected else 'differs from gitlink' if expected else 'unverifiable',
            'hash': content, 'gitlinkTree': expected}
    return result


def dependencies(gate):
    return re.findall(r'\{out:([^}]+)\}', json.dumps([gate.get('args', []), gate.get('runs', [])]))


def self_check(tree, listing, own):
    problems = []
    gates = listing['gates']
    names = [gate['name'] for gate in gates]
    problems += [name + ' is listed twice' for name in sorted({n for n in names if names.count(n) > 1})]
    drivers = {gate['driver'] for gate in gates}
    covered = {fixture for gate in gates for fixture in gate.get('fixtures', [])}
    undriven = listing.get('undriven', {})
    variants = listing.get('variants', {})
    purple = tree / 'purple'
    for path in sorted(purple.glob('test_*.py')) + sorted(purple.glob('test_*.sh')):
        relative = 'purple/' + path.name
        if relative not in drivers:
            problems.append(relative + ' is a driver with no configuration in host_gates.json')
        for name in sorted(set(re.findall(r'test_\w+\.cpp', path.read_text()))):
            if 'purple/' + name not in covered:
                problems.append(relative + ' names purple/' + name + ', which no configuration lists')
    programs = sorted(purple.glob('test_*.cpp')) + sorted(p for p in (tree / 'Telegram/SourceFiles/purple/tests').glob('*') if p.suffix in ('.cpp', '.mm'))
    for path in programs:
        relative = str(path.relative_to(tree))
        if relative not in covered and relative not in undriven:
            problems.append(relative + ' is neither listed by a configuration nor recorded as undriven')
    for relative in sorted(undriven):
        if relative in covered:
            problems.append(relative + ' is recorded as undriven, but a configuration lists it')
        elif not (tree / relative).exists():
            problems.append(relative + ' is recorded as undriven, but does not exist')
    count = collections.Counter(gate['driver'] for gate in gates)
    seen = set()
    for gate in gates:
        driver = tree / gate['driver']
        text = driver.read_text() if driver.exists() else None
        if own and text is None:
            problems.append(gate['name'] + ' names ' + gate['driver'] + ', which does not exist')
        for fixture in gate.get('fixtures', []):
            if own and not (tree / fixture).exists():
                problems.append(gate['name'] + ' lists ' + fixture + ', which does not exist')
            elif text is not None and Path(fixture).name not in text:
                problems.append(gate['name'] + ' lists ' + fixture + ', which ' + gate['driver'] + ' never names')
        if count[gate['driver']] > 1:
            # A driver with several configurations: each lists exactly the
            # fixtures its arguments select, by file name or by a flag the
            # list declares under "variants" ("" is the driver's default).
            choices = variants.get(gate['driver'], {})
            arguments = gate.get('args', [])
            selected = {'purple/' + value for value in arguments if re.fullmatch(r'test_\w+\.cpp', value)}
            selected |= {choices[value] for value in arguments if value in choices}
            if not selected and '' in choices:
                selected = {choices['']}
            if selected != set(gate.get('fixtures', [])):
                problems.append(gate['name'] + ' lists ' + str(sorted(gate.get('fixtures', []))) + ', but its arguments select ' + str(sorted(selected)))
        for dependency in dependencies(gate):
            if dependency not in seen:
                problems.append(gate['name'] + ' reuses ' + dependency + ', which is not listed before it')
        seen.add(gate['name'])
    for driver, choices in sorted(variants.items()):
        for flag, fixture in sorted(choices.items()):
            if own and not (tree / fixture).exists():
                problems.append('the ' + (flag or 'default') + ' variant of ' + driver + ' names ' + fixture + ', which does not exist')
    return problems


def execute(command, log, env, cwd, timeout):
    start = time.monotonic()
    with log.open('wb') as handle:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=handle,
            stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                code = process.wait()
        except BaseException:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    return code, round(time.monotonic() - start, 1), timed_out


def main():
    parser = argparse.ArgumentParser(description='Run every Purple Desktop host gate against one tree.')
    parser.add_argument('--tree', type=Path, default=script.parent.parent, help='source tree to test (default: this repository)')
    parser.add_argument('--output-dir', type=Path, help='results, logs and gate outputs; must be outside the tree')
    parser.add_argument('--build-dir', type=Path, help='configured Debug build whose recipes and objects the compiled gates use')
    parser.add_argument('--only', action='append', default=[], help='comma-separated configurations to run, plus what they reuse')
    parser.add_argument('--skip', action='append', default=[], help='comma-separated configurations to leave out')
    parser.add_argument('--jobs', type=int, default=4, help='parallel jobs for the gates that do not compile')
    parser.add_argument('--compile-wrapper', default='', help='command prefix around every compiler or linker call')
    parser.add_argument('--qt-prefix', type=Path, help='Qt prefix for the shell gates (default: $QtPrefix, then the build\'s Qt)')
    parser.add_argument('--timeout', type=int, default=3600, help='seconds allowed per configuration')
    parser.add_argument('--min-free-gb', type=float, default=5.0, help='stop before a compiled gate when free space falls below this')
    parser.add_argument('--accept-stale', action='store_true', help='count STALE configurations as passing; the summary still names them')
    parser.add_argument('--list', action='store_true', help='print the canonical list, run the self-check and exit')
    args = parser.parse_args()
    tree = args.tree.resolve()
    source = tree / 'purple/host_gates.json'
    own = source.exists()
    if not own:
        source = script.parent / 'host_gates.json'
    listing = json.loads(source.read_text())
    list_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    gates = listing['gates']
    by_name = {gate['name']: gate for gate in gates}
    problems = self_check(tree, listing, own)
    if args.list:
        for gate in gates:
            print(gate['name'].ljust(40) + gate['driver'] + (' (compiles)' if gate.get('compiles') else ''))
        print(str(len(gates)) + ' configurations, list sha256 ' + list_hash[:16])
    for problem in problems:
        print('self-check: ' + problem, file=sys.stderr)
    if args.list or problems:
        sys.exit(1 if problems else 0)

    def split(values):
        return {name for value in values for name in value.split(',') if name}
    wanted, skipped = split(args.only), split(args.skip)
    unknown = sorted((wanted | skipped) - set(by_name))
    if unknown:
        parser.error('unknown configurations: ' + ', '.join(unknown))
    selected = (wanted or set(by_name)) - skipped
    pending = list(selected)
    while pending:
        for dependency in dependencies(by_name[pending.pop()]):
            if dependency in skipped:
                parser.error(dependency + ' is skipped, but a selected configuration reuses its output')
            if dependency not in selected:
                selected.add(dependency)
                pending.append(dependency)
    chosen = [gate for gate in gates if gate['name'] in selected]

    if args.output_dir is None:
        parser.error('--output-dir is required')
    out = args.output_dir.resolve()
    if out == tree or tree in out.parents:
        parser.error('--output-dir must be outside the tree')
    if out.exists() and any(out.iterdir()) and not (out / 'results.json').exists():
        parser.error('--output-dir is not empty and holds no earlier results.json')
    build = native = qt_dir = None
    warnings = []
    if any('{build}' in gate.get('args', []) for gate in chosen):
        if args.build_dir is None:
            parser.error('--build-dir is required for the compiled gates')
        build = args.build_dir.resolve()
        cache = (build / 'CMakeCache.txt').read_text() if (build / 'CMakeCache.txt').exists() else ''
        if not cache or not (build / 'build.ninja').exists():
            parser.error(str(build) + ' is not a configured ninja build')
        values = dict(line.split('=', 1) for line in cache.splitlines() if '=' in line and not line.startswith(('#', '//')))
        native = values.get('CMAKE_HOME_DIRECTORY:INTERNAL')
        if values.get('CMAKE_BUILD_TYPE:STRING') != 'Debug':
            warnings.append('build type is ' + str(values.get('CMAKE_BUILD_TYPE:STRING')) + ', not Debug')
        qt_dir = values.get('Qt6_DIR:PATH')
    if any(gate.get('compiles') for gate in chosen) and not any((tree / 'Telegram/ThirdParty/purple_core/purple').glob('*.cpp')):
        parser.error('the tree has no Telegram/ThirdParty/purple_core sources; populate that submodule first')
    qt = args.qt_prefix or (Path(os.environ['QtPrefix']) if os.environ.get('QtPrefix') else Path(qt_dir).parents[2] if qt_dir else None)
    if any(gate['driver'].endswith('.sh') for gate in chosen) and (qt is None or not (qt / 'frameworks/QtCore.framework').exists()):
        parser.error('no Qt prefix with frameworks/QtCore.framework; pass --qt-prefix')
    wrapper = shlex.split(args.compile_wrapper)

    modules = submodules(tree, native)
    unpopulated = {str(Path(native) / path): entry.get('buildHead') != entry['gitlink'] or entry.get('buildDirty', False)
        for path, entry in modules.items() if native and entry['state'] == 'unpopulated' and 'buildHead' in entry}
    moved = sorted(os.path.relpath(root, native) for root, differs in unpopulated.items() if differs)
    if moved:
        warnings.append('the build\'s copies of these unpopulated submodules are not at the tree\'s gitlink, so objects built from them are STALE: ' + ' '.join(moved))
    state = comparer(tree, native, str(build) if build else None, unpopulated)
    if (out / 'shim').exists():
        shutil.rmtree(out / 'shim')
    for folder in ['logs', 'gates', 'tmp', 'host-config', 'shim/bin', 'shim/deps']:
        (out / folder).mkdir(parents=True, exist_ok=True)
    overlays, changed, divergent = build_overlays(tree, native, build, out, state) if native and Path(native).resolve() != tree else ([], [], [])
    if changed:
        warnings.append(str(len(changed)) + ' tree files under the build\'s include roots differ from its checkout or are gone from the tree; compiles see the tree\'s version first: ' + ' '.join(changed))
    if divergent:
        warnings.append('headers in the build\'s precompiled header differ in the tree; gates compiled with it see the build\'s copies and are marked STALE: ' + ' '.join(divergent))
    generated = []
    if native and Path(native).resolve() != tree:
        langs = sorted(str(p.relative_to(tree)) for p in (tree / 'Telegram/Resources/langs').glob('*')
            if p.is_file() and state(str(Path(native) / p.relative_to(tree))) != 'same')
        generated = sorted({path for path in changed if path.endswith(GENERATOR_INPUTS)} | set(langs))
    if generated:
        warnings.append('the build\'s generated headers come from inputs the tree changes, so gates that reuse the build are STALE: ' + ' '.join(generated))
    (out / 'compile-calls.jsonl').write_text('')
    shim_file = out / 'shim/shim.json'
    shim_file.write_text(json.dumps({'tree': str(tree), 'native': native, 'build': str(build) if build else None,
        'overlays': overlays, 'unpopulated': sorted(unpopulated), 'wrapper': wrapper, 'bin': str(out / 'shim/bin'),
        'lock': str(out / 'shim/compile.lock'), 'calls': str(out / 'compile-calls.jsonl'), 'deps': str(out / 'shim/deps')}, indent=2) + '\n')
    plan, outputs, manifest = None, set(), []
    if build:
        plan = plan_manifest(build, out)
        listed = subprocess.run(['ninja', '-C', str(build), '-t', 'targets', 'all'], capture_output=True, text=True, check=True).stdout
        outputs = {line.rsplit(': ', 1)[0] for line in listed.splitlines() if ': ' in line}
        try:
            manifest = pending_work(build, plan, {'build.ninja'})[1]
        except RuntimeError as error:
            sys.exit('run_host_gates: cannot tell whether the build manifest is current: ' + str(error))
        if manifest:
            warnings.append('the build\'s manifest is behind its CMake inputs, so every gate that reuses the build is STALE: ' + '; '.join(manifest[:5]))
    for name in COMPILERS:
        tool = out / 'shim/bin' / name
        tool.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(script), '--exec-compile', str(shim_file), name]) + ' "$@"\n')
        tool.chmod(0o755)
    env = {key: value for key, value in os.environ.items() if key not in ('PYTHONOPTIMIZE', 'PYTHONINSPECT', 'PYTHONSTARTUP')}
    env.update(QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(out / 'host-config'), PYTHONDONTWRITEBYTECODE='1',
        PATH=str(out / 'shim/bin') + os.pathsep + os.environ.get('PATH', ''))
    if qt:
        env['QtPrefix'] = str(qt)
        plugins = next((p for p in [qt / 'share/qt/plugins', qt / 'plugins'] if p.is_dir()), None)
        if plugins:
            env['QT_PLUGIN_PATH'] = str(plugins)

    def git(*command):
        result = subprocess.run(['git', '-C', str(tree), *command], capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None
    for warning in warnings:
        print('warning: ' + warning, file=sys.stderr)
    before = tree_hash(tree)
    core = modules.get('Telegram/ThirdParty/purple_core', {}).get('hash')
    results = {'runner': 'purple/run_host_gates.py', 'list': str(source), 'listSha256': list_hash, 'canonical': len(gates),
        'selected': len(chosen), 'started': datetime.datetime.now().astimezone().isoformat(), 'tree': str(tree),
        'head': git('rev-parse', 'HEAD'), 'treeHash': before, 'coreHash': core, 'submodules': modules,
        'buildDir': str(build) if build else None, 'buildCheckout': native, 'qtPrefix': str(qt) if qt else None,
        'compileWrapper': wrapper, 'acceptStale': args.accept_stale, 'warnings': warnings, 'gates': []}
    rows, ninja_cache = {}, {}

    def save():
        results['gates'] = [rows[gate['name']] for gate in chosen if gate['name'] in rows]
        temporary = out / 'results.json.tmp'
        temporary.write_text(json.dumps(results, indent=2) + '\n')
        temporary.replace(out / 'results.json')

    def substitute(value, gate):
        value = value.replace('{build}', str(build)).replace('{out}', str(out / 'gates' / gate['name']))
        return re.sub(r'\{out:([^}]+)\}', lambda m: str(out / 'gates' / m.group(1)), value)

    def run(gate):
        name = gate['name']
        row = {'name': name, 'driver': gate['driver'], 'treeHash': before, 'coreHash': core, 'command': None,
            'exitCode': None, 'durationSeconds': 0, 'log': None, 'runs': []}
        missing = [p for p in [gate['driver'], *gate.get('fixtures', [])] if not (tree / p).exists()]
        blocked = [d for d in dependencies(gate) if rows.get(d, {}).get('status') not in ('PASS', 'STALE')]
        if missing or blocked:
            row.update(status='MISSING' if missing else 'BLOCKED', missing=missing, blockedBy=blocked)
            return row
        work = out / 'gates' / name
        if work.exists():
            shutil.rmtree(work)
        (out / 'tmp' / name).mkdir(exist_ok=True)
        driver = str(tree / gate['driver'])
        command = ([sys.executable, str(script), '--exec-driver', str(shim_file), driver] if driver.endswith('.py')
            else ['bash', driver]) + [substitute(value, gate) for value in gate.get('args', [])]
        log = out / 'logs' / (name + '.log')
        gate_env = dict(env, TMPDIR=str(out / 'tmp' / name), PURPLE_HOST_GATE=name, **gate.get('env', {}))
        code, seconds, timed_out = execute(command, log, gate_env, tree, args.timeout)
        row.update(command=command, exitCode=code, durationSeconds=seconds, log=str(log), timedOut=timed_out, outputDir=str(work))
        for index, binary in enumerate(gate.get('runs', []) if code == 0 and not timed_out else []):
            binary = [substitute(value, gate) for value in binary]
            run_log = out / 'logs' / (name + '-run' + str(index) + '.log')
            run_code, run_seconds, run_timed_out = execute(binary, run_log, gate_env, tree, args.timeout)
            row['runs'].append({'command': binary, 'exitCode': run_code, 'durationSeconds': run_seconds, 'log': str(run_log), 'timedOut': run_timed_out})
        records = [json.loads(line) for line in (out / 'compile-calls.jsonl').read_text().splitlines()]
        records = [record for record in records if record['gate'] == name]
        calls = [record['argv'] for record in records]
        row['compileCalls'] = len(calls)
        reasons = []
        # Every file the compiler read, from the dependency files: one taken
        # from the build's checkout where the tree has another version means
        # the overlay could not intercept it (a neighbour include, a root that
        # is not overlaid, or the precompiled header).
        read = set()
        for record in records:
            for depfile in record.get('depfiles', []):
                read.update(depfile_paths(Path(depfile).read_text(), record['cwd']))
        row['filesRead'] = len(read)
        unrecorded = sum(record.get('missingDepfiles', 0) for record in records)
        if unrecorded:
            reasons.append(str(unrecorded) + ' compiles left no dependency file, so what they read is unknown')
        for path in sorted(read):
            if state(path) in ('differs', 'deleted'):
                reasons.append('compiled with ' + path + ' from the build\'s checkout, which ' + ('differs in the tree' if state(path) == 'differs' else 'the tree deletes'))
        if build and calls:
            reused, more = stale_reasons(calls, build, state, ninja_cache)
            reasons += more
            if reused and generated:
                reasons.append('the build\'s generated headers come from inputs the tree changes: ' + ' '.join(generated))
            row['reusedFromBuild'] = sorted(reused)
            built = {os.path.relpath(path, build) for path in read if under(path, str(build))} & outputs
            row['generatedFromBuild'] = len(built)
            if reused | built:
                try:
                    row['pendingSteps'], why = pending_work(build, plan, reused | built)
                    reasons += ['ninja would rebuild: ' + line for line in why]
                except RuntimeError as error:
                    row['failReason'] = str(error)
            if (reused | built) and manifest:
                reasons.append('the build\'s manifest is behind its CMake inputs')
        for dependency in dependencies(gate):
            if rows[dependency].get('stale'):
                reasons.append('reuses the output of ' + dependency + ', which is STALE')
        row['stale'] = reasons = list(dict.fromkeys(reasons))
        passed = code == 0 and not timed_out and all(r['exitCode'] == 0 and not r['timedOut'] for r in row['runs'])
        if row.get('failReason'):
            passed = False
        if passed and gate.get('compiles') and not calls:
            passed = False
            row['failReason'] = 'a compiling configuration made no compiler call through the wrapper'
        row['status'] = 'FAIL' if not passed else 'STALE' if reasons and not args.accept_stale else 'PASS'
        return row

    def finish(row):
        rows[row['name']] = row
        save()
        print(row['status'] + ' ' + row['name'] + ' ' + str(row['durationSeconds']) + 's' + (' (stale accepted)' if row.get('stale') and row['status'] == 'PASS' else ''), flush=True)

    stopped = None
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for row in pool.map(run, [gate for gate in chosen if not gate.get('compiles')]):
                finish(row)
        for gate in [gate for gate in chosen if gate.get('compiles')]:
            if stopped is None and shutil.disk_usage(out).free < args.min_free_gb * 1024 ** 3:
                stopped = 'free space fell below ' + str(args.min_free_gb) + ' GB'
            if stopped:
                rows[gate['name']] = {'name': gate['name'], 'driver': gate['driver'], 'status': 'NOT_RUN', 'treeHash': before}
                continue
            finish(run(gate))
    except KeyboardInterrupt:
        stopped = 'interrupted'
    finally:
        for gate in chosen:
            rows.setdefault(gate['name'], {'name': gate['name'], 'driver': gate['driver'], 'status': 'NOT_RUN', 'treeHash': before})
        after = tree_hash(tree)
        modules_after = submodules(tree, native)
        passed = sum(row['status'] == 'PASS' for row in rows.values())
        others = collections.Counter(row['status'] for row in rows.values() if row['status'] != 'PASS')
        accepted = sorted(row['name'] for row in rows.values() if row['status'] == 'PASS' and row.get('stale'))
        partial = len(chosen) != len(gates)
        differing = sorted(path for path, entry in modules.items() if entry['state'] in ('differs from gitlink', 'unverifiable'))
        changed_during = after != before or modules_after != modules
        marks = ['PARTIAL'] * partial + ['STALE ACCEPTED'] * bool(accepted) + ['NOT CLEAN'] * bool(differing or changed_during or stopped)
        summary = ' '.join([*marks, str(passed) + '/' + str(len(gates)) + ' PASS'])
        summary += ''.join(', ' + str(n) + ' ' + status for status, n in sorted(others.items()))
        if partial:
            summary += ', ' + str(len(chosen)) + ' of ' + str(len(gates)) + ' configurations selected'
        if accepted:
            summary += ', stale accepted: ' + ' '.join(accepted)
        if differing:
            summary += ', SUBMODULE NOT AT ITS GITLINK: ' + ' '.join(differing)
        if changed_during:
            summary += ', TREE CHANGED DURING THE RUN'
        if stopped:
            summary += ', STOPPED: ' + stopped
        if listing.get('undriven'):
            summary += ', ' + str(len(listing['undriven'])) + ' undriven'
        summary += ', list sha256 ' + list_hash[:16]
        results.update(finished=datetime.datetime.now().astimezone().isoformat(), treeHashAfter=after,
            coreHashAfter=modules_after.get('Telegram/ThirdParty/purple_core', {}).get('hash'), passed=passed,
            summary=summary, stopped=stopped)
        save()
        for row in rows.values():
            if row['status'] != 'PASS':
                print(row['status'] + ' ' + row['name'] + (': ' + row['log'] if row.get('log') else ''), file=sys.stderr)
                for reason in row.get('stale', [])[:20]:
                    print('    ' + reason, file=sys.stderr)
        print(summary, flush=True)
    clean = passed == len(chosen) and after == before and modules_after == modules and not stopped and not differing
    sys.exit(1 if not clean else 3 if partial else 2 if accepted else 0)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--exec-compile':
        exec_compile(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) > 2 and sys.argv[1] == '--exec-driver':
        exec_driver(sys.argv[2], sys.argv[3:])
    else:
        main()
