"""Networkless worker supervisor. Only this trusted process sees the job queue."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(os.environ.get('JOBS_ROOT', '/jobs')).resolve()
RUNNER = Path(__file__).with_name('run_stage.py')


def work(job):
    process = None
    try:
        timeout = min(900, max(1, int((job / 'ready').read_text())))
        if (job / 'cancel').exists():
            raise ValueError('CANCELLED')
        if (job / 'input.ndjson').stat().st_size > 512 * 1024 * 1024:
            raise ValueError('Input exceeds 512 MiB')
        with (job / 'diagnostics.log').open('w', encoding='utf-8') as output:
            process = subprocess.Popen([sys.executable, '-I', '-B', str(RUNNER), str(job)], cwd=str(job),
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                       env={'PATH': '/usr/local/bin:/usr/bin:/bin', 'LANG': 'C.UTF-8', 'PYTHONUNBUFFERED': '1'},
                                       start_new_session=True)
            deadline = time.monotonic() + timeout
            reason = None
            while process.poll() is None:
                if (job / 'cancel').exists():
                    reason = 'CANCELLED'
                elif time.monotonic() > deadline:
                    reason = 'Python execution timeout'
                elif (job / 'diagnostics.log').stat().st_size > 1024 * 1024:
                    reason = 'Diagnostic output limit exceeded'
                if reason:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    break
                time.sleep(0.05)
            code = process.wait()
        failure = 'Procedure exited with code ' + str(code) if code else ''
        if code and not reason:
            # Keep the complete traceback in diagnostics and surface its final
            # exception (including CHECK identity) in the run's failure reason.
            with (job / 'diagnostics.log').open('rb') as diagnostics:
                diagnostics.seek(max(0, diagnostics.seek(0, 2) - 8192))
                lines = diagnostics.read().decode('utf-8', errors='replace').splitlines()
                last = next((line.strip() for line in reversed(lines) if line.strip()), '')
                if last:
                    failure += ': ' + last[:2048]
        result = {'ok': code == 0 and not reason, 'error': reason or failure}
    except Exception as exc:
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        result = {'ok': False, 'error': str(exc)}
    temporary = job / 'done.tmp'
    try:
        temporary.write_text(json.dumps(result), encoding='utf-8')
        temporary.replace(job / 'done.json')
    except FileNotFoundError:
        pass  # Backend may already have discarded a cancelled job.


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    print('Python worker ready; network disabled; Landlock + seccomp required', flush=True)
    running = {}
    with ThreadPoolExecutor(max_workers=2) as pool:
        while True:
            running = {name: task for name, task in running.items() if not task.done()}
            for ready in sorted(ROOT.glob('*/ready')):
                job = ready.parent
                if len(job.name) != 32 or any(c not in "0123456789abcdef" for c in job.name) or job.is_symlink() or (job / 'done.json').exists() or job.name in running:
                    continue
                if len(running) >= 2:
                    break
                running[job.name] = pool.submit(work, job)
            time.sleep(0.1)


if __name__ == '__main__':
    main()
