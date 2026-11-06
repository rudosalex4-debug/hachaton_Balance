"""Black-box acceptance tests against a running Docker Compose application.
Creates isolated test accounts/data; never deletes user data. Python stdlib only.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from http.cookiejar import CookieJar
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, url):
        self.url = url.rstrip('/') + '/api/v1'
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(CookieJar()))

    def call(self, path, data=None, method=None, expected=None):
        body = None if data is None else json.dumps(data).encode()
        request = urllib.request.Request(self.url + path, body, {'Content-Type': 'application/json'}, method=method)
        try:
            with self.opener.open(request, timeout=120) as response:
                value = json.load(response)
        except urllib.error.HTTPError as error:
            value = json.load(error)
            if expected == error.code:
                return value
            raise AssertionError(f'{path}: HTTP {error.code}: {value}') from error
        if expected and expected >= 400:
            raise AssertionError(f'{path}: expected HTTP {expected}, got success')
        return value

    def register(self):
        self.call('/register', {'username': 'accept_' + uuid.uuid4().hex[:14], 'password': uuid.uuid4().hex})

    def upload(self):
        sample = ROOT / 'tests' / 'fixtures' / 'samples'
        files = [{'path': 'manifest.json', 'content': json.dumps({'applications': 4})}]
        for source, folder in [('BEFORE_MASTER', 'BEFORE'), ('AFTER_BETA', 'AFTER')]:
            for file in sorted((sample / folder).glob('*.json')):
                files.append({'path': 'RESULTS/' + source + '/' + file.name, 'content': file.read_text(encoding='utf-8')})
        return self.call('/datasets/upload', {'name': 'Acceptance samples', 'files': files})

    def config(self, dataset_id, python=False, compare_id=None, timeout=180):
        settings = self.call('/defaults')['settings']
        settings['worker_timeout_seconds'] = timeout
        body = {'name': 'Acceptance', 'dataset_id': dataset_id, 'settings': settings}
        for stage in ('normalize', 'compare', 'aggregate'):
            body[stage + '_id'] = ('python-' if python else '') + stage
        if compare_id:
            body['compare_id'] = compare_id
        return self.call('/configs', body)

    def start(self, config):
        return self.call('/runs', {'config_id': config['id']})['id']

    def wait(self, run_id, expected='completed'):
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            run = self.call('/runs/' + run_id)['run']
            if run['status'] in ('completed', 'failed', 'cancelled'):
                assert run['status'] == expected, run
                return run
            time.sleep(.25)
        raise AssertionError('Run did not finish within 600 seconds: ' + run_id)

    def rows(self, run_id, view):
        rows, offset = [], 0
        while True:
            page = self.call(f'/runs/{run_id}/{view}?offset={offset}&limit=1000')
            rows.extend(page['items'])
            offset += len(page['items'])
            if offset >= page['total']:
                return rows
            assert page['items'], 'Pagination made no progress'


def verify(url, full):
    one, two = Client(url), Client(url)
    one.register()
    two.register()
    dataset = one.upload()
    assert dataset['applications'] == 4 and dataset['files'] == 8
    two.call('/datasets', {'path': '/etc'}, expected=400)
    one.call('/datasets/upload', {'files': [{'path': '../outside.json', 'content': '{}'}]}, expected=400)
    config = one.config(dataset['id'])
    two.call('/configs/' + config['id'], expected=403)
    run_id = one.start(config)
    run = one.wait(run_id)
    two.call('/runs/' + run_id, expected=403)
    comparisons = one.rows(run_id, 'comparison')
    keys = [(r['APP_ID'], r['PATH'], r['ARRAY_KEY']) for r in comparisons]
    assert len(keys) == len(set(keys))
    assert len(comparisons) == run['summary']['comparison_rows']
    assert sum(r['DIFF_FLAG'] for r in comparisons) == run['summary']['changed_rows']
    assert len(one.rows(run_id, 'segments')) == 4
    assert len(one.rows(run_id, 'normalized')) == run['summary']['normalized_rows']
    facts = one.call(f'/runs/{run_id}/facts?limit=10')['items']
    assert facts
    for fact in facts:
        members = one.call(f'/runs/{run_id}/facts/{fact["FACT_ID"]}/apps?limit=1000')
        assert members['total'] == fact['CNT']
        app = members['items'][0]['APP_ID']
        details = one.call(f'/runs/{run_id}/apps/{urllib.parse.quote(app)}?limit=1')
        assert len(details['comparison']) == 1 and details['total'] >= 1
    query = one.call(f'/runs/{run_id}/query', {'dimensions': ['SOURCE'], 'filters': {}})
    assert {f['SOURCE']: f['CNT'] for f in query['items']} == {'BEFORE': 4, 'AFTER': 4}
    members = one.call(f'/runs/{run_id}/query/apps', {'filters': {'SOURCE': 'AFTER'}})
    assert members['total'] == 4
    second_id = one.start(config)
    one.wait(second_id)
    assert second_id != run_id
    assert one.rows(second_id, 'comparison') == comparisons
    assert one.call('/runs/' + run_id)['run']['status'] == 'completed'
    # Same data and configuration, all three procedures replaced without recompiling C++.
    custom = one.config(dataset['id'], python=True)
    python_id = one.start(custom)
    one.wait(python_id)
    assert one.rows(python_id, 'comparison') == comparisons
    assert one.rows(python_id, 'facts') == one.rows(run_id, 'facts')
    # Simultaneous runs belonging to distinct users.
    dataset_two = two.upload()
    config_two = two.config(dataset_two['id'])
    ids = one.start(config), two.start(config_two)
    with ThreadPoolExecutor(2) as pool:
        futures = [pool.submit(one.wait, ids[0]), pool.submit(two.wait, ids[1])]
        for future in futures:
            future.result()
    one.call('/runs/' + ids[1], expected=403)
    # Errors, timeouts and cancellation must preserve server availability and hide partial results.
    for name, code, target in [
        ('error', 'def compare(rows, config, emit):\n    raise RuntimeError("acceptance failure")\n', 'failed'),
        ('timeout', 'def compare(rows, config, emit):\n    while True: pass\n', 'failed'),
        ('cancel', 'def compare(rows, config, emit):\n    while True: pass\n', 'cancelled'),
    ]:
        procedure = one.call('/procedures', {'kind': 'compare', 'name': name, 'code': code, 'params': {}})
        conf = one.config(dataset['id'], compare_id=procedure['id'], timeout=2 if name == 'timeout' else 180)
        failed_id = one.start(conf)
        if name == 'cancel':
            one.call('/runs/' + failed_id + '/cancel', {})
        one.wait(failed_id, expected=target)
        one.call('/runs/' + failed_id + '/comparison', expected=409)
        assert one.call('/me')['user']
        assert one.call('/runs/' + failed_id + '/logs')['logs']
    if full:
        datasets = one.call('/datasets')['datasets']
        large = next(d for d in datasets if d['id'] == 'builtin-RUN-20260827-0001')
        full_id = one.start(one.config(large['id']))
        report = one.wait(full_id)['summary']
        expected = {'total_apps': 2002, 'normalized_rows': 572221, 'comparison_rows': 289533,
                    'changed_rows': 14412, 'changed_apps': 502, 'decision_changed': 385, 'facts': 44588}
        for key, value in expected.items():
            assert report[key] == value, (key, report[key], value)
        for change, count in {'SAME': 275121, 'VALUE_CHANGED': 7567, 'ATTRIBUTE_NEW': 3835, 'ATTRIBUTE_LOST': 3010}.items():
            assert one.call(f'/runs/{full_id}/comparison?change_type={change}&limit=1')['total'] == count
        print('FULL DATASET:', json.dumps(report, ensure_ascii=False), flush=True)
    print('PASS: upload, validation, native/Python parity, distinct counts, drill-down, SQL grouping, '
          'repeatability, concurrent users, worker errors/timeouts/cancellation.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://localhost:8088')
    parser.add_argument('--full', action='store_true')
    arguments = parser.parse_args()
    verify(arguments.url, arguments.full)
