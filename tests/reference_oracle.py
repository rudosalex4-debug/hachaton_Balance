"""Independent raw-JSON oracle. Does not import the application or its engine."""
from collections import Counter
from decimal import Decimal
import json
from pathlib import Path

ARRAY_FIELDS = {
    'decisionResult.available_offers': ('product_id', 'type'),
    'decisionResult.product_trials': ('product_id',),
    'decisionResult.product_trials.rules': ('name',),
    'decisionResult.product_trials.scoring.scoreModels': ('modelName',),
    'decisionResult.scoring': ('modelName',),
    'decisionResult.scoring.value': None,
    'ext_data': ('body.action',),
    'product_catalog': ('mfo_id', 'product_id'),
    'tariff_matrix': ('mfo_id', 'product_id'),
}
EXCLUDED = {'appData.request_id', 'appData.createDateTime'}


def typed(value):
    if value is None:
        return ('null', None)
    if isinstance(value, bool):
        return ('boolean', value)
    if isinstance(value, (int, Decimal)):
        return ('number', Decimal(value))
    return ('string', value)


def at_path(obj, path):
    for component in path.split('.'):
        obj = obj[component]
    return typed(obj)


def leaves(obj, path='', context=()):
    """Keys remain nested tuples; no application ARRAY_KEY encoding is reused."""
    result = {}
    if path in EXCLUDED:
        return result
    if isinstance(obj, dict):
        for name, value in obj.items():
            result.update(leaves(value, path + '.' + name if path else name, context))
    elif isinstance(obj, list):
        seen = set()
        for index, value in enumerate(obj):
            fields = ARRAY_FIELDS[path]
            key = tuple(at_path(value, f) for f in fields) if fields else index
            if key in seen:
                raise AssertionError('Duplicate array identity: ' + path)
            seen.add(key)
            rows = leaves(value, path, context + (key,))
            if result.keys() & rows.keys():
                raise AssertionError('Duplicate leaf identity: ' + path)
            result.update(rows)
    else:
        result[path, context] = typed(obj)
    return result


def inspect_dataset(dataset):
    root = Path(dataset) / 'RESULTS'
    before_files = {p.name: p for p in (root / 'BEFORE_MASTER').glob('*.json')}
    after_files = {p.name: p for p in (root / 'AFTER_BETA').glob('*.json')}
    if before_files.keys() != after_files.keys():
        raise AssertionError('Dataset pairs are incomplete')
    counts = Counter()
    changed_ids = []
    missing = object()
    for name, before_file in sorted(before_files.items()):
        before = json.loads(before_file.read_text(encoding='utf-8'), parse_float=Decimal)
        after = json.loads(after_files[name].read_text(encoding='utf-8'), parse_float=Decimal)
        if not before['appData']['application_id'] == after['appData']['application_id'] == before_file.stem:
            raise AssertionError('APP_ID mismatch')
        old, new = leaves(before), leaves(after)
        counts['before_rows'] += len(old)
        counts['after_rows'] += len(new)
        counts['total_apps'] += 1
        different = False
        for key in old.keys() | new.keys():
            a, b = old.get(key, missing), new.get(key, missing)
            change = ('SAME' if a == b else 'ATTRIBUTE_NEW' if a is missing
                      else 'ATTRIBUTE_LOST' if b is missing else 'VALUE_CHANGED')
            counts[change] += 1
            different |= change != 'SAME'
        counts['changed_apps'] += different
        if different:
            changed_ids.append(before_file.stem)
        decision_changed = before['decisionResult']['decision'] != after['decisionResult']['decision']
        counts['decision_changed'] += decision_changed
        counts['changed_same_decision'] += different and not decision_changed
        counts['external_changed'] += before['characteristics']['score_okb'] != after['characteristics']['score_okb']
    counts['rows'] = sum(counts[k] for k in ('SAME', 'ATTRIBUTE_NEW', 'ATTRIBUTE_LOST', 'VALUE_CHANGED'))
    counts['union_rows'] = counts['before_rows'] + counts['after_rows']
    return dict(counts), changed_ids


if __name__ == '__main__':
    import sys
    dataset_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'data' / 'RUN-20260827-0001'
    print(json.dumps(inspect_dataset(dataset_path)[0], indent=2))
