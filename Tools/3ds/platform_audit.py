#!/usr/bin/env python3
"""Fingerprint exact conditional blocks; carry forward only matching manual reviews."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / 'Docs/3ds'

def blocks(lines):
    stack, found = [], {}
    for index, line in enumerate(lines):
        m = re.match(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b', line)
        if not m: continue
        op = m[1]
        if op in ('if', 'ifdef', 'ifndef'):
            stack.append((index, [index], [s[0] for s in stack]))
        elif op == 'elif' and stack:
            stack[-1][1].append(index)
        elif op == 'endif' and stack:
            start, members, parents = stack.pop()
            for member in members: found[member] = (start, index, parents)
    if stack: raise ValueError('Unclosed conditional')
    return found

def inventory():
    data = json.loads((DOC / 'arm32_portability_inventory.json').read_text())
    reviews_path = DOC / 'platform_conditional_reviews.json'
    reviews = json.loads(reviews_path.read_text()) if reviews_path.exists() else {}
    entries, cache = [], {}
    for condition in data['conditionals']:
        path = condition['file']
        if path not in cache:
            lines = (ROOT / 'Source' / path).read_text().splitlines()
            cache[path] = (lines, blocks(lines))
        lines, block_map = cache[path]
        start, end, parents = block_map[condition['line'] - 1]
        code = '\n'.join(lines[start:end + 1])
        fingerprint = hashlib.sha256((path + '\n' + condition['condition'] + '\n' + code).encode()).hexdigest()
        review = reviews.get(fingerprint)
        if review and not all(review.get(k) for k in ['meaning', 'pc_behavior', 'native_behavior', 'verification']):
            raise ValueError('Incomplete manual review: ' + fingerprint)
        entries.append({'id': fingerprint, 'file': path, 'line': condition['line'],
                        'condition': condition['condition'], 'start': start + 1, 'end': end + 1,
                        'enclosing_conditions': [lines[i].strip() for i in parents],
                        'code': code, 'review': review})
    result = {'schema': 1, 'compile_inventory_label': data['label'], 'total': len(entries),
              'reviewed': sum(e['review'] is not None for e in entries),
              'semantic_review_complete': all(e['review'] is not None for e in entries),
              'runtime_acceptance': False, 'entries': entries}
    (DOC / 'platform_conditional_audit.json').write_text(json.dumps(result, indent=2) + '\n')
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--path-prefix', default='')
    parser.add_argument('--show', action='store_true')
    args = parser.parse_args()
    result = inventory()
    print(f"Reviewed {result['reviewed']} / {result['total']} exact conditional occurrences")
    if args.show:
        emitted = set()
        for e in result['entries']:
            if not e['file'].startswith(args.path_prefix): continue
            key = (e['file'], e['start'])
            if key in emitted: continue
            emitted.add(key)
            print(f"\n{e['file']}:{e['start']}..{e['end']} ({e['id'][:12]})")
            print('Within:', ' / '.join(e['enclosing_conditions']))
            print(e['code'])
