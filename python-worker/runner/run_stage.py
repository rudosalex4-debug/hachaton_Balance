"""NDJSON stage bridge; isolation is installed before reading untrusted inputs."""
import ast
from array import array
from collections.abc import Sequence
import operator
import inspect
import json
from decimal import Decimal
from pathlib import Path
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sandbox import confine
from checks import execute_checks, load_check


def strict_object(pairs):
    value = {}
    for name, item in pairs:
        if name in value:
            raise ValueError('Duplicate JSON member: ' + name)
        value[name] = item
    return value


def loads(text, **kwargs):
    def reject(value):
        raise ValueError('Non-finite JSON number: ' + value)
    return json.loads(text, object_pairs_hook=strict_object, parse_constant=reject, **kwargs)


MAX_ROW_BYTES = 16 * 1024 * 1024


class NdjsonRows(Sequence):
    def __init__(self, path, loader):
        self.path = Path(path)
        self.offsets = array('Q')
        self.loader = loader

    def __len__(self):
        return len(self.offsets)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(len(self)))]
        index = operator.index(index)
        offset = self.offsets[index]
        with self.path.open('rb') as stream:
            stream.seek(offset)
            return self._read(stream)

    def _read(self, stream):
        line = stream.readline(MAX_ROW_BYTES + 1)
        if len(line) > MAX_ROW_BYTES:
            raise ValueError('Input row exceeds 16 MiB')
        return self.loader(line)['data']

    def __iter__(self):
        # One file per iterator permits independent/nested iteration, and avoids
        # reopening the input or retaining decoded dictionaries for every row.
        with self.path.open('rb') as stream:
            position = 0
            for offset in self.offsets:
                if position != offset:
                    stream.seek(offset)
                line = stream.readline(MAX_ROW_BYTES + 1)
                if len(line) > MAX_ROW_BYTES:
                    raise ValueError('Input row exceeds 16 MiB')
                position = offset + len(line)
                yield self.loader(line)['data']


class RowEmitter:
    def __init__(self, output):
        self.output = output
        self.count = 0
        self.written = 0

    def __call__(self, row):
        if not isinstance(row, dict):
            raise ValueError('emit expects a dictionary')
        encoded = json.dumps(row, ensure_ascii=False, allow_nan=False, separators=(',', ':'))
        size = len(encoded.encode('utf-8')) + 1
        if size > MAX_ROW_BYTES:
            raise ValueError('Emitted row exceeds 16 MiB')
        self.count += 1
        self.written += size
        if self.count > 5_000_000 or self.written > 512 * 1024 * 1024:
            raise ValueError('Procedure output limit exceeded')
        self.output.write(encoded + '\n')


def execute(job):
    with (job / 'input.ndjson').open('rb') as stream:
        first = stream.readline(1024 * 1024 + 1)
        if len(first) > 1024 * 1024:
            raise ValueError('Metadata exceeds 1 MiB')
        meta = loads(first)
        if meta.get('type') != 'meta' or not isinstance(meta.get('config'), dict):
            raise ValueError('Missing NDJSON metadata')
        config = meta['config']
        with (job / 'output.ndjson').open('w', encoding='utf-8') as output:
            confine(job, memory_mb=min(1536, max(128, int(config.get('worker_memory_mb', 1024)))),
                    cpu_seconds=min(900, max(1, int(config.get('worker_timeout_seconds', 180)))))
            groups = {}
            # Aggregate input can exceed a million dictionaries. Keep compact
            # byte offsets for the large tables, decoding rows only on access.
            # Normalize/compare retain their existing list-based batch contract.
            lazy_kinds = ({'comparison_rows', 'union_rows'} if meta['kind'] == 'aggregate' else
                          {'comparison_rows', 'segments'} if meta['kind'] == 'checks' else set())
            position = stream.tell()
            while True:
                line = stream.readline(MAX_ROW_BYTES + 1)
                if not line:
                    break
                if len(line) > MAX_ROW_BYTES:
                    raise ValueError('Input row exceeds 16 MiB')
                record = loads(line)
                name = record['type']
                if name in lazy_kinds:
                    if name not in groups:
                        groups[name] = NdjsonRows(job / 'input.ndjson', loads)
                    groups[name].offsets.append(position)
                else:
                    groups.setdefault(name, []).append(record['data'])
                position += len(line)
            code = (job / 'procedure.py').read_text(encoding='utf-8-sig')
            kind = meta['kind']
            source_limit = 16 * 1024 * 1024 if kind == 'checks' else 1024 * 1024
            if len(code.encode('utf-8')) > source_limit:
                raise ValueError('Procedure source/bundle exceeds size limit')
            emit = RowEmitter(output)
            expected = config.get('procedure_kind') if kind == 'validate' else kind
            if kind == 'checks':
                execute_checks(loads(code), groups.get('comparison_rows', []), groups.get('segments', []), emit)
                print('Emitted check rows: ' + str(emit.count), flush=True)
                return
            if kind == 'validate' and expected == 'check':
                check, entry, defaults = load_check(code, str(job / 'procedure.py'))
                emit({'valid': True, 'id': check['id'], 'label': check['label'], 'params': defaults})
                return
            arities = {'normalize': 3, 'compare': 3, 'aggregate': 5}
            if expected not in arities:
                raise ValueError('Unknown stage: ' + str(expected))
            tree = ast.parse(code, filename=str(job / 'procedure.py'))
            functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == expected]
            if not functions:
                raise ValueError('Procedure must define ' + expected)
            args = functions[-1].args
            positional = len(args.posonlyargs) + len(args.args)
            minimum = positional - len(args.defaults)
            if minimum > arities[expected] or (args.vararg is None and positional < arities[expected]) or any(x is None for x in args.kw_defaults):
                raise ValueError('Incorrect interface for ' + expected)
            if kind == 'validate':
                output.write('{"valid":true}\n')
                return
            namespace = {'__name__': 'user_procedure', '__file__': str(job / 'procedure.py')}
            exec(compile(tree, namespace['__file__'], 'exec'), namespace)
            entry = namespace.get(kind)
            if not callable(entry):
                raise ValueError('Procedure must define ' + kind)
            inspect.signature(entry).bind(*([None] * arities[kind]))
            if kind == 'normalize':
                records = groups.get('records', [])
                for row in records:
                    if 'raw' in row:
                        row['document'] = loads(row['raw'], parse_float=Decimal)
                result = entry(records, config, emit)
            elif kind == 'compare':
                union = groups.get('union_rows', [])
                if not union:
                    union = [dict(r, SOURCE='BEFORE') for r in groups.get('before', [])]
                    union += [dict(r, SOURCE='AFTER') for r in groups.get('after', [])]
                # Optional context for procedures emitting replacement segment records.
                config['application_segments'] = groups.get('segments', [])
                result = entry(union, config, emit)
            else:
                result = entry(groups.get('comparison_rows', []), groups.get('union_rows', []), groups.get('segments', []), config, emit)
            if result is not None:
                raise ValueError('Procedure must emit rows and return None')
    print('Emitted rows: ' + str(emit.count), flush=True)


if __name__ == '__main__':
    try:
        execute(Path(sys.argv[1]).resolve())
    except Exception:
        traceback.print_exc()
        sys.exit(1)
