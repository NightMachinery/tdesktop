#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,re
root=Path(__file__).resolve().parent
manifest=json.loads((root/'VERSION.json').read_text())
for name,digest in manifest['files'].items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
source=(root/'sqlite3.c').read_text()
header=(root/'sqlite3.h').read_text()
pattern=r'\bsqlite3(?:[A-Za-z0-9_]*)\b'
macros=set(re.findall(r'^\s*#\s*define\s+(sqlite3[A-Za-z0-9_]*)',source+'\n'+header,re.M))
public=set(re.findall(pattern,header))
identifiers=sorted(set(re.findall(pattern,source+header))-(macros-public))
assert len(identifiers)==manifest['prefix_identifiers']
output='#pragma once\n'+''.join('#define '+name+' purple_'+name+'\n' for name in identifiers)
prefix=root/'purple_sqlite_prefix.h'
if not prefix.exists() or prefix.read_text()!=output:prefix.write_text(output)
