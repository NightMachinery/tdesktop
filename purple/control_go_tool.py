#!/usr/bin/env python3
import os
from pathlib import Path
import shlex
import subprocess
import sys

argv = sys.argv[1:]
if not argv:
    raise SystemExit('go toolexec requires a tool')
wrapper = shlex.split(os.environ.get('PURPLE_CONTROL_GO_COMPILE_WRAPPER', ''))
if wrapper and Path(argv[0]).name in ('compile', 'asm', 'link') and not any(
        value.startswith('-V=') for value in argv[1:]):
    raise SystemExit(subprocess.call([*wrapper, *argv]))
os.execv(argv[0], argv)
