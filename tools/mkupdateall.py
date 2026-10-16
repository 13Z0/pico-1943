#!/usr/bin/env python3
"""Writes pico1943's sets into pico-bootLoader's updateAll.json.

c1943/c1943_sets.c is the single source of truth for the ROM sets: for each
game the folder, the MAME set, its variants in the order they are preferred,
and the files with their sizes and CRC32s. updateAll installs one variant per
game, the first: for 1943 that is 1943u, a clone, so its zips are the parent's
(a merged 1943.zip holds 1943u as well) and then the clone's own.

    tools/mkupdateall.py ../pico-bootLoader/updateAll/updateAll.json

An entry with the game's destination folder is updated in place and keeps its
id; a game without one gets a new entry at the end. Every other entry, and the
rest of the file, is left as it is. With --print the entries are printed
instead of written.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SETS_C = os.path.join(HERE, '..', 'c1943', 'c1943_sets.c')
ROMROOT = '/roms/arcade'


def strings(s):
    return re.findall(r'"([^"]*)"', s)


def parse_sets(path):
    src = open(path, encoding='utf-8').read()
    tables = {}
    for m in re.finditer(r'static const c1943_romfile_t (\w+)\[\] = \{(.*?)\n\};', src, re.S):
        tables[m.group(1)] = [
            (name, int(crc, 16), int(size, 16), var) for name, crc, size, var in
            re.findall(r'\{"([^"]+)", 0x([0-9a-fA-F]{8}), 0x([0-9a-fA-F]+), \w+, 0x[0-9a-fA-F]+, (\w+)\}', m.group(2))]
    # a file's variants, as c1943_variant_files() reads them: 0 for every
    # variant, else bit v for variant v
    masks = {k: int(v, 16) for k, v in re.findall(r'#define (V_\w+) (0x[0-9a-fA-F]+)', src)}
    masks['0'] = ~0
    sets = []
    for m in re.finditer(r'\[C1943_SET_(\w+)\] =\s*\{(.*?)\n        \},', src, re.S):
        body = m.group(2)
        field = lambda f: re.search(r'\.%s = (.*?),\n' % f, body).group(1)
        title, folder, mameset = (strings(field(f))[0] for f in ('title', 'folder', 'mameset'))
        variants = strings(re.search(r'\.variant_set = \{(.*?)\}', body).group(1))
        vnames = strings(re.search(r'\.variant_name = \{(.*?)\}', body).group(1))
        # the first variant's files: those of every variant, and its own
        chosen = [(n, c, s) for n, c, s, var in tables[field('files')] if masks[var] & 1]
        v0 = variants[0]
        if v0 == mameset:
            desc, zips = 'MAME %s (%s)' % (v0, vnames[0]), [mameset + '.zip']
        else:
            desc, zips = 'MAME %s (%s), a clone of %s' % (v0, vnames[0], mameset), [mameset + '.zip', v0 + '.zip']
        sets.append({
            'id': mameset,
            'name': title,
            'set': desc,
            'zips': zips,
            'destination': ROMROOT + '/' + folder,
            'files': [{'name': n, 'size': s, 'crc': '%08x' % c} for n, c, s in chosen],
        })
    if not sets:
        raise SystemExit('no sets found in ' + path)
    return sets


def render_entry(e, indent='    '):
    """One entry as updateAll.json writes it: a file per line, columns aligned."""
    i2 = indent + '  '
    lines = [indent + '{']
    lines.append('%s"id": %s,' % (i2, json.dumps(e['id'])))
    lines.append('%s"name": %s,' % (i2, json.dumps(e['name'], ensure_ascii=False)))
    lines.append('%s"set": %s,' % (i2, json.dumps(e['set'], ensure_ascii=False)))
    lines.append('%s"zips": [%s],' % (i2, ', '.join(json.dumps(z) for z in e['zips'])))
    lines.append('%s"destination": %s,' % (i2, json.dumps(e['destination'])))
    lines.append('%s"files": [' % i2)
    names = ['"name": %s,' % json.dumps(f['name']) for f in e['files']]
    sizes = ['"size": %d,' % f['size'] for f in e['files']]
    wn = max(len(n) for n in names)
    ws = max(len(s) for s in sizes)
    for k, f in enumerate(e['files']):
        comma = ',' if k < len(e['files']) - 1 else ''
        lines.append('%s  { %s %s "crc": "%s" }%s' % (i2, names[k].ljust(wn), sizes[k].ljust(ws), f['crc'], comma))
    lines.append(i2 + ']')
    lines.append(indent + '}')
    return '\n'.join(lines)


def render_file(doc):
    """The file: the keys before "cores" on a line each, then the entries."""
    out = ['{']
    for key, value in doc.items():
        if key != 'cores':
            out.append('  %s: %s,' % (json.dumps(key), json.dumps(value, ensure_ascii=False)))
    out.append('  "cores": [')
    out.append(',\n'.join(render_entry(e) for e in doc['cores']))
    out.append('  ]')
    out.append('}')
    return '\n'.join(out) + '\n'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sets = parse_sets(SETS_C)
    if '--print' in sys.argv or not args:
        print(',\n'.join(render_entry(s) for s in sets))
        return
    path = args[0]
    text = open(path, encoding='utf-8').read()
    doc = json.loads(text)
    if list(doc)[-1] != 'cores' or render_file(doc) != text:
        raise SystemExit('%s is not in the layout this script writes; not touching it' % path)

    cores = doc['cores']
    added = 0
    for s in sets:
        where = {c['destination']: i for i, c in enumerate(cores)}
        if s['destination'] in where:
            i = where[s['destination']]
            cores[i] = dict(s, id=cores[i]['id'])  # keep ids others rely on
        else:
            cores.append(s)
            added += 1
    ids = [c['id'] for c in cores]
    dup = set(i for i in ids if ids.count(i) > 1)
    if dup:
        raise SystemExit('duplicate ids: ' + ', '.join(sorted(dup)))
    open(path, 'w', encoding='utf-8').write(render_file(doc))
    print('%s: %d sets, %d added, %d entries in all' % (path, len(sets), added, len(cores)))


if __name__ == '__main__':
    main()
