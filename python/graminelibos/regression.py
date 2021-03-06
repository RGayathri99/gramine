import contextlib
import os
import pathlib
import signal
import subprocess
import sys
import unittest

import graminelibos

fspath = getattr(os, 'fspath', str) # pylint: disable=invalid-name

# pylint: disable=subprocess-popen-preexec-fn,subprocess-run-check

HAS_SGX = os.environ.get('SGX') == '1'
ON_X86 = os.uname().machine in ['x86_64']

def expectedFailureIf(predicate):
    if predicate:
        return unittest.expectedFailure
    return lambda func: func

class RegressionTestCase(unittest.TestCase):
    DEFAULT_TIMEOUT = (20 if HAS_SGX else 10)

    def get_env(self, name):
        try:
            return os.environ[name]
        except KeyError:
            self.fail('environment variable {} not set'.format(name))

    @property
    def pal_path(self):
        # pylint: disable=protected-access
        return pathlib.Path(graminelibos._CONFIG_PKGLIBDIR) / ('sgx' if HAS_SGX else 'direct')

    @property
    def libpal_path(self):
        return self.pal_path / 'libpal.so'

    @property
    def loader_path(self):
        return self.pal_path / 'loader'

    def has_debug(self):
        p = subprocess.run(['objdump', '-x', fspath(self.libpal_path)],
            check=True, stdout=subprocess.PIPE)
        dump = p.stdout.decode()
        return '.debug_info' in dump

    def run_gdb(self, args, gdb_script, **kwds):
        prefix = ['gdb', '-q']
        env = os.environ.copy()
        if HAS_SGX:
            prefix += ['-x', fspath(self.pal_path / 'gdb_integration/gramine_sgx_gdb.py')]
            sgx_gdb = fspath(self.pal_path / 'gdb_integration/sgx_gdb.so')
            env['LD_PRELOAD'] = sgx_gdb + ':' + env.get('LD_PRELOAD', '')
        else:
            prefix += ['-x', fspath(self.pal_path / 'gdb_integration/gramine_linux_gdb.py')]

        # Override TTY, as apparently os.setpgrp() confuses GDB and causes it to hang.
        prefix += ['-x', gdb_script, '-batch', '-tty=/dev/null']
        prefix += ['--args']

        return self.run_binary(args, prefix=prefix, env=env, **kwds)

    def run_binary(self, args, *, timeout=None, prefix=None, **kwds):
        timeout = (max(self.DEFAULT_TIMEOUT, timeout) if timeout is not None
            else self.DEFAULT_TIMEOUT)

        if not self.loader_path.exists():
            self.fail('loader ({}) not found'.format(self.loader_path))
        if not self.libpal_path.exists():
            self.fail('libpal ({}) not found'.format(self.libpal_path))

        if prefix is None:
            prefix = []

        with subprocess.Popen(
                [*prefix, fspath(self.loader_path), fspath(self.libpal_path), 'init', *args],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                preexec_fn=os.setpgrp,
                **kwds) as process:
            try:
                stdout, stderr = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                self.fail('timeout ({} s) expired'.format(timeout))

            self.print_output(stdout, stderr)

            if process.returncode:
                raise subprocess.CalledProcessError(
                    process.returncode, args, stdout, stderr)

        return stdout.decode(), stderr.decode()

    @classmethod
    def run_native_binary(cls, args, timeout=None, libpath=None, **kwds):
        timeout = (max(cls.DEFAULT_TIMEOUT, timeout) if timeout is not None
            else cls.DEFAULT_TIMEOUT)

        my_env = os.environ.copy()
        if not libpath is None:
            my_env["LD_LIBRARY_PATH"] = libpath

        with subprocess.Popen(args,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=my_env,
                preexec_fn=os.setpgrp,
                **kwds) as process:
            try:
                stdout, stderr = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                raise AssertionError('timeout ({} s) expired'.format(timeout))

            cls.print_output(stdout, stderr)

            if process.returncode:
                raise subprocess.CalledProcessError(
                    process.returncode, args, stdout, stderr)

        return stdout.decode(), stderr.decode()

    @staticmethod
    def print_output(stdout: bytes, stderr: bytes):
        '''
        Print command output (stdout, stderr) so that pytest can capture it.
        '''

        sys.stdout.write(stdout.decode(errors='surrogateescape'))
        sys.stderr.write(stderr.decode(errors='surrogateescape'))

    @contextlib.contextmanager
    def expect_returncode(self, returncode):
        if returncode == 0:
            raise ValueError('expected returncode should be nonzero')
        try:
            yield
            self.fail('did not fail (expected {})'.format(returncode))
        except subprocess.CalledProcessError as e:
            self.assertEqual(e.returncode, returncode,
                'failed with returncode {} (expected {})'.format(
                    e.returncode, returncode))
