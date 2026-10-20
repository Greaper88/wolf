"""Startup policy regressions; no Docker daemon, NVIDIA driver or network needed."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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

if __name__ == "__main__":
    unittest.main()
