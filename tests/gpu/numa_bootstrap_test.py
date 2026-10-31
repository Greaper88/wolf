#!/usr/bin/env python3
"""Exercise the real, statically linked guest launcher without an app image."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest


BOOTSTRAP = str(Path(sys.argv.pop(1)).resolve())
PROBE = r"""
import ctypes, json, mmap, os, sys
allocation = mmap.mmap(-1, 1024 * 1024, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
for offset in range(0, len(allocation), mmap.PAGESIZE):
    allocation[offset] = 1
address = ctypes.addressof(ctypes.c_char.from_buffer(allocation))
start = None
for line in open('/proc/self/maps'):
    low, high = (int(part, 16) for part in line.split()[0].split('-'))
    if low <= address < high:
        start = low
        break
policy = None
for line in open('/proc/self/numa_maps'):
    fields = line.split()
    if int(fields[0], 16) == start:
        policy = fields[1]
        break
print(json.dumps(dict(cpus=sorted(os.sched_getaffinity(0)), policy=policy,
                      pid=os.getpid(), cwd=os.getcwd(), marker=os.getenv('NUMA_TEST_MARKER'),
                      argv=sys.argv[1:])))
"""


def parse_list(value):
    result = set()
    for item in value.split(","):
        bounds = item.strip().split("-")
        result.update(range(int(bounds[0]), int(bounds[-1]) + 1))
    return sorted(result)


class GuestNumaBootstrap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.allowed_cpus = sorted(os.sched_getaffinity(0))
        memory = next(line.split(":", 1)[1].strip() for line in Path("/proc/self/status").read_text().splitlines()
                      if line.startswith("Mems_allowed_list:"))
        for node in parse_list(memory):
            local_path = Path(f"/sys/devices/system/node/node{node}/cpulist")
            local = parse_list(local_path.read_text()) if local_path.exists() else cls.allowed_cpus
            cls.local_cpus = sorted(set(local) & set(cls.allowed_cpus))
            if cls.local_cpus:
                cls.node = node
                break
        else:
            raise unittest.SkipTest("No allowed CPUs on an allowed memory node")

    def command(self, *args):
        return [BOOTSTRAP, str(self.node), str(self.local_cpus[0]), "--", *args]

    def probe(self):
        result = subprocess.run(self.command(sys.executable, "-c", PROBE),
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout), result.stderr

    def test_limits_cpus_to_local_selection(self):
        result, diagnostic = self.probe()
        self.assertEqual(result["cpus"], self.local_cpus[:1])
        self.assertIn("CPUs bound locally", diagnostic)

    def test_preferred_memory_policy_survives_exec(self):
        result, diagnostic = self.probe()
        if "set_mempolicy failed" in diagnostic:
            self.skipTest("Memory policy syscall is unavailable in this test environment")
        self.assertTrue(result["policy"].startswith("prefer"), result)
        self.assertEqual(result["policy"].split(":")[-1], str(self.node))

    def test_unavailable_memory_preference_keeps_local_cpus_and_executes(self):
        # A valid launcher argument outside Linux's possible NUMA nodes forces
        # memory-policy failure independently of the Docker seccomp profile.
        result = subprocess.run([BOOTSTRAP, "1048575", str(self.local_cpus[0]), "--",
                                 sys.executable, "-c", PROBE], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["cpus"], self.local_cpus[:1])
        self.assertIn("set_mempolicy failed", result.stderr)
        self.assertIn("preference unavailable", result.stderr)

    def test_descendants_inherit_cpu_and_memory_policy(self):
        source = "import subprocess, sys; subprocess.run([sys.executable, '-c', sys.argv[1]], check=True)"
        result = subprocess.run(self.command(sys.executable, "-c", source, PROBE),
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        descendant = json.loads(result.stdout)
        self.assertEqual(descendant["cpus"], self.local_cpus[:1])
        if "set_mempolicy failed" in result.stderr:
            self.skipTest("Memory policy syscall is unavailable in this test environment")
        self.assertTrue(descendant["policy"].startswith("prefer"), descendant)
        self.assertEqual(descendant["policy"].split(":")[-1], str(self.node))

    def test_unavailable_local_cpus_do_not_execute_payload(self):
        excluded = max(self.allowed_cpus) + 1
        result = subprocess.run([BOOTSTRAP, str(self.node), str(excluded), "--",
                                 sys.executable, "-c", "print('must not run')"],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 125)
        self.assertEqual(result.stdout, "")
        self.assertIn("refusing to execute", result.stderr)

    def test_exec_preserves_pid_arguments_environment_and_working_directory(self):
        with tempfile.TemporaryDirectory() as working:
            args = ["space in argument", "literal $() ; *"]
            process = subprocess.Popen(self.command(sys.executable, "-c", PROBE, *args),
                                       cwd=working, env={**os.environ, "NUMA_TEST_MARKER": "preserved"},
                                       text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            output, diagnostic = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, diagnostic)
            result = json.loads(output)
            self.assertEqual(result["pid"], process.pid)
            self.assertEqual(result["cwd"], working)
            self.assertEqual(result["marker"], "preserved")
            self.assertEqual(result["argv"], args)

    def test_exit_status_and_signals_reach_original_program(self):
        result = subprocess.run(self.command(sys.executable, "-c", "raise SystemExit(37)"),
                                capture_output=True)
        self.assertEqual(result.returncode, 37)
        source = "import signal, time; signal.signal(signal.SIGTERM, lambda *_: exit(23)); print('ready', flush=True); time.sleep(10)"
        process = subprocess.Popen(self.command(sys.executable, "-c", source),
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(process.stdout.readline().strip(), "ready")
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 23)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_invalid_arguments_do_not_execute_payload(self):
        for node, cpus in [("invalid", "0"), ("-1", "0"), ("0", ""), ("0", "3-1"),
                           ("0", "0,"), ("0", "1048576")]:
            with self.subTest(node=node, cpus=cpus):
                result = subprocess.run([BOOTSTRAP, node, cpus, "--", sys.executable,
                                         "-c", "print('must not run')"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 125)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
