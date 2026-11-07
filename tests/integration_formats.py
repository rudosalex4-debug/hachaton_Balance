"""End-to-end tests for mixed input, Python compatibility and complete table exports.

Uses the running application and isolated test accounts. Only the standard library
is required. Run: python3 tests/integration_formats.py --url http://localhost:8088
"""
import argparse
import csv
import html
import io
import json
import urllib.request
from xml.etree import ElementTree as ET

from integration_api import Client, ROOT


VIEWS = {'normalized': 'normalized', 'comparisons': 'comparison', 'facts': 'facts', 'checks': 'checks'}
MIME = {'json': 'application/json', 'xml': 'application/xml', 'csv': 'text/csv'}
SIDES = ('BEFORE_MASTER', 'AFTER_BETA')


def file_entry(path, content):
    return {'path': path, 'content': content}


def mixed_files():
    root = ROOT / 'tests' / 'fixtures' / 'formats'
    return [file_entry(p.relative_to(root).as_posix(), p.read_bytes().decode('utf-8'))
            for p in sorted(root.rglob('*')) if p.is_file()]


def json_only_reference(files):
    contents = {f['path']: f['content'] for f in files}
    reference = [file_entry('manifest.json', '{"applications":2}')]
    for side in SIDES:
        for app, original_side in [('0007', 'BEFORE_MASTER'), ('0008', 'AFTER_BETA')]:
            document = json.loads(contents[f'RESULTS/{original_side}/{app}.json'])
            if app == '0007' and side == 'AFTER_BETA':
                document['risk']['score'] = 43
            reference.append(file_entry(f'RESULTS/{side}/{app}.json', json.dumps(document, ensure_ascii=False)))
    return reference


def upload(client, name, files, expected=None):
    return client.call('/datasets/upload', {'name': name, 'files': files}, expected=expected)


def run_dataset(client, dataset_id, checks=(), normalizer='normalize'):
    config = client.call('/configs', {
        'name': 'Input formats acceptance', 'dataset_id': dataset_id,
        'normalize_id': normalizer, 'compare_id': 'compare', 'aggregate_id': 'aggregate',
        'settings': client.call('/defaults')['settings'],
        'checks': [{'procedure_id': check['id'], 'params': {}} for check in checks]
    })
    return client.wait(client.start(config))


def read_attachment(client, path, mime):
    with client.opener.open(urllib.request.Request(client.url + path), timeout=120) as response:
        assert response.status == 200, (path, response.status)
        assert response.headers.get_content_type() == mime, (path, response.headers)
        assert response.headers.get_content_disposition() == 'attachment', (path, response.headers)
        return response.read().decode('utf-8')


def xml_fields(element):
    row = {}
    for field in element:
        assert field.tag == 'field', field.tag
        name = field.get('name') if 'name' in field.attrib else json.loads(field.attrib['name-json'])
        assert name not in row, name
        row[name] = xml_value(field)
    return row


def xml_value(element):
    kind = element.attrib['type']
    if kind == 'object':
        return xml_fields(element)
    if kind == 'array':
        assert all(item.tag == 'item' for item in element)
        return [xml_value(item) for item in element]
    text = element.text or ''
    if kind == 'string':
        return json.loads(text) if element.get('encoding') == 'json' else text
    if kind == 'null':
        assert text == ''
        return None
    assert kind in ('number', 'boolean'), kind
    value = json.loads(text)
    assert type(value) is bool if kind == 'boolean' else type(value) in (int, float), (kind, text)
    return value


def csv_rows(text, reference):
    # CSV has no native scalar types. The API row supplies the column's expected type.
    reader = csv.DictReader(io.StringIO(text, newline=''), strict=True)
    columns = reader.fieldnames
    assert columns and len(set(columns)) == len(columns), columns
    if reference:
        assert set(columns) == set().union(*(row.keys() for row in reference)), columns
    records = list(reader)
    assert len(records) == len(reference), (len(records), len(reference))
    result = []
    for record, expected in zip(records, reference):
        assert None not in record and all(value is not None for value in record.values())
        row = {}
        for key, value in record.items():
            if value == r'\M':
                continue
            if value == r'\N':
                row[key] = None
            elif isinstance(expected.get(key), str):
                row[key] = value[1:] if value.startswith('\\\\') else value
            else:
                row[key] = json.loads(value)
        result.append(row)
    return result


def assert_export(client, run_id, artifact, format_name, expected):
    path = f'/runs/{run_id}/export?artifact={artifact}&format={format_name}'
    text = read_attachment(client, path, MIME[format_name])
    if format_name == 'json':
        actual = json.loads(text)
    elif format_name == 'xml':
        root = ET.fromstring(text)
        assert root.tag == 'rows' and all(row.tag == 'row' for row in root)
        actual = [xml_fields(row) for row in root]
    else:
        actual = csv_rows(text, expected)
    assert isinstance(actual, list) and len(actual) == len(expected), (artifact, format_name, len(actual), len(expected))
    for i, (record, reference) in enumerate(zip(actual, expected)):
        # JSON spelling also distinguishes false from 0 and true from 1.
        assert json.dumps(record, sort_keys=True) == json.dumps(reference, sort_keys=True), (artifact, format_name, i, record, reference)


