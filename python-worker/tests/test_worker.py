"""Protocol tests run anywhere; OS confinement tests require Linux and are explicit."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from decimal import Decimal
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


normalizer = load('normalizer', ROOT / 'procedures/normalize.py').normalize
comparator = load('comparator', ROOT / 'procedures/compare.py').compare
aggregator = load('aggregator', ROOT / 'procedures/aggregate.py').aggregate


class ProcedureTests(unittest.TestCase):
    def normalize(self, value, source='BEFORE', config=None, app='A'):
        out = []
        normalizer([{'APP_ID': app, 'SOURCE': source, 'document': value}], config or {}, out.append)
        return out

    def compare(self, before, after):
        out = []
        comparator(before + after, {}, out.append)
        return out

    def test_missing_null_false_zero_empty_and_type_change(self):
        before = self.normalize({'null': None, 'false': False, 'zero': 0, 'empty': '', 'lost': None, 'typed': '0'})
        after = self.normalize({'null': None, 'false': False, 'zero': 0, 'empty': '', 'new': None, 'typed': 0}, 'AFTER')
        result = {row['PATH']: row for row in self.compare(before, after)}
        for key in ('null', 'false', 'zero', 'empty'):
            self.assertEqual(result[key]['CHANGE_TYPE'], 'SAME')
        self.assertEqual(result['lost']['CHANGE_TYPE'], 'ATTRIBUTE_LOST')
        self.assertEqual(result['new']['CHANGE_TYPE'], 'ATTRIBUTE_NEW')
        self.assertEqual(result['typed']['CHANGE_TYPE'], 'VALUE_CHANGED')

    def test_composite_keys_reordered_and_delimiter_collision(self):
        config = {'array_rules': {'offers': {'mode': 'BY_FIELD', 'fields': ['product', 'type']}}}
        records = [{'product': '', 'type': 'x', 'value': 1}, {'product': 'x', 'type': '', 'value': 2},
                   {'product': 'a~b', 'type': 'c', 'value': 3}, {'product': 'a', 'type': 'b~c', 'value': 4}]
        before = self.normalize({'offers': records}, config=config)
        after = self.normalize({'offers': list(reversed(records))}, 'AFTER', config)
        self.assertTrue(all(row['CHANGE_TYPE'] == 'SAME' for row in self.compare(before, after)))
        self.assertEqual(len({row['ARRAY_KEY'] for row in before}), 4)
        with self.assertRaisesRegex(ValueError, 'Duplicate array key'):
            self.normalize({'offers': records + records[:1]}, config=config)

    def test_value_occurrences_and_index_vectors(self):
        config = {'array_rules': {'tags': {'mode': 'BY_VALUE', 'allow_duplicates': True}, 'vector': {'mode': 'BY_INDEX'}}}
        before = self.normalize({'tags': ['a', 'a', 'b'], 'vector': [1, 2]}, config=config)
        after = self.normalize({'tags': ['b', 'a', 'a'], 'vector': [2, 1]}, 'AFTER', config)
        rows = self.compare(before, after)
        self.assertEqual(sum(row['DIFF_FLAG'] for row in rows), 2)
        self.assertEqual({row['ARRAY_KEY'] for row in before if row['PATH'] == 'tags'}, {'a@0', 'a@1', 'b@0'})

    def test_exact_decimal_and_bucket_does_not_hide_difference(self):
        config = {'bucket_rules': {'score': [0, 1]}}
        before = self.normalize({'score': Decimal('0.123456789012345678901')}, config=config)
        after = self.normalize({'score': Decimal('0.123456789012345678902')}, 'AFTER', config)
        row = self.compare(before, after)[0]
        self.assertEqual(row['ADJUSTED_BEFORE'], row['ADJUSTED_AFTER'])
        self.assertEqual(row['CHANGE_TYPE'], 'VALUE_CHANGED')
        before[0]['VALUE'] = '1.00'
        after[0]['VALUE'] = '1e0'
        self.assertEqual(self.compare(before, after)[0]['CHANGE_TYPE'], 'SAME')
        with self.assertRaisesRegex(ValueError, 'ascending'):
            self.normalize({'score': -1}, config={'bucket_rules': {'score': [0, 2, 1]}})

    def test_distinct_application_membership(self):
        flat = self.normalize({'x': 1, 'y': 2}) + self.normalize({'x': 1, 'y': 2}, 'AFTER')
        flat += self.normalize({'x': 1}, app='B') + self.normalize({'x': 1}, 'AFTER', app='B')
        compared = []
        comparator(flat, {}, compared.append)
        facts = []
        aggregator(compared, flat, [{'APP_ID': 'A'}, {'APP_ID': 'B'}], {'aggregate_dimensions': ['SOURCE']}, facts.append)
        self.assertEqual(len(facts), 2)
        for fact in facts:
            self.assertEqual(fact['CNT'], 2)
            self.assertEqual(fact['APP_IDS'], ['A', 'B'])

    def test_reference_example(self):
        example = PROJECT / 'tests' / 'fixtures' / 'example'
        if not example.exists():
            self.skipTest('Reference fixture not packaged')
        config = json.loads((PROJECT / 'configs/default.json').read_text(encoding='utf-8'))
        config['exclude_paths'].append('company.decisionResult.calculatedAt')
        config['array_rules']['company.scoreCardResult'] = {'mode': 'field', 'fields': ['name']}
        config['array_rules']['company.rejectReasons'] = {'mode': 'value'}
        before = self.normalize(json.loads((example / '01_response_BEFORE.json').read_text(encoding='utf-8-sig'), parse_float=Decimal), config=config)
        after = self.normalize(json.loads((example / '02_response_AFTER.json').read_text(encoding='utf-8-sig'), parse_float=Decimal), 'AFTER', config)
        self.assertEqual((len(before), len(after)), (12, 14))
        result = self.compare(before, after)
        self.assertEqual((len(result), sum(row['DIFF_FLAG'] for row in result)), (14, 6))


class RunnerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Only bypass confinement in in-process protocol unit tests. Never expose
        # this switch in the production runner or supervisor.
        with patch.dict(sys.modules, {'sandbox': types.SimpleNamespace(confine=lambda *a, **k: None)}):
            cls.runner = load('runner_contract_test', ROOT / 'runner/run_stage.py')

    def run_job(self, code, kind='normalize', records=(), config=None, groups=None):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'procedure.py').write_text(code, encoding='utf-8')
            lines = [json.dumps({'type': 'meta', 'kind': kind, 'config': config or {}})]
            lines += [json.dumps({'type': 'records', 'data': record}) for record in records]
            for name, rows in (groups or {}).items():
                lines += [json.dumps({'type': name, 'data': row}, ensure_ascii=False) for row in rows]
            (job / 'input.ndjson').write_text('\n'.join(lines) + '\n', encoding='utf-8')
            self.runner.execute(job)
            return [json.loads(line) for line in (job / 'output.ndjson').read_text(encoding='utf-8').splitlines()]

    def test_validate_without_executing_module(self):
        result = self.run_job('raise RuntimeError("must not execute")\ndef normalize(records, config, emit): pass', 'validate', config={'procedure_kind': 'normalize'})
        self.assertEqual(result, [{'valid': True}])

    def test_wrong_signature_and_syntax_rejected(self):
        for source in ('def normalize(): pass', 'def normalize(records, config, emit, extra): pass', 'def normalize(:'):
            with self.assertRaises((ValueError, SyntaxError)):
                self.run_job(source, 'validate', config={'procedure_kind': 'normalize'})

    def test_exception_return_value_and_nonfinite_emit_rejected(self):
        for body in ('raise RuntimeError("example")', 'return []', 'emit({"x": float("nan")})'):
            with self.assertRaises((ValueError, RuntimeError)):
                self.run_job('def normalize(records, config, emit):\n    ' + body)

    def test_duplicate_raw_json_keys_rejected(self):
        source = (ROOT / 'procedures/normalize.py').read_text(encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON member'):
            self.run_job(source, records=[{'APP_ID': 'A', 'SOURCE': 'BEFORE', 'raw': '{"x":1,"x":2}'}])

    def test_aggregate_sequence_protocol_and_utf8(self):
        code = """def aggregate(comparison_rows, union_rows, segments, config, emit):
    from collections.abc import Sequence
    assert isinstance(comparison_rows, Sequence)
    assert isinstance(union_rows, Sequence)
    assert isinstance(segments, list)
    assert len(comparison_rows) == 2 and len(union_rows) == 3
    assert comparison_rows[-1] == {'text': 'Привет 🙂'}
    assert union_rows[1::2] == [{'text': 'after'}]
    assert list(union_rows) == list(union_rows)
    assert len([(a,b) for a in comparison_rows for b in comparison_rows]) == 4
    changed = comparison_rows[0]
    changed['text'] = 'mutated'
    assert comparison_rows[0]['text'] == 'before'
    emit({'ok': True, 'last': union_rows[-1]['text']})
