"""Independent Python-template full-dataset acceptance run (no HTTP/storage)."""
import argparse
from collections import Counter
from decimal import Decimal
import importlib.util
import json
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[1]


def procedure(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'procedures' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return getattr(module, name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dataset', type=Path)
    args = parser.parse_args()
    config = json.loads((ROOT.parent / 'configs/default.json').read_text(encoding='utf-8'))
    normalize, compare = procedure('normalize'), procedure('compare')
    start = time.monotonic()
    counts = Counter()
    root = args.dataset / 'RESULTS'
    before = {p.stem: p for p in (root / 'BEFORE_MASTER').glob('*.json')}
    after = {p.stem: p for p in (root / 'AFTER_BETA').glob('*.json')}
    if before.keys() != after.keys():
        raise AssertionError('Unmatched application sets')
    for app in sorted(before):
        rows = []
        for source, paths in (('BEFORE', before), ('AFTER', after)):
            raw = paths[app].read_text(encoding='utf-8-sig')
            document = json.loads(raw, parse_float=Decimal)
            if document['appData']['application_id'] != app:
                raise AssertionError('Application identifier mismatch')
            normalized = []
            normalize([{'APP_ID': app, 'SOURCE': source, 'raw': raw, 'document': document}], config, normalized.append)
            rows.extend(normalized)
            counts['normalized_' + source.lower()] += len(normalized)
        compared = []
        compare(rows, config, compared.append)
        counts['applications'] += 1
        counts['comparison_rows'] += len(compared)
        counts['changed_apps'] += any(row['DIFF_FLAG'] for row in compared)
        for row in compared:
            counts[row['CHANGE_TYPE']] += 1
            if row['PATH'] == 'decisionResult.decision':
                counts['decision_changed'] += row['DIFF_FLAG']
    expected = {'applications': 2002, 'normalized_before': 285698, 'normalized_after': 286523,
                'comparison_rows': 289533, 'changed_apps': 502, 'decision_changed': 385,
                'SAME': 275121, 'VALUE_CHANGED': 7567, 'ATTRIBUTE_NEW': 3835, 'ATTRIBUTE_LOST': 3010}
    for name, value in expected.items():
        if counts[name] != value:
            raise AssertionError(name + ': expected ' + str(value) + ', got ' + str(counts[name]))
    print(json.dumps(dict(counts, elapsed_seconds=round(time.monotonic() - start, 3)), indent=2))


if __name__ == '__main__':
    main()
