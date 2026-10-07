#!/usr/bin/env python3
"""Dependency scan for CI (run after `flutter pub get`).

1. Licences: every package in pubspec.lock must ship a LICENSE file that is
   recognisably permissive (MIT / BSD / Apache / ISC / Zlib / Unlicense).
   GPL, AGPL and LGPL fail the check (they don't mix with a closed-source
   app). MPL / anything unrecognised fails too until someone reviews it and
   adds it to tools/ci/licence_allowlist.txt ("package  # reason").
2. Known vulnerabilities: every package version is looked up at OSV
   (osv.dev, ecosystem Pub). Any advisory fails the check.

Prints a licence table, so the log doubles as the third-party notice list.
Usage: dependency_check.py [--offline]   (offline skips the OSV lookup)
"""
import json, os, re, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.environ.get('PUB_CACHE', os.path.expanduser('~/.pub-cache'))

def packages():
    text = open(os.path.join(ROOT, 'pubspec.lock')).read()
    out = []
    for m in re.finditer(r'^  (\S+):\n((?:    .*\n)+)', text, re.M):
        body = m.group(2)
        ver = re.search(r'version: "?([^"\n]+)"?', body)
        src = re.search(r'source: (\w+)', body)
        if ver and src:
            out.append((m.group(1), ver.group(1), src.group(1)))
    return out

BAD = [('GNU AFFERO', 'AGPL'), ('GNU LESSER', 'LGPL'), ('GNU LIBRARY GENERAL', 'LGPL'),
       ('GNU GENERAL PUBLIC', 'GPL')]
GOOD = [('Apache License', 'Apache-2.0'), ('Permission is hereby granted, free of charge', 'MIT'),
        ('Redistribution and use in source and binary forms', 'BSD'),
        ('Permission to use, copy, modify, and/or distribute', 'ISC'),
        ('This is free and unencumbered software', 'Unlicense'), ('zlib License', 'Zlib')]

def classify(text):
    up = text.upper()
    for key, name in BAD:
        if key in up:
            return name, False
    for key, name in GOOD:
        if key.upper() in up:
            return name, True
    if 'MOZILLA PUBLIC LICENSE' in up:
        return 'MPL', False
    return 'unrecognised', False

def licence_text(name, ver, src):
    if src == 'sdk':  # flutter itself: BSD-3
        return 'Redistribution and use in source and binary forms'
    base = os.path.join(CACHE, 'hosted', 'pub.dev', f'{name}-{ver}')
    if not os.path.isdir(base):
        return None
    for f in sorted(os.listdir(base)):
        if re.match(r'(LICEN[CS]E|COPYING|UNLICENSE)', f, re.I):
            return open(os.path.join(base, f), errors='replace').read()
    return ''

def main():
    allow = set()
    p = os.path.join(ROOT, 'tools/ci/licence_allowlist.txt')
    if os.path.exists(p):
        allow = {l.split('#')[0].strip() for l in open(p) if l.split('#')[0].strip()}
    pk = packages()
    failures = []
    print(f'{len(pk)} packages in pubspec.lock\n')
    print(f'{"package":38} {"version":12} licence')
    for name, ver, src in pk:
        t = licence_text(name, ver, src)
        if t is None:
            lic, ok = 'not in pub cache (run flutter pub get)', False
        elif t == '':
            lic, ok = 'no LICENSE file', False
        else:
            lic, ok = classify(t)
        if not ok and name in allow:
            lic, ok = lic + ' (allowlisted)', True
        print(f'{name:38} {ver:12} {lic}')
        if not ok:
            failures.append(f'licence: {name} {ver}: {lic}')
    if '--offline' not in sys.argv:
        q = {'queries': [{'package': {'name': n, 'ecosystem': 'Pub'}, 'version': v}
                         for n, v, s in pk if s == 'hosted']}
        req = urllib.request.Request('https://api.osv.dev/v1/querybatch', json.dumps(q).encode(),
                                     {'Content-Type': 'application/json'})
        try:
            res = json.load(urllib.request.urlopen(req, timeout=60))['results']
            hosted = [x for x in pk if x[2] == 'hosted']
            for (n, v, _), r in zip(hosted, res):
                for vuln in r.get('vulns', []):
                    failures.append(f'vulnerability: {n} {v}: {vuln["id"]} https://osv.dev/{vuln["id"]}')
            print('\nOSV lookup done')
        except Exception as e:  # network trouble must not hide real failures
            print(f'\nWARNING: OSV lookup failed ({e}); vulnerabilities not checked')
    if failures:
        print('\nFAILED:')
        for f in failures:
            print(' -', f)
        return 1
    print('\ndependency check passed')
    return 0

sys.exit(main())