"""
        result = self.run_job(code, 'aggregate', groups={
            'comparison_rows': [{'text': 'before'}, {'text': 'Привет 🙂'}],
            'segments': [{'APP_ID': 'A', 'unicode': 'Разрез'}],
            'union_rows': [{'text': 'before'}, {'text': 'after'}, {'text': '終わり'}]})
        self.assertEqual(result, [{'ok': True, 'last': '終わり'}])

    def test_lazy_sequence_does_not_parse_until_access_and_can_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ndjson'
            offsets = []
            with path.open('wb') as stream:
                for value in ('Привет 🙂', 'skip', '最後'):
                    offsets.append(stream.tell())
                    stream.write((json.dumps({'type': 'rows', 'data': {'text': value}}, ensure_ascii=False) + '\n').encode('utf-8'))
            loader = Mock(side_effect=self.runner.loads)
            rows = self.runner.NdjsonRows(path, loader)
            rows.offsets.extend(offsets[::2])
            self.assertEqual(len(rows), 2)
            loader.assert_not_called()
            self.assertEqual(rows[-1], {'text': '最後'})
            self.assertEqual(loader.call_count, 1)
            expected = [{'text': 'Привет 🙂'}, {'text': '最後'}]
            self.assertEqual(list(rows), expected)
            self.assertEqual(list(rows), expected)
            self.assertEqual(rows[:], expected)
            with self.assertRaises(IndexError):
                rows[2]
            with self.assertRaises(TypeError):
                rows[0.5]

    def test_lazy_sequence_rejects_malformed_row_when_accessed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ndjson'
            path.write_text('{"data":{"x":1,"x":2}}\n', encoding='utf-8')
            rows = self.runner.NdjsonRows(path, self.runner.loads)
            rows.offsets.append(0)
            with self.assertRaisesRegex(ValueError, 'Duplicate JSON member'):
                rows[0]
            with self.assertRaisesRegex(ValueError, 'Duplicate JSON member'):
                list(rows)

    def test_stdout_and_decimal_input(self):
        source = 'def normalize(records, config, emit):\n    print("diagnostic")\n    emit({"value": str(records[0]["document"]["x"])})'
        self.assertEqual(self.run_job(source, records=[{'raw': '{"x":0.1234567890123456789}'}]), [{'value': '0.1234567890123456789'}])


class SupervisorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon = load('supervisor_test', ROOT / 'runner/daemon.py')

    def test_timeout_kills_process_group_and_reports_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'ready').write_text('1')
            (job / 'input.ndjson').write_text('{}')
            process = Mock(pid=123, **{'poll.return_value': None, 'wait.return_value': -9})
            with patch.object(self.daemon.subprocess, 'Popen', return_value=process), \
                 patch.object(self.daemon.time, 'monotonic', side_effect=[0, 2]), \
                 patch.object(self.daemon.os, 'killpg', create=True) as kill, \
                 patch.object(self.daemon.signal, 'SIGKILL', 9, create=True):
                self.daemon.work(job)
            kill.assert_called_once_with(123, 9)
            process.wait.assert_called_once()
            result = json.loads((job / 'done.json').read_text())
            self.assertFalse(result['ok'])
            self.assertIn('timeout', result['error'])

    def test_cancelled_queued_job_never_executes(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'ready').write_text('1')
            (job / 'cancel').write_text('stop')
            with patch.object(self.daemon.subprocess, 'Popen') as launch:
                self.daemon.work(job)
            launch.assert_not_called()
            self.assertEqual(json.loads((job / 'done.json').read_text()), {'ok': False, 'error': 'CANCELLED'})

    def test_procedure_failure_does_not_escape_supervisor(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            (job / 'ready').write_text('1')
            (job / 'input.ndjson').write_text('{}')
            process = Mock(**{'poll.return_value': 1, 'wait.return_value': 1})
            with patch.object(self.daemon.subprocess, 'Popen', return_value=process):
                self.daemon.work(job)
            result = json.loads((job / 'done.json').read_text())
            self.assertFalse(result['ok'])
            self.assertIn('code 1', result['error'])


@unittest.skipUnless(sys.platform == 'linux', 'Linux-only kernel confinement tests')
class SandboxTests(unittest.TestCase):
    def test_sandbox_denies_network_processes_and_other_jobs(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)
            source = '''def normalize(records, config, emit):
    import socket, subprocess
    for attack in (lambda: socket.socket(), lambda: subprocess.run(['true']), lambda: open('/etc/passwd')):
        try:
            attack()
        except OSError:
            continue
        raise AssertionError('Sandbox violation')
    emit({'ok': True})
'''
            (job / 'procedure.py').write_text(source)
            (job / 'input.ndjson').write_text(json.dumps({'type': 'meta', 'kind': 'normalize', 'config': {}}) + '\n')
            result = subprocess.run([sys.executable, '-I', '-B', str(ROOT / 'runner/run_stage.py'), str(job)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads((job / 'output.ndjson').read_text()), {'ok': True})


if __name__ == '__main__':
    unittest.main()
