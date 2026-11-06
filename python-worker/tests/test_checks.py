"""Behavioral tests for indexed MartAPI and unchanged organizer CHECK files."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from collections.abc import Sequence
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runner'))
from checks import MartAPI, execute_checks, load_check


def row(app='A', path='decisionResult.decision', key='decision', change='VALUE_CHANGED', before='APPROVE', after='REJECT', array_key=''):
    return {'APP_ID': app, 'PATH': path, 'KEY': key, 'ARRAY_KEY': array_key,
            'CHANGE_TYPE': change, 'DIFF_FLAG': int(change != 'SAME'),
            'VALUE_BEFORE': before, 'VALUE_AFTER': after}


def sample():
    segments = [
        {'APP_ID': 'A', 'PRODUCTS': 'P', 'DECISION_BEFORE': 'APPROVE', 'DECISION_AFTER': 'REJECT', 'DECISION_CHANGED': 1,
         'PD_MODEL_BEFORE': 'M1', 'PD_MODEL_AFTER': 'M2', 'CUTOFF_BEFORE': 'C1', 'CUTOFF_AFTER': 'C2'},
        {'APP_ID': 'B', 'PRODUCTS': 'P', 'DECISION_BEFORE': 'APPROVE', 'DECISION_AFTER': 'APPROVE', 'DECISION_CHANGED': 0,
         'PD_MODEL_BEFORE': 'M1', 'PD_MODEL_AFTER': 'M1', 'CUTOFF_BEFORE': 'C1', 'CUTOFF_AFTER': 'C1'},
    ]
    rows = [row(), row(path='characteristics.score_okb', key='score_okb', before='710', after='650'),
            row(path='decisionResult.scoring.features.score_okb', key='score_okb', before='710', after='650'),
            row(path='decisionResult.product_trials.rules.hit', key='hit', before='true', after='', change='ATTRIBUTE_LOST', array_key='1|AGELOW@21'),
            row(path='new.field', key='field', before='', after='yes', change='ATTRIBUTE_NEW'),
            row(app='B', path='decisionResult.scoring.features.score_okb', key='score_okb', before='710', after='700'),
            row(app='B', path='characteristics.score_okb', key='score_okb', before='710', after='710', change='SAME')]
    return rows, segments


def bundle():
    return [{'id': path.stem, 'name': path.stem, 'code': path.read_text(encoding='utf-8-sig'), 'params': {}}
            for path in sorted((ROOT / 'checks').glob('check_*.py'))]


class MartTests(unittest.TestCase):
    def test_all_filters_lists_and_literal_prefixes(self):
        rows = [row(path='a.%foo', key='x', before='0', after='1', array_key='key'),
                row(app='B', path='a._bar', key='y', before='false', after='false', change='SAME'),
                row(path='abc', key='z', before='', after='new', change='ATTRIBUTE_NEW')]
        mart = MartAPI(rows, [{'APP_ID': 'B'}, {'APP_ID': 'A'}, {'APP_ID': 'EMPTY'}])
        self.addCleanup(mart.close)
        self.assertEqual(mart.total_apps(), 3)
        self.assertEqual(mart.count(), 3)
        self.assertEqual(mart.app_ids(), ['A', 'B'])
        self.assertEqual([r['APP_ID'] for r in mart.segments()], ['A', 'B', 'EMPTY'])
        self.assertEqual(mart.count(path_prefix='a.'), 2)
        self.assertEqual(mart.count(path_prefix='a.%'), 1)
        self.assertEqual(mart.count(path=['a.%foo', 'abc']), 2)
        self.assertEqual(mart.count(change_type=['VALUE_CHANGED', 'ATTRIBUTE_NEW']), 2)
        self.assertEqual(mart.count(path=[], change_type=[]), 0)
        self.assertEqual(mart.app_ids(app_ids=[]), [])
        filters = dict(path='a.%foo', path_prefix='a.', key='x', array_key='key', diff_flag=1,
                       change_type='VALUE_CHANGED', value_before='0', value_after='1', app_ids=['A'])
        self.assertEqual(list(mart.rows(**filters)), rows[:1])
        self.assertEqual(mart.count(**filters), 1)
        self.assertEqual(mart.app_ids(**filters), ['A'])
        self.assertEqual(mart.count(value_after="' OR 1=1 --"), 0)
        for filters in ({'bad': 'x'}, {'app_ids': 'A'}, {'diff_flag': True}, {'change_type': 'UNKNOWN'}, {'key': ['x']}):
            with self.assertRaises(ValueError):
                mart.count(**filters)
        changed = next(mart.rows(path='a.%foo'))
        changed['PATH'] = 'mutated'
        self.assertEqual(next(mart.rows(path='a.%foo'))['PATH'], 'a.%foo')
        # Independent SQLite cursors support nested queries on the same mart.
        self.assertEqual(sum(mart.count(app_ids=[r['APP_ID']]) for r in mart.rows()), 5)

    def test_selective_queries_use_indices_and_never_rescan_source(self):
        class TrackedRows(Sequence):
            iterations = 0
            reads = 0
            def __len__(self): return 20000
            def __iter__(self):
                self.iterations += 1
                for i in range(len(self)): yield row(app='A', path='p' + str(i % 100), key='k' + str(i % 17))
            def __getitem__(self, i):
                self.reads += 1
                return row(app='A', path='p' + str(i % 100), key='k' + str(i % 17))
        source = TrackedRows()
        mart = MartAPI(source, [{'APP_ID': 'A'}])
        self.addCleanup(mart.close)
        self.assertEqual(source.iterations, 1)
        for _ in range(100):
            self.assertEqual(mart.count(path='p17'), 200)
            self.assertEqual(mart.app_ids(path='p17'), ['A'])
        self.assertEqual(source.reads, 0)
        self.assertEqual(len(list(mart.rows(path='p17'))), 200)
        self.assertEqual(source.reads, 200)
        self.assertEqual(source.iterations, 1)
        for filters in ({'path': 'p17'}, {'key': 'k2'}, {'array_key': ''}, {'diff_flag': 1},
                        {'change_type': 'VALUE_CHANGED'}, {'value_before': 'APPROVE'}, {'value_after': 'REJECT'},
                        {'app_ids': ['A']}, {'path_prefix': 'p1'}):
            where, values = mart._where(filters)
            plan = str(mart._db.execute('EXPLAIN QUERY PLAN SELECT ordinal FROM mart' + where, values).fetchall())
            self.assertIn('SEARCH', plan, plan)
        with self.assertRaises(Exception):
            mart._db.execute('DELETE FROM mart')

    def test_supplied_checks_execute_unchanged_and_share_mart(self):
        checks = bundle()
        self.assertEqual(len(checks), 5)
        rows, segments = sample()
        out = []
        execute_checks(checks, rows, segments, out.append)
        self.assertEqual(len(out), 7)
        self.assertEqual({r['CHECK_ID'] for r in out}, {c['id'] for c in checks})
        self.assertTrue(all(r['RESULT'] == 'FAIL' and r['CONFIDENCE'] == 'LOW' for r in out))
        ar = next(r for r in out if r['CHECK_ID'] == 'check_ar_drop_by_product')
        self.assertEqual((ar['CNT'], ar['TOTAL'], ar['METRIC'], ar['APP_IDS']), (1, 2, 0.5, ['A']))
        external = [r for r in out if r['CHECK_ID'] == 'check_external_score_drift']
        self.assertEqual([r['APP_IDS'] for r in external], [['A'], ['B']])
        # Parameters override exported CHECK defaults; samples remain bounded.
        checks[0]['params'] = {'app_ids_limit': 0, 'max_drop_rel': 0.9}
        out = []
        execute_checks(checks[:1], rows, segments, out.append)
        self.assertEqual((out[0]['RESULT'], out[0]['APP_IDS']), ('PASS', []))


class ChecksProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with patch.dict(sys.modules, {'sandbox': types.SimpleNamespace(confine=lambda *a, **k: None)}):
            spec = importlib.util.spec_from_file_location('checks_runner_test', ROOT / 'runner/run_stage.py')
            cls.runner = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.runner)

    def run_job(self, code, kind='checks', config=None):
        rows, segments = sample()
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'procedure.py').write_text(code, encoding='utf-8')
            with (job / 'input.ndjson').open('w', encoding='utf-8') as stream:
                stream.write(json.dumps({'type': 'meta', 'kind': kind, 'config': config or {}}) + '\n')
                for group, records in (('comparison_rows', rows), ('segments', segments)):
                    for record in records:
                        stream.write(json.dumps({'type': group, 'data': record}) + '\n')
            self.runner.execute(job)
            return [json.loads(line) for line in (job / 'output.ndjson').read_text(encoding='utf-8').splitlines()]

    def test_ndjson_bundle_and_check_validation(self):
        self.assertEqual(len(self.run_job(json.dumps(bundle()))), 7)
        result = self.run_job(bundle()[0]['code'], 'validate', {'procedure_kind': 'check'})
        self.assertTrue(result[0]['valid'])
        self.assertEqual(result[0]['id'], 'ar_drop_by_product')
        self.assertEqual(result[0]['params']['max_drop_rel'], 0.1)
        for source in ('CHECK={}', 'def run(a): pass\nCHECK={"id":"x","label":"X","params":{},"entrypoint":run}',
                       'raise RuntimeError("invalid registration")'):
            with self.assertRaises(Exception):
                self.run_job(source, 'validate', {'procedure_kind': 'check'})

    def test_invalid_results_and_exceptions_fail_with_check_identity(self):
        template = (ROOT / 'procedures/check.py').read_text()
        mutations = ["rows[0]['METRIC']=float('nan')", "rows[0]['METRIC']='text'", "rows[0]['CNT']=True",
                     "rows[0]['TOTAL']=100", "rows[0]['APP_IDS']=['foreign']", "rows[0]['APP_IDS']=['A','A']",
                     "rows[0]['CONFIDENCE']='MEDIUM'", "rows[0]['RESULT']='ERROR'", "rows[0]['SOURCE']='BOTH'",
                     "raise RuntimeError('boom')", "return None"]
        for mutation in mutations:
            code = template + '\noriginal=run\ndef broken(mart,params):\n    rows=original(mart,params)\n    ' + mutation + '\n    return rows\nCHECK["entrypoint"]=broken\n'
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, 'Named check.*custom'):
                self.run_job(json.dumps([{'id': 'custom', 'name': 'Named check', 'code': code, 'params': {}}]))

    @unittest.skipUnless(sys.platform == 'linux', 'Linux kernel sandbox required')
    def test_real_sandbox_checks_and_index_require_no_write_permission(self):
        rows, segments = sample()
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'procedure.py').write_text(json.dumps(bundle()), encoding='utf-8')
            with (job / 'input.ndjson').open('w', encoding='utf-8') as stream:
                stream.write(json.dumps({'type': 'meta', 'kind': 'checks', 'config': {}}) + '\n')
                for group, records in (('comparison_rows', rows), ('segments', segments)):
                    for record in records: stream.write(json.dumps({'type': group, 'data': record}) + '\n')
            result = subprocess.run([sys.executable, '-I', '-B', str(ROOT / 'runner/run_stage.py'), str(job)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len((job / 'output.ndjson').read_text(encoding='utf-8').splitlines()), 7)
            self.assertEqual({p.name for p in job.iterdir()}, {'procedure.py', 'input.ndjson', 'output.ndjson'})


if __name__ == '__main__':
    unittest.main()
