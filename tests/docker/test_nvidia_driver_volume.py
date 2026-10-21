"""Startup policy regressions; no Docker daemon, NVIDIA driver or network needed."""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import urllib.parse

SCRIPT = Path(__file__).resolve().parents[2] / "docker/nvidia-driver-volume.py"
spec = importlib.util.spec_from_file_location("driver_volume", SCRIPT)
manager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manager)
VERSION = "580.178.04"

class FakeDocker:
    def __init__(self, volumes=(), valid=(), fail_new=False):
        self.volumes = list(volumes)
        self.valid = set(valid)
        self.fail_new = fail_new
        self.requests = []
        self.helpers = []
        self.builds = []

    def request(self, method, path, body=None, missing_ok=False):
        self.requests.append((method, path, body))
        if method == "GET" and path.startswith("/volumes?"):
            return {"Volumes": [{"Name": name} for name in self.volumes]}
        if method == "GET" and path.startswith("/volumes/"):
            return {"Name": path.removeprefix("/volumes/")} if path.removeprefix("/volumes/") in self.volumes else None
        if method == "POST" and path == "/volumes/create":
            self.volumes.append(body["Name"])
            return body
        if method == "DELETE" and path.startswith("/volumes/"):
            self.volumes.remove(path.removeprefix("/volumes/"))
            return None
        raise AssertionError((method, path))

    def helper(self, image, volume, command, readonly):
        self.helpers.append((image, volume, readonly))
        if not readonly and not self.fail_new:
            self.valid.add(volume)
        return volume in self.valid

    def build(self, version):
        self.builds.append(version)
        return "driver-package:" + version

class VolumeManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for name, elf_class in manager.required_files(VERSION).items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"\x7fELF" + bytes([elf_class]) + name.encode())
        for name in ("share/glvnd/egl_vendor.d/10_nvidia.json", "share/vulkan/icd.d/nvidia_icd.json"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{}")
        (self.root / "lib/libGLX_nvidia.so.0").symlink_to("libGLX_nvidia.so." + VERSION)
        manager.seal(self.root, VERSION)

    def tearDown(self):
        self.temp.cleanup()

    def test_complete_package_passes(self):
        manager.validate(self.root, VERSION)

    def test_loaded_driver_upgrade_rejects_old_package(self):
        with self.assertRaises(ValueError):
            manager.validate(self.root, "581.1.2")

    def test_compat32_missing_is_rejected(self):
        (self.root / ("lib32/libGLX_nvidia.so." + VERSION)).unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            manager.validate(self.root, VERSION)

    def test_compat32_wrong_architecture_is_rejected(self):
        path = self.root / ("lib32/libGLX_nvidia.so." + VERSION)
        path.write_bytes(b"\x7fELF\x02wrong-architecture")
        with self.assertRaises(ValueError):
            manager.validate(self.root, VERSION)

    def test_corrupted_payload_is_rejected(self):
        path = self.root / ("lib/libnvidia-encode.so." + VERSION)
        path.write_bytes(path.read_bytes() + b"damaged")
        with self.assertRaises(ValueError):
            manager.validate(self.root, VERSION)

    def test_changed_soname_link_is_rejected(self):
        path = self.root / "lib/libGLX_nvidia.so.0"
        path.unlink()
        path.symlink_to("libEGL_nvidia.so." + VERSION)
        with self.assertRaises(ValueError):
            manager.validate(self.root, VERSION)

class StartupPolicyTests(unittest.TestCase):
    def test_amd_intel_only_never_contacts_docker(self):
        with patch.dict(manager.os.environ, {}, clear=True), patch.object(manager, "loaded_version", return_value=None), \
             patch.object(manager, "Docker", side_effect=AssertionError("NVIDIA work on non-NVIDIA host")):
            self.assertEqual(manager.startup(), "")

    def test_toolkit_opt_out_never_provisions(self):
        with patch.dict(manager.os.environ, {"WOLF_NVIDIA_DRIVER_MODE": "toolkit"}, clear=True), \
             patch.object(manager, "Docker", side_effect=AssertionError("Unexpected provisioning")):
            self.assertEqual(manager.startup(), "")

    def test_verified_cache_reused_without_build_or_volume_mutation(self):
        docker = FakeDocker(["old-good"], ["old-good"])
        self.assertEqual(manager.select_volume(docker, VERSION, "wolf"), "old-good")
        self.assertFalse(docker.builds)
        self.assertTrue(all(method == "GET" for method, _, _ in docker.requests))
        self.assertTrue(all(readonly for _, _, readonly in docker.helpers))

    def test_corrupt_cache_replaced_without_overwriting_or_deleting_old_volume(self):
        docker = FakeDocker(["old-broken"])
        selected = manager.select_volume(docker, VERSION, "wolf")
        self.assertNotEqual(selected, "old-broken")
        self.assertIn("old-broken", docker.volumes)
        self.assertEqual(docker.builds, [VERSION])
        self.assertTrue(all(name != "old-broken" for _, name, readonly in docker.helpers if not readonly))

    def test_new_driver_version_gets_separate_volume(self):
        docker = FakeDocker()
        selected = manager.select_volume(docker, "581.1.2", "wolf")
        self.assertTrue(selected.startswith("wolf-nvidia-driver-581.1.2-"))
        self.assertEqual(docker.builds, ["581.1.2"])

    def test_failed_creation_removes_only_unpublished_candidate(self):
        docker = FakeDocker(["old-broken"], fail_new=True)
        with self.assertRaises(RuntimeError):
            manager.select_volume(docker, VERSION, "wolf")
        self.assertEqual(docker.volumes, ["old-broken"])
        deleted = [path for method, path, _ in docker.requests if method == "DELETE"]
        self.assertEqual(len(deleted), 1)
        self.assertNotEqual(deleted[0], "/volumes/old-broken")

    def test_invalid_explicit_pin_is_rejected_without_replacing_it(self):
        docker = FakeDocker(["operator-volume"])
        with self.assertRaises(RuntimeError):
            manager.select_volume(docker, VERSION, "wolf", "operator-volume")
        self.assertEqual(docker.volumes, ["operator-volume"])
        self.assertFalse(docker.builds)
        self.assertTrue(all(method == "GET" for method, _, _ in docker.requests))

    def test_version_and_volume_name_cannot_inject_build_or_mount_arguments(self):
        for version in ("580.178.04; reboot", "../580.178.04", "580.178.04\n"):
            with self.assertRaises(ValueError):
                manager.select_volume(FakeDocker(), version, "wolf")
        with self.assertRaises(ValueError):
            manager.select_volume(FakeDocker(), VERSION, "wolf", "volume:/unexpected")

class InstallerDownloadTests(unittest.TestCase):
    def run_download(self, primary_status, tesla_status):
        # Run the actual Dockerfile download expression, with no network or installer execution.
        source = SCRIPT.with_name("nvidia-driver.Dockerfile").read_text().replace("\\\n", "")
        download = re.search(r"&& (\(curl.*?)\s*&& sh /tmp/nvidia\.run", source, re.DOTALL)
        self.assertIsNotNone(download)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            curl = root / "curl"
            curl.write_text("#!" + sys.executable + "\n" + '''import os, sys
from pathlib import Path
url = next(arg for arg in sys.argv[1:] if arg.startswith("https://"))
kind = "primary" if "/XFree86/" in url else "tesla"
with Path(os.environ["CURL_LOG"]).open("a") as log:
    log.write(url + "\\n")
status = int(os.environ[kind.upper() + "_STATUS"])
Path(sys.argv[sys.argv.index("-o") + 1]).write_text(kind if status == 0 else "partial")
sys.exit(status)
''')
            curl.chmod(0o755)
            output, marker, log = root / "nvidia.run", root / "installer-reached", root / "curl.log"
            command = download.group(1).replace("/tmp/nvidia.run", str(output))
            result = subprocess.run(["/bin/sh", "-c", command + ' && touch "$INSTALLER_MARKER"'],
                                    env={**os.environ, "PATH": str(root) + os.pathsep + os.environ.get("PATH", ""),
                                         "NV_VERSION": "615.71.10", "PRIMARY_STATUS": str(primary_status),
                                         "TESLA_STATUS": str(tesla_status), "CURL_LOG": str(log),
                                         "INSTALLER_MARKER": str(marker)}, capture_output=True, text=True)
            return result.returncode, log.read_text().splitlines(), output.read_text(), marker.exists()

    def test_primary_success_does_not_request_tesla(self):
        status, urls, payload, installer = self.run_download(0, 22)
        self.assertEqual(status, 0)
        self.assertEqual(urls, ["https://download.nvidia.com/XFree86/Linux-x86_64/615.71.10/NVIDIA-Linux-x86_64-615.71.10.run"])
        self.assertEqual(payload, "primary")
        self.assertTrue(installer)

    def test_primary_404_uses_same_version_tesla_payload(self):
        status, urls, payload, installer = self.run_download(22, 0)
        self.assertEqual(status, 0)
        self.assertEqual(len(urls), 2)
        self.assertEqual(urls[1], "https://us.download.nvidia.com/tesla/615.71.10/NVIDIA-Linux-x86_64-615.71.10.run")
        self.assertTrue(all("/615.71.10/" in url and url.endswith("-615.71.10.run") for url in urls))
        self.assertEqual(payload, "tesla")
        self.assertTrue(installer)

    def test_both_downloads_fail_without_running_installer_or_using_old_version(self):
        status, urls, _, installer = self.run_download(22, 22)
        self.assertNotEqual(status, 0)
        self.assertEqual(len(urls), 2)
        self.assertTrue(all("/615.71.10/" in url and url.endswith("-615.71.10.run") for url in urls))
        self.assertFalse(installer)

class DockerBuildTests(unittest.TestCase):
    @staticmethod
    def connection(status=200, body=None, events=()):
        response = MagicMock()
        response.status = status
        response.read.return_value = b"" if body is None else json.dumps(body).encode()
        response.__iter__.return_value = iter(json.dumps(event).encode() + b"\n" for event in events)
        connection = MagicMock()
        connection.getresponse.return_value = response
        return connection

    def test_build_removes_failed_intermediates_and_passes_only_matching_version(self):
        connections = [self.connection(404), self.connection(events=[{"stream": "built\n"}]),
                       self.connection(body={"Id": "driver-image"})]
        with patch.object(manager, "UnixHTTP", side_effect=connections):
            image = manager.Docker("/unused.sock").build(VERSION)
        self.assertEqual(image, "wolf-nvidia-driver:" + VERSION + "-v" + manager.SCHEMA)
        method, path, *_ = connections[1].request.call_args.args
        self.assertEqual(method, "POST")
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(path).query)
        self.assertEqual(query["rm"], ["1"])
        self.assertEqual(query["forcerm"], ["1"])
        self.assertEqual(json.loads(query["buildargs"][0]), {"NV_VERSION": VERSION})
        self.assertTrue(all(connection.close.called for connection in connections))

    def test_failed_build_does_not_create_candidate_volume(self):
        connections = [self.connection(body={"Volumes": []}), self.connection(404),
                       self.connection(events=[{"error": "curl exited 22"}])]
        with patch.object(manager, "UnixHTTP", side_effect=connections):
            with self.assertRaisesRegex(RuntimeError, "Driver package build failed: curl exited 22"):
                manager.select_volume(manager.Docker("/unused.sock"), VERSION, "wolf")
        calls = [connection.request.call_args.args for connection in connections]
        self.assertFalse(any(path == "/volumes/create" for _, path, *_ in calls))
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(calls[-1][1]).query)
        self.assertEqual(query["forcerm"], ["1"])
        self.assertTrue(all(connection.close.called for connection in connections))

if __name__ == "__main__":
    unittest.main()
