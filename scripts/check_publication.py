"""Heuristic publication check. Reports filenames/rule names, never matched secrets."""
from pathlib import Path
import re
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
rules = {
    'private key block': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'GitHub credential': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})\b'),
    'AWS access ID': re.compile(rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'Windows user path': re.compile(rb'[A-Za-z]:[\\/]+Users[\\/]+[^\s"<>]+', re.I),
    'Unix home path': re.compile(rb'/(?:home|Users)/[A-Za-z0-9_.-]+/'),
}
failed = []
for name in filter(None, files):
    path = root / name
    if not path.is_file():
        failed.append((name, 'missing tracked file')); continue
    low = path.name.lower()
    if (low.startswith(('checkpoint','results','selftest')) and low.endswith(('.txt','.json'))) or low in ('settings.ini','.env') or path.suffix.lower() in ('.exe','.dll','.pdb','.zip','.pem','.key','.pfx'):
        failed.append((name, 'generated, private or binary artifact'))
    if path.suffix.lower() in ('.png','.ico'): continue
    data = path.read_bytes()
    for rule, pattern in rules.items():
        if pattern.search(data): failed.append((name, rule))
for name, rule in failed: print(f'FAIL {name}: {rule}')
print(f'Checked {len(list(filter(None, files)))} tracked files; {len(failed)} findings.')
sys.exit(bool(failed))
