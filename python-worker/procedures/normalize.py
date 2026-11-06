"""Standalone editable normalizer. Exact decimals; no third-party dependencies."""
import json
import re
from decimal import Decimal


def normalize(records, config, emit):
    def glob(pattern, path):
        return re.fullmatch(''.join('.*' if c == '*' else '.' if c == '?' else re.escape(c)
                                   for c in pattern), path) is not None

    def scalar(value):
        if value is None:
            return 'NULL', 'NULL'
        if isinstance(value, bool):
            return 'BOOLEAN', 'true' if value else 'false'
        if isinstance(value, str):
            return 'STRING', value
        if isinstance(value, (int, float, Decimal)):
            number = Decimal(str(value))
            if not number.is_finite() or abs(number.as_tuple().exponent) > 100000:
                raise ValueError('Invalid or oversized decimal')
            text = format(number, 'f')
            if '.' in text:
                text = text.rstrip('0').rstrip('.')
            return 'NUMBER', '0' if number == 0 else text
        raise ValueError('Array keys must be scalars')

    def identity(value):
        kind, text = scalar(value)
        escaped = text.replace('%', '%25').replace('|', '%7C').replace('~', '%7E')
        if kind == 'STRING' and text.startswith('@str:'):
            escaped = '%40' + escaped[1:]
        if kind == 'STRING' and (text in ('NULL', 'true', 'false') or
                re.fullmatch(r'-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?', text.strip())):
            escaped = '@str:' + escaped
        return escaped

    def bucket(value, path):
        kind, raw = scalar(value)
        if kind != 'NUMBER':
            return raw
        for pattern in sorted(config.get('bucket_rules', {})):
            if not glob(pattern, path):
                continue
            edges = config['bucket_rules'][pattern]
            if not isinstance(edges, list) or not edges:
                raise ValueError('Bucket edges must be a nonempty array')
            numbers = [Decimal(str(edge)) for edge in edges]
            if any(not n.is_finite() for n in numbers) or any(a >= b for a, b in zip(numbers, numbers[1:])):
                raise ValueError('Bucket edges must be finite and ascending')
            low = '-inf'
            for edge, number in zip(edges, numbers):
                label = scalar(Decimal(str(edge)))[1]
                if Decimal(raw) <= number:
                    return '(' + low + '; ' + label + ']'
                low = label
            return '(' + low + '; +inf)'
        return raw

    def field(value, path):
        for name in path.split('.'):
            if not isinstance(value, dict) or name not in value:
                raise ValueError('Missing array key field: ' + path)
            value = value[name]
        return value

    for record in records:
        app, source = record['APP_ID'], record['SOURCE']
        if source not in ('BEFORE', 'AFTER'):
            raise ValueError('Invalid SOURCE')
        raw = record.get('raw')
        if raw is not None and len(raw.encode('utf-8')) > config.get('max_json_bytes', 10485760):
            raise ValueError('JSON exceeds max_json_bytes')
        document = record['document']
        if not isinstance(document, dict):
            raise ValueError('Response must be a JSON object')
        rows = []

        def walk(value, parts, keys, depth):
            if depth > config.get('max_depth', 256):
                raise ValueError('JSON nesting exceeds max_depth')
            path = '.'.join(parts)
            if any(path == p or path.startswith(p + '.') or glob(p, path) for p in config.get('exclude_paths', [])):
                return
            if isinstance(value, dict):
                for name, child in value.items():
                    if not name or '.' in name:
                        raise ValueError('Empty/dotted JSON key requires an extended path contract')
                    walk(child, parts + [name], keys, depth + 1)
            elif isinstance(value, list):
                rule = config.get('array_rules', {}).get(path)
                if rule is None:
                    raise ValueError('Configure array_rules for path: ' + path)
                mode = rule.get('mode') if isinstance(rule, dict) else rule
                seen = {}
                for index, child in enumerate(value):
                    if mode in ('index', 'BY_INDEX'):
                        key = str(index)
                    elif mode in ('field', 'BY_FIELD'):
                        fields = rule.get('fields', [])
                        if not fields:
                            raise ValueError('BY_FIELD requires fields')
                        key = '~'.join(identity(field(child, f)) for f in fields)
                    elif mode in ('value', 'BY_VALUE'):
                        key = identity(child)
                    else:
                        raise ValueError('Unknown array mode: ' + str(mode))
                    occurrence = seen.get(key, 0)
                    seen[key] = occurrence + 1
                    if mode in ('value', 'BY_VALUE') and isinstance(rule, dict) and rule.get('allow_duplicates', False):
                        key += '@' + str(occurrence)
                    elif occurrence:
                        raise ValueError('Duplicate array key: ' + path + ': ' + key)
                    walk(child, parts, keys + [key], depth + 1)
            else:
                if len(rows) >= config.get('max_rows_per_app', 10000):
                    raise ValueError('Normalized response exceeds max_rows_per_app')
                kind, raw_value = scalar(value)
                rows.append({'APP_ID': app, 'SOURCE': source, 'PATH': path,
                             'PREFIX': parts[0] if parts else '', 'CLASS': parts[1] if len(parts) > 1 else '',
                             'SUFFIX': '.'.join(parts[2:-1]), 'KEY': parts[-1] if parts else '',
                             'ARRAY_KEY': '|'.join(keys), 'VALUE': raw_value, 'VALUE_TYPE': kind,
                             'ADJUSTED_VALUE': bucket(value, path)})
        walk(document, [], [], 0)
        for row in sorted(rows, key=lambda r: (r['APP_ID'], r['PATH'], r['ARRAY_KEY'])):
            emit(row)
