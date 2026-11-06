"""Export one completed run to the real isolated aggregate worker, then verify facts.

Linux host usage:
  python3 tests/integration_worker_large.py --compose docker-compose.yml
No database mutation or full pipeline replay. The random /jobs directory is retained
for diagnosis; remove it explicitly after collecting the probe results.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compose', required=True)
    parser.add_argument('--run-id')
    parser.add_argument('--memory-mb', type=int, default=1024)
    parser.add_argument('--timeout', type=int, default=900)
    args = parser.parse_args()
    base = ['docker', 'compose', '-f', str(Path(args.compose).resolve()), 'exec', '-T']

    def command(service, *argv):
        return base + [service, *argv]

    def run(service, *argv, **kwargs):
        return subprocess.run(command(service, *argv), check=True, **kwargs)

    def sql_command(sql):
        return command('postgres', 'psql', '-X', '-U', 'lens', '-d', 'lens', '-v', 'ON_ERROR_STOP=1', '-Atq', '-c', sql)

    def sql_value(sql):
        return subprocess.check_output(sql_command(sql), text=True).strip()

    run_id = args.run_id or sql_value("SELECT id FROM runs WHERE data->>'status'='completed' AND (data->'summary'->>'total_apps')::integer=2002 ORDER BY data->>'created_at' DESC LIMIT 1")
    if not re.fullmatch('[0-9a-f]+', run_id):
        raise ValueError('No valid completed full run found')
    config = json.loads(sql_value("SELECT data->'snapshot'->'config'->'settings' FROM runs WHERE id='" + run_id + "'"))
    config.update(worker_memory_mb=args.memory_mb, worker_timeout_seconds=args.timeout)
    job = '/jobs/' + uuid.uuid4().hex
    metadata = json.dumps({'type': 'meta', 'kind': 'aggregate', 'config': config}) + '\n'
    prepare = "import pathlib,shutil,sys;p=pathlib.Path(sys.argv[1]);p.mkdir();(p/'input.ndjson').write_text(sys.argv[2],encoding='utf-8');src=next(x for x in (pathlib.Path('/app/procedures/aggregate.py'),pathlib.Path('/app/python-worker/procedures/aggregate.py')) if x.exists());shutil.copyfile(src,p/'procedure.py')"
    run('python-worker', 'python3', '-c', prepare, job, metadata)
    print(json.dumps({'run_id': run_id, 'job': job, 'memory_mb': args.memory_mb, 'timeout': args.timeout}), flush=True)
    for record_type, table in [('comparison_rows', 'comparison_values'), ('union_rows', 'normalized_values'), ('segments', 'application_segments')]:
        statement = "SELECT jsonb_build_object('type','" + record_type + "','data',data)::text FROM " + table + " WHERE run_id='" + run_id + "' ORDER BY app_id"
        writer_code = "import pathlib,shutil,sys\nwith pathlib.Path(sys.argv[1]).open('ab') as f: shutil.copyfileobj(sys.stdin.buffer,f)"
        export = subprocess.Popen(sql_command(statement), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        writer = subprocess.Popen(command('python-worker', 'python3', '-c', writer_code, job + '/input.ndjson'), stdin=export.stdout)
        export.stdout.close()
        writer_code_result = writer.wait()
        error = export.stderr.read().decode('utf-8', 'replace')
        if export.wait() or writer_code_result:
            raise RuntimeError('Export failed: ' + error)
        print('Exported ' + record_type, flush=True)
    size_code = "import pathlib,sys;p=pathlib.Path(sys.argv[1]);size=(p/'input.ndjson').stat().st_size;print(size);assert size<=512*1024*1024;(p/'ready.tmp').write_text(sys.argv[2]);(p/'ready.tmp').rename(p/'ready')"
    size = run('python-worker', 'python3', '-c', size_code, job, str(args.timeout), capture_output=True, text=True).stdout.strip()
    print('Input bytes: ' + size, flush=True)
    started = time.monotonic()
    status_code = "import pathlib,sys;p=pathlib.Path(sys.argv[1])/'done.json';print(p.read_text() if p.exists() else '')"
    status = None
    while time.monotonic() - started < args.timeout + 30:
        text = run('python-worker', 'python3', '-c', status_code, job, capture_output=True, text=True).stdout.strip()
        if text:
            status = json.loads(text)
            break
        time.sleep(2)
    logs = run('python-worker', 'python3', '-c', "import pathlib,sys;p=pathlib.Path(sys.argv[1])/'diagnostics.log';print(p.read_text(errors='replace')[-12000:] if p.exists() else '')", job, capture_output=True, text=True).stdout
    print(logs, flush=True)
    if not status or not status['ok']:
        raise RuntimeError('Aggregate worker failed: ' + repr(status))

    def digest(row):
        for name in ('id', 'FACT_ID'):
            row.pop(name, None)
        row['APP_IDS'].sort()
        if row['CNT'] != len(set(row['APP_IDS'])):
            raise AssertionError('Distinct member count mismatch')
        return hashlib.sha256(json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

    output = subprocess.Popen(command('python-worker', 'cat', job + '/output.ndjson'), stdout=subprocess.PIPE, text=True, encoding='utf-8')
    actual = {digest(json.loads(line)) for line in output.stdout}
    if output.wait():
        raise RuntimeError('Cannot read worker facts')
    baseline = subprocess.Popen(sql_command("SELECT data::text FROM aggregate_facts WHERE run_id='" + run_id + "'"), stdout=subprocess.PIPE, text=True, encoding='utf-8')
    expected = {digest(json.loads(line)) for line in baseline.stdout}
    if baseline.wait():
        raise RuntimeError('Cannot read native facts')
    if actual != expected:
        raise AssertionError('Fact mismatch: missing=%d extra=%d' % (len(expected - actual), len(actual - expected)))
    print(json.dumps({'verified_facts': len(actual), 'expected_facts': len(expected), 'elapsed_seconds': round(time.monotonic()-started, 3), 'memory_mb': args.memory_mb, 'job': job}), flush=True)


if __name__ == '__main__':
    main()
