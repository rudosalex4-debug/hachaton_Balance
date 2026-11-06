"""Real HTTP acceptance for checks, reports, HTML and dynamic pivot (stdlib only).

Run against the application started by Docker Compose. Creates its own users and
data; leaves records available for inspection. --full adds the 2,002-pair run.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import time
import urllib.request

from integration_api import Client, ROOT


class OracleMart:
    """Deliberately simple independent filter oracle for the four sample pairs."""
    def __init__(self, rows, segments):
        self.data, self.segment_data = rows, segments

    def rows(self, **filters):
        columns = {'path': 'PATH', 'key': 'KEY', 'array_key': 'ARRAY_KEY',
                   'diff_flag': 'DIFF_FLAG', 'change_type': 'CHANGE_TYPE',
                   'value_before': 'VALUE_BEFORE', 'value_after': 'VALUE_AFTER',
                   'app_ids': 'APP_ID'}
        for row in self.data:
            matches = True
            for key, value in filters.items():
                if key == 'path_prefix':
                    matches &= row['PATH'].startswith(value)
                else:
                    actual = row.get(columns[key])
                    matches &= actual in value if isinstance(value, list) else actual == value
            if matches:
                yield dict(row)

    def count(self, **filters):
        return sum(1 for _ in self.rows(**filters))

    def app_ids(self, **filters):
        return sorted({row['APP_ID'] for row in self.rows(**filters)})

    def segments(self):
        return iter(self.segment_data)

    def total_apps(self):
        return len(self.segment_data)


def configuration(client, dataset, checks):
    value = client.config(dataset)
    value['checks'] = checks
    return client.call('/configs/' + value['id'], value, method='PUT')


def new_check(client, code, params=None):
    return client.call('/procedures', {'kind': 'check', 'name': 'Acceptance check',
                                      'code': code, 'params': params or {}})


CUSTOM = '''def run(mart, params):
    apps = mart.app_ids(diff_flag=1)
    return [dict(LABEL="</td><script>alert(1)</script>", SOURCE="AFTER",
                 ARGUMENT="acceptance", CNT=len(apps), TOTAL=mart.total_apps(),
                 METRIC=params["marker"], APP_IDS=apps[:params["app_ids_limit"]],
                 RESULT="FAIL" if apps else "PASS", CONFIDENCE="LOW")]
CHECK = dict(id="custom_acceptance", label="Custom", params={"marker":1,"app_ids_limit":20}, entrypoint=run)
'''


def verify(url, full=False):
    start = time.monotonic()
    client, outsider = Client(url), Client(url)
    client.register()
    outsider.register()
    dataset = client.upload()
    builtins = [p for p in client.call('/procedures')['procedures'] if p['kind'] == 'check']
    assert len(builtins) == 5, [p['id'] for p in builtins]
    scripts = {p.read_bytes().decode('utf-8-sig'): p for p in (ROOT / 'python-worker' / 'checks').glob('check_*.py')}
    for procedure in builtins:
        assert procedure['code'].lstrip('\ufeff') in scripts, 'Bundled check source changed'
        assert client.call('/procedures/' + procedure['id'])['code'] == procedure['code']
    selections = [{'procedure_id': p['id'], 'params': {'app_ids_limit': 2}} for p in builtins]
    config = configuration(client, dataset['id'], selections)
    invalid = dict(config, checks=[{'procedure_id': 'normalize', 'params': {}}])
    client.call('/configs', invalid, expected=400)
    run_id = client.start(config)
    client.call(f'/runs/{run_id}/report', expected=409)
    client.call(f'/runs/{run_id}/checks', expected=409)
    run = client.wait(run_id)
    results = client.rows(run_id, 'checks')
    assert results
    comparisons = client.rows(run_id, 'comparison')
    segments = client.rows(run_id, 'segments')
    mart = OracleMart(comparisons, segments)
    columns = ('LABEL', 'SOURCE', 'ARGUMENT', 'CNT', 'TOTAL', 'METRIC', 'APP_IDS', 'RESULT', 'CONFIDENCE')
    canonical = lambda rows: sorted(json.dumps({k: r[k] for k in columns}, ensure_ascii=False, sort_keys=True) for r in rows)
    for procedure in builtins:
        scope = {'__name__': 'acceptance_oracle'}
        exec(compile(procedure['code'], '<supplied-check>', 'exec'), scope)
        expected = scope['CHECK']['entrypoint'](mart, {'app_ids_limit': 2})
        actual = [row for row in results if row['CHECK_ID'] == procedure['id']]
        assert canonical(actual) == canonical(expected), procedure['name']
    assert len({r['id'] for r in results}) == len(results)
    for field, param in [('RESULT', 'result'), ('CONFIDENCE', 'confidence'), ('CHECK_ID', 'check_id')]:
        value = results[0][field]
        page = client.call(f'/runs/{run_id}/checks?{param}={value}&limit=1')
        assert page['total'] == sum(r[field] == value for r in results)
        assert len(page['items']) == 1
    report = client.call(f'/runs/{run_id}/report')
    assert report['summary']['total_apps'] == 4
    assert report['checks_summary']['total'] == len(results)
    assert report['checks_summary']['pass'] + report['checks_summary']['fail'] == len(results)
    assert report['checks_summary']['low_confidence'] == sum(r['CONFIDENCE'] == 'LOW' for r in results)
    for endpoint in ('report', 'checks', 'report.html'):
        outsider.call(f'/runs/{run_id}/{endpoint}', expected=403)
    assert client.call(f'/runs/{run_id}/logs')['logs']

    # Distinct cells and drill-down must agree with an independent set calculation.
    dims = ['SOURCE', 'PRODUCTS', 'DECISION_CHANGED']
    expected = defaultdict(set)
    by_app = {r['APP_ID']: r for r in segments}
    for row in comparisons:
        if not row['DIFF_FLAG']:
            continue
        for source in ('BEFORE', 'AFTER'):
            context = dict(row, **by_app[row['APP_ID']], SOURCE=source)
            expected[tuple(context[k] for k in dims)].add(row['APP_ID'])
    query = client.call(f'/runs/{run_id}/query', {'dimensions': dims, 'filters': {'DIFF_FLAG': 1}, 'limit': 1000})
    assert query['total'] == len(expected)
    for row in query['items']:
        members = expected[tuple(row[k] for k in dims)]
        assert row['CNT'] == len(members)
        filters = {k: row[k] for k in dims}
        filters['DIFF_FLAG'] = 1
        apps = client.call(f'/runs/{run_id}/query/apps', {'filters': filters, 'limit': 1000})
        assert {r['APP_ID'] for r in apps['items']} == members
    all_apps = client.call(f'/runs/{run_id}/query/apps', {'filters': {}})
    assert all_apps['total'] == 4  # never sum BEFORE + AFTER counts
    client.call(f'/runs/{run_id}/query', {'dimensions': ['PATH); DROP TABLE runs;--']}, expected=400)
    selected_paths = sorted({r['PATH'] for r in comparisons if r['DIFF_FLAG']})[:2]
    dims = ['PATH', 'SOURCE', 'VALUE_ADJUSTED', 'VALUE_TYPE']
    values_expected = defaultdict(set)
    for row in comparisons:
        if row['PATH'] not in selected_paths:
            continue
        for source in ('BEFORE', 'AFTER'):
            adjusted = row['ADJUSTED_' + source] if row['PRESENT_' + source] else '(отсутствует)'
            values_expected[(row['PATH'], source, adjusted, row['TYPE_' + source])].add(row['APP_ID'])
    values = client.call(f'/runs/{run_id}/query', {'dimensions': dims, 'filters': {'PATH': selected_paths}, 'limit': 1000})
    assert {tuple(r[k] for k in dims): r['CNT'] for r in values['items']} == {k: len(v) for k, v in values_expected.items()}
    empty_page = client.call(f'/runs/{run_id}/query', {'dimensions': dims, 'filters': {'PATH': selected_paths}, 'offset': 999999})
    assert empty_page['total'] == len(values_expected) and empty_page['items'] == []
    for invalid_filters in ({'SOURCE': 'SIDEWAYS'}, {'DIFF_FLAG': True}, {'PATH': []}, {'PATH': {'sql': 'x'}}):
        client.call(f'/runs/{run_id}/query', {'dimensions': ['PATH'], 'filters': invalid_filters}, expected=400)

    # Config params override procedure defaults; immutable snapshots survive edits.
    custom = new_check(client, CUSTOM, {'marker': 2})
    custom_config = configuration(client, dataset['id'], [{'procedure_id': custom['id'], 'params': {'marker': 7, 'app_ids_limit': 1}}])
    custom_id = client.start(custom_config)
    replacement = dict(custom, code=CUSTOM.replace('METRIC=params["marker"]', 'METRIC=99'))
    client.call('/procedures/' + custom['id'], replacement, method='PUT')
    client.wait(custom_id)
    rows = client.rows(custom_id, 'checks')
    assert len(rows) == 1 and rows[0]['METRIC'] == 7 and len(rows[0]['APP_IDS']) == 1
    details = client.call(f'/runs/{custom_id}/apps/{rows[0]["APP_IDS"][0]}')
    assert details['comparison']
    with client.opener.open(client.url + f'/runs/{custom_id}/report.html', timeout=120) as response:
        document = response.read().decode('utf-8')
        assert 'text/html' in response.headers['Content-Type']
        assert 'attachment' in response.headers['Content-Disposition']
    assert '<script>alert(1)</script>' not in document
    assert '&lt;script&gt;alert(1)&lt;/script&gt;' in document
    assert custom_id in document and 'tab=pivot' in document
    assert url.rstrip('/') in document, 'Export must link back to the application'
    repeat_id = client.start(custom_config)
    client.wait(repeat_id)
    assert client.rows(repeat_id, 'checks')[0]['METRIC'] == 99
    assert client.rows(custom_id, 'checks')[0]['METRIC'] == 7

    # A faulty check is a run error, not a fabricated PASS. No partial report leaks.
    bad_sources = [CUSTOM.replace('apps = mart.app_ids(diff_flag=1)', 'raise RuntimeError("check acceptance error")'),
                   CUSTOM.replace('SOURCE="AFTER"', 'SOURCE="UNEXPECTED"'),
                   CUSTOM.replace('APP_IDS=apps[:params["app_ids_limit"]]', 'APP_IDS=["unknown-app"]')]
    for code in bad_sources:
        bad = new_check(client, code)
        conf = configuration(client, dataset['id'], [{'procedure_id': bad['id'], 'params': {}}])
        failed = client.start(conf)
        client.wait(failed, expected='failed')
        client.call(f'/runs/{failed}/checks', expected=409)
        client.call(f'/runs/{failed}/report', expected=409)
        assert client.call('/me')['user']

    measured = {'sample_seconds': round(time.monotonic() - start, 3), 'sample_check_rows': len(results)}
    if full:
        large = next(d for d in client.call('/datasets')['datasets'] if d['id'] == 'builtin-RUN-20260827-0001')
        large_config = configuration(client, large['id'], selections)
        large_id = client.start(large_config)
        print('FULL RUN:', large_id, flush=True)
        completed = client.wait(large_id)
        summary = completed['summary']
        for key, expected_value in {'total_apps': 2002, 'changed_apps': 502, 'changed_rows': 14412,
                                    'decision_changed': 385, 'facts': 44588}.items():
            assert summary[key] == expected_value, (key, summary[key])
        full_rows = client.rows(large_id, 'checks')
        assert {r['CHECK_ID'] for r in full_rows} == {p['id'] for p in builtins}
        assert all(len(r['APP_IDS']) <= 2 for r in full_rows)
        assert client.call(f'/runs/{large_id}/report')['checks_summary']['total'] == len(full_rows)
        timed = time.monotonic()
        pivot = client.call(f'/runs/{large_id}/query', {'dimensions': ['SOURCE', 'CHANGE_TYPE'], 'filters': {'DIFF_FLAG': 1}})
        assert pivot['total'] == 6
        measured.update(full_run_id=large_id, full_summary=summary, full_check_rows=len(full_rows),
                        full_pivot_seconds=round(time.monotonic() - timed, 3))
    print('PASS: supplied checks, independent Mart oracle, params/snapshots, ownership, gated reports, '
          'HTML escaping, dynamic distinct pivot/drill-down and failed checks.', flush=True)
    print(json.dumps(measured, ensure_ascii=False), flush=True)
    return measured


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://localhost:8088')
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--result', type=Path)
    args = parser.parse_args()
    result = verify(args.url, args.full)
    if args.result:
        args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
