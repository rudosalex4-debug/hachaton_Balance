"""Indexed, read-only MartAPI and the supplied CHECK protocol.

The database holds compact filter columns plus ordinals, not another copy of
the mart. NDJSON sources decode only matched rows, using their byte offsets.
All work runs inside the existing worker's memory/time/filesystem sandbox.
"""
import copy
import inspect
import json
import math
import sqlite3


CHANGES = {'SAME', 'VALUE_CHANGED', 'ATTRIBUTE_NEW', 'ATTRIBUTE_LOST'}
FILTERS = {
    'path': 'path', 'key': 'attribute_key', 'array_key': 'array_key',
    'diff_flag': 'diff_flag', 'change_type': 'change_type',
    'value_before': 'value_before', 'value_after': 'value_after',
    'app_ids': 'app_id',
}


class MartAPI:
    def __init__(self, comparison_rows, segments):
        self._rows = comparison_rows
        self._segments = segments
        self._db = sqlite3.connect(':memory:')
        self._db.execute('PRAGMA temp_store=MEMORY')
        self._db.execute('PRAGMA journal_mode=OFF')
        self._db.execute('CREATE TABLE mart (ordinal INTEGER PRIMARY KEY, app_id TEXT NOT NULL, '
                         'path TEXT NOT NULL, attribute_key TEXT NOT NULL, array_key TEXT NOT NULL, '
                         'diff_flag INTEGER NOT NULL, change_type TEXT NOT NULL, '
                         'value_before TEXT NOT NULL, value_after TEXT NOT NULL)')
        self._db.execute('CREATE TABLE segments (app_id TEXT PRIMARY KEY, ordinal INTEGER NOT NULL)')
        self._known_apps = set()
        for ordinal, segment in enumerate(segments):
            app = segment.get('APP_ID')
            if not isinstance(app, str) or not app or app in self._known_apps:
                raise ValueError('Invalid or duplicate segment APP_ID')
            self._known_apps.add(app)
            self._db.execute('INSERT INTO segments VALUES (?,?)', (app, ordinal))
        self._row_apps = set()
        self._count = 0

        def indexed_rows():
            for ordinal, row in enumerate(comparison_rows):
                names = ('APP_ID', 'PATH', 'KEY', 'ARRAY_KEY', 'VALUE_BEFORE', 'VALUE_AFTER')
                if not isinstance(row, dict) or any(not isinstance(row.get(key), str) for key in names):
                    raise ValueError('Malformed comparison row for checks')
                if row['APP_ID'] not in self._known_apps:
                    raise ValueError('Comparison references unknown APP_ID')
                if type(row.get('DIFF_FLAG')) is not int or row['DIFF_FLAG'] not in (0, 1):
                    raise ValueError('Invalid DIFF_FLAG in checks input')
                if row.get('CHANGE_TYPE') not in CHANGES:
                    raise ValueError('Invalid CHANGE_TYPE in checks input')
                self._row_apps.add(row['APP_ID'])
                self._count += 1
                yield (ordinal, row['APP_ID'], row['PATH'], row['KEY'], row['ARRAY_KEY'],
                       row['DIFF_FLAG'], row['CHANGE_TYPE'], row['VALUE_BEFORE'], row['VALUE_AFTER'])

        self._db.executemany('INSERT INTO mart VALUES (?,?,?,?,?,?,?,?,?)', indexed_rows())
        for column in FILTERS.values():
            self._db.execute('CREATE INDEX mart_' + column + ' ON mart (' + column + ')')
        self._db.execute('CREATE INDEX mart_path_diff ON mart(path,diff_flag)')
        self._db.execute('CREATE INDEX mart_change_path ON mart(change_type,path)')
        self._db.commit()
        self._db.execute('PRAGMA query_only=ON')

    def close(self):
        self._db.close()

    def total_apps(self):
        return len(self._known_apps)

    def _where(self, filters):
        clauses, parameters = [], []
        for name, value in filters.items():
            if name == 'path_prefix':
                if not isinstance(value, str):
                    raise ValueError('path_prefix must be a string')
                # A BINARY range uses the PATH index and treats %, _ literally.
                clauses.append('path >= ?')
                parameters.append(value)
                for position in range(len(value) - 1, -1, -1):
                    if ord(value[position]) < 0x10ffff:
                        upper = value[:position] + chr(ord(value[position]) + 1)
                        clauses.append('path < ?')
                        parameters.append(upper)
                        break
                continue
            if name not in FILTERS:
                raise ValueError('Unsupported MartAPI filter: ' + name)
            column = FILTERS[name]
            is_list = isinstance(value, (list, tuple, set, frozenset))
            if name == 'app_ids' and not is_list:
                raise ValueError('app_ids must be a collection of strings')
            if is_list and name not in ('path', 'change_type', 'app_ids'):
                raise ValueError(name + ' does not accept a list')
            values = list(value) if is_list else [value]
            if name == 'diff_flag':
                if type(value) is not int or value not in (0, 1):
                    raise ValueError('diff_flag must be 0 or 1')
            elif any(not isinstance(item, str) for item in values):
                raise ValueError(name + ' must contain strings')
            if name == 'change_type' and any(item not in CHANGES for item in values):
                raise ValueError('Unknown change_type')
            if not values:
                clauses.append('0')
            elif is_list:
                clauses.append(column + ' IN (' + ','.join('?' for _ in values) + ')')
                parameters.extend(values)
            else:
                clauses.append(column + ' = ?')
                parameters.append(value)
        return (' WHERE ' + ' AND '.join(clauses) if clauses else ''), parameters

    @staticmethod
    def _selected(source, ordinals):
        # NdjsonRows exposes compact offsets. One independent file per iterator
        # supports repeated/nested MartAPI queries without opening per result.
        if hasattr(source, 'offsets') and hasattr(source, '_read') and hasattr(source, 'path'):
            with source.path.open('rb') as stream:
                for (ordinal,) in ordinals:
                    stream.seek(source.offsets[ordinal])
                    yield source._read(stream)
        else:
            for (ordinal,) in ordinals:
                yield copy.deepcopy(source[ordinal])

    def rows(self, **filters):
        where, parameters = self._where(filters)
        cursor = self._db.execute('SELECT ordinal FROM mart' + where + ' ORDER BY ordinal', parameters)
        return self._selected(self._rows, cursor)

    def count(self, **filters):
        if not filters:
            return self._count
        where, parameters = self._where(filters)
        return self._db.execute('SELECT count(*) FROM mart' + where, parameters).fetchone()[0]

    def app_ids(self, **filters):
        if not filters:
            return sorted(self._row_apps)
        where, parameters = self._where(filters)
        return [row[0] for row in self._db.execute('SELECT DISTINCT app_id FROM mart' + where + ' ORDER BY app_id', parameters)]

    def segments(self):
        return self._selected(self._segments, self._db.execute('SELECT ordinal FROM segments ORDER BY app_id'))