def semantic_rows(rows, view):
    # Fact and check row IDs are local to a run. Compare all business fields.
    volatile = {'RUN_ID'}
    if view in ('facts', 'checks'):
        volatile.update(('id', 'FACT_ID'))
    return sorted(json.dumps({k: v for k, v in row.items() if k not in volatile}, sort_keys=True) for row in rows)


def large_file_set():
    values = ['', r'\N', r'\M', '<tag> & "quoted",\r\nстрока']
    doc = {'appData': {'application_id': 'BULK'}, 'fields': {
        f'field_{i:03d}': values[i % len(values)] for i in range(510)
    }}
    text = json.dumps(doc, ensure_ascii=False)
    return [file_entry('manifest.json', '{"applications":1}'),
            *(file_entry(f'RESULTS/{side}/BULK.json', text) for side in SIDES)]


def verify(url):
    client, outsider = Client(url), Client(url)
    client.register()
    outsider.register()
    files = mixed_files()
    mixed = upload(client, 'Mixed XML, CSV and JSON', files)
    reference = upload(client, 'Equivalent JSON only', json_only_reference(files))
    assert mixed['applications'] == reference['applications'] == 2
    assert mixed['files'] == reference['files'] == 4

    duplicate = file_entry('RESULTS/BEFORE_MASTER/0007.xml', '<response/>')
    upload(client, 'Duplicate ID across formats', [*files, duplicate], expected=400)
    invalid = [dict(f, content='<response><unclosed>') if f['path'].endswith('/0007.xml') else f for f in files]
    error = upload(client, 'Malformed XML', invalid, expected=400)
    assert '0007.xml' in str(error), error

    checks = [p for p in client.call('/procedures')['procedures'] if p['kind'] == 'check' and not p.get('owner')]
    assert len(checks) == 5, [p['id'] for p in checks]
    baseline = run_dataset(client, reference['id'], checks)
    native = run_dataset(client, mixed['id'], checks)
    template = client.call('/procedure-templates')['normalize']
    signature = 'def normalize(records, config, emit):'
    assert signature in template
    code = template.replace(signature, 'def supplied_normalize(records, config, emit):', 1) + '''

def normalize(records, config, emit):
    for record in records:
        if record['document']['appData']['application_id'] != record['APP_ID']:
            raise ValueError('Expected a mapped document with the original string identifier')
    supplied_normalize(records, config, emit)
'''
    procedure = client.call('/procedures', {'kind': 'normalize', 'name': 'Format acceptance normalizer', 'code': code, 'params': {}})
    custom = run_dataset(client, mixed['id'], checks, normalizer=procedure['id'])
    assert custom['snapshot']['procedures']['normalize']['id'] == procedure['id']
    native_id = native['id']
    expected_tables = {}
    for artifact, view in VIEWS.items():
        reference_rows = semantic_rows(client.rows(baseline['id'], view), view)
        rows = client.rows(native_id, view)
        assert semantic_rows(rows, view) == reference_rows, ('native', artifact)
        assert semantic_rows(client.rows(custom['id'], view), view) == reference_rows, ('custom', artifact)
        expected_tables[artifact] = rows
        for fmt in MIME:
            assert_export(client, native_id, artifact, fmt, rows)
    assert semantic_rows(client.rows(native_id, 'segments'), 'segments') == semantic_rows(client.rows(baseline['id'], 'segments'), 'segments')
    changed = [r for r in expected_tables['comparisons'] if r['DIFF_FLAG']]
    assert [(r['APP_ID'], r['PATH'], r['VALUE_BEFORE'], r['VALUE_AFTER'], r['CHANGE_TYPE']) for r in changed] == [
        ('0007', 'risk.score', '42', '43', 'VALUE_CHANGED')]
    assert {r['APP_ID'] for r in expected_tables['comparisons']} == {'0007', '0008'}
    report = client.call(f'/runs/{native_id}/report')
    assert report['checks_summary']['checks'] == 5
    assert report['checks_summary']['total'] == len(expected_tables['checks']) > 0
    report_html = read_attachment(client, f'/runs/{native_id}/report.html', 'text/html')
    assert f'run={native_id}&tab=pivot' in html.unescape(report_html)

    export = f'/runs/{native_id}/export?'
    outsider.call(export + 'artifact=facts&format=json', expected=403)
    Client(url).call(export + 'artifact=facts&format=json', expected=401)
    client.call(export + 'artifact=normalized&format=xlsx', expected=400)
    client.call(export + 'artifact=unknown&format=json', expected=400)

    large = upload(client, 'Export across page boundaries', large_file_set())
    long_run = run_dataset(client, large['id'])
    rows = client.rows(long_run['id'], 'normalized')
    assert len(rows) == 1022, len(rows)
    for fmt in MIME:
        assert_export(client, long_run['id'], 'normalized', fmt, rows)
    print(f'PASS: mixed/native {native_id}, mixed/Python {custom["id"]}, equivalent JSON {baseline["id"]}; '
          f'12 complete artifact exports matched API, HTML and permissions checked, '
          f'{len(rows)} rows verified in each format on {long_run["id"]}.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://localhost:8088')
    verify(parser.parse_args().url)
