"""Linux filesystem + syscall confinement, applied before loading user source.

Fail closed: unsupported Landlock kernels never execute untrusted procedures.
The daemon container also has no network, no capabilities, and a read-only root.
"""
import ctypes
import errno
import os
from pathlib import Path
import resource


def confine(job, memory_mb=1024, cpu_seconds=180):
    resource.setrlimit(resource.RLIMIT_AS, (memory_mb * 1024 * 1024,) * 2)
    resource.setrlimit(resource.RLIMIT_CPU, (cpu_seconds, cpu_seconds + 1))
    resource.setrlimit(resource.RLIMIT_FSIZE, (512 * 1024 * 1024,) * 2)
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    libc = ctypes.CDLL(None, use_errno=True)
    # Linux x86_64 / aarch64 use the same Landlock syscall numbers.
    abi = libc.syscall(444, 0, 0, 1)
    if abi < 1:
        raise RuntimeError('Linux Landlock is required; untrusted code was not executed')
    execute, write, read, read_dir = 1, 2, 4, 8
    handled = (1 << 13) - 1
    if abi >= 2:
        handled |= 1 << 13  # REFER
    if abi >= 3:
        handled |= 1 << 14  # TRUNCATE
    class Ruleset(ctypes.Structure):
        _fields_ = [('handled_access_fs', ctypes.c_uint64)]
    class PathRule(ctypes.Structure):
        _pack_ = 1
        _fields_ = [('allowed_access', ctypes.c_uint64), ('parent_fd', ctypes.c_int32)]
    ruleset = Ruleset(handled)
    fd = libc.syscall(444, ctypes.byref(ruleset), ctypes.sizeof(ruleset), 0)
    if fd < 0:
        raise OSError(ctypes.get_errno(), 'landlock_create_ruleset')
    try:
        paths = [(str(job), read | read_dir), ('/usr', execute | read | read_dir),
                 ('/lib', execute | read | read_dir), ('/lib64', execute | read | read_dir),
                 ('/app', read | read_dir), ('/etc/ld.so.cache', read),
                 ('/dev/null', read | write), ('/dev/urandom', read)]
        for name, access in paths:
            path = Path(name)
            if not path.exists():
                continue
            parent = os.open(str(path), os.O_PATH | os.O_CLOEXEC)
            try:
                rule = PathRule(access, parent)
                if libc.syscall(445, fd, 1, ctypes.byref(rule), 0) != 0:
                    raise OSError(ctypes.get_errno(), 'landlock_add_rule: ' + name)
            finally:
                os.close(parent)
        if libc.prctl(38, 1, 0, 0, 0) != 0 or libc.syscall(446, fd, 0) != 0:
            raise OSError(ctypes.get_errno(), 'landlock_restrict_self')
    finally:
        os.close(fd)
    seccomp = ctypes.CDLL('libseccomp.so.2', use_errno=True)
    seccomp.seccomp_init.argtypes = [ctypes.c_uint32]
    seccomp.seccomp_init.restype = ctypes.c_void_p
    seccomp.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    seccomp.seccomp_syscall_resolve_name.restype = ctypes.c_int
    seccomp.seccomp_rule_add.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint]
    seccomp.seccomp_load.argtypes = [ctypes.c_void_p]
    seccomp.seccomp_release.argtypes = [ctypes.c_void_p]
    context = seccomp.seccomp_init(0x7FFF0000)
    if not context:
        raise RuntimeError('seccomp_init failed')
    try:
        blocked = ('socket', 'socketpair', 'connect', 'bind', 'listen', 'accept', 'accept4',
                   'execve', 'execveat', 'fork', 'vfork', 'clone', 'clone3', 'ptrace',
                   'process_vm_readv', 'process_vm_writev', 'mount', 'umount2', 'bpf',
                   'kill', 'tkill', 'tgkill', 'setsid', 'setpgid', 'open_by_handle_at')
        for name in blocked:
            number = seccomp.seccomp_syscall_resolve_name(name.encode())
            if number >= 0 and seccomp.seccomp_rule_add(context, 0x50000 | errno.EPERM, number, 0) != 0:
                raise RuntimeError('Cannot restrict syscall ' + name)
        if seccomp.seccomp_load(context) != 0:
            raise RuntimeError('seccomp_load failed')
    finally:
        seccomp.seccomp_release(context)