def load_check(code, filename='check.py'):
    if not isinstance(code, str) or len(code.encode('utf-8')) > 1024 * 1024:
        raise ValueError('Check source must be a string up to 1 MiB')
    namespace = {'__name__': 'user_check', '__file__': filename}
    exec(compile(code, filename, 'exec'), namespace)
    check = namespace.get('CHECK')
    if not isinstance(check, dict):
        raise ValueError('Check must export CHECK dictionary')
    for name in ('id', 'label'):
        if not isinstance(check.get(name), str) or not check[name].strip():
            raise ValueError('CHECK.' + name + ' must be a nonempty string')
    if not isinstance(check.get('params', {}), dict):
        raise ValueError('CHECK.params must be an object')
    defaults = json.loads(json.dumps(check.get('params', {}), allow_nan=False))
    entry = check.get('entrypoint')
    if not callable(entry) or inspect.iscoroutinefunction(entry):
        raise ValueError('CHECK.entrypoint must be a synchronous callable')
    inspect.signature(entry).bind(None, None)
    return check, entry, defaults


def validate_row(row, mart, limit):
    if not isinstance(row, dict):
        raise ValueError('Check result row must be a dictionary')
    for name in ('LABEL', 'SOURCE', 'ARGUMENT', 'RESULT', 'CONFIDENCE'):
        if not isinstance(row.get(name), str):
            raise ValueError('Missing check string: ' + name)
    for name, allowed in (('SOURCE', {'BEFORE', 'AFTER'}), ('RESULT', {'PASS', 'FAIL'}),
                          ('CONFIDENCE', {'HIGH', 'LOW'})):
        if row[name] not in allowed:
            raise ValueError('Invalid check ' + name)
    if not row['LABEL'].strip():
        raise ValueError('Check LABEL must not be empty')
    for name in ('CNT', 'TOTAL'):
        if type(row.get(name)) is not int or row[name] < 0:
            raise ValueError(name + ' must be a nonnegative integer')
    if row['CNT'] > row['TOTAL'] or row['TOTAL'] > mart.total_apps():
        raise ValueError('Check requires CNT <= TOTAL <= total applications')
    metric = row.get('METRIC')
    if type(metric) not in (int, float) or not math.isfinite(metric):
        raise ValueError('METRIC must be a finite number')
    ids = row.get('APP_IDS')
    if not isinstance(ids, list) or any(not isinstance(app, str) for app in ids):
        raise ValueError('APP_IDS must be a list of strings')
    if len(ids) > min(limit, row['CNT']) or len(set(ids)) != len(ids):
        raise ValueError('APP_IDS exceeds limit/CNT or contains duplicates')
    if not set(ids).issubset(mart._known_apps):
        raise ValueError('Check returned unknown APP_ID')
    return dict(row)


def execute_checks(bundle, comparison_rows, segments, emit):
    if not isinstance(bundle, list) or len(bundle) > 100:
        raise ValueError('Checks bundle must be an array of at most 100 checks')
    identities = set()
    mart = MartAPI(comparison_rows, segments)
    try:
        for selection in bundle:
            if not isinstance(selection, dict):
                raise ValueError('Check selection must be an object')
            identity, name = selection.get('id'), selection.get('name')
            if not isinstance(identity, str) or not identity or identity in identities:
                raise ValueError('Invalid or duplicate check selection id')
            if not isinstance(name, str) or not name.strip():
                raise ValueError('Check selection name must be nonempty')
            identities.add(identity)
            try:
                check, entry, params = load_check(selection.get('code'), 'check_' + identity + '.py')
                overrides = selection.get('params', {})
                if not isinstance(overrides, dict):
                    raise ValueError('Check parameters must be an object')
                params.update(overrides)
                limit = params.get('app_ids_limit', 20)
                if type(limit) is not int or not 0 <= limit <= 10000:
                    raise ValueError('app_ids_limit must be an integer in 0..10000')
                print('CHECK started: ' + identity + ' — ' + name, flush=True)
                rows = entry(mart, params)
                if not isinstance(rows, list):
                    raise ValueError('CHECK.entrypoint must return list[dict]')
                for row in rows:
                    result = validate_row(row, mart, limit)
                    result['CHECK_ID'], result['CHECK_NAME'] = identity, name
                    emit(result)
                print('CHECK finished: ' + identity + '; rows=' + str(len(rows)), flush=True)
            except Exception as error:
                raise RuntimeError('Check "' + name + '" (' + identity + ') failed: ' + str(error)) from error
    finally:
        mart.close()
