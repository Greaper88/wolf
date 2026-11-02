"""Installer tests: no package installation, Docker daemon, or real disk writes."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("setup_wolf", ROOT / "scripts/setup_wolf.py")
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


def disk(name="/dev/sdb", **kwargs):
    return {"name": name, "type": "disk", "size": 1000000000000, "fstype": None,
            "uuid": None, "mountpoints": [None], "ro": False, "rm": False,
            "serial": name + "-serial", "model": "Fixture", **kwargs}


def gpu(vendor, pci, node, driver):
    return {"vendor": vendor, "pci": pci, "node": node, "driver": driver}


def plan():
    return {"channel": "stable", "state": "/srv/wolf/apps", "config": "/srv/wolf/config",
            "install": "/opt/wolf", "uid": 1000, "gid": 1000, "libraries": {"roms": "/mnt/games/ROMs"},
            "gpu": {"mode": "mesa", "blacklist": [], "node": None}, "driver_volume": None}


class DistroTests(unittest.TestCase):
    def test_supported_releases(self):
        cases = [("ubuntu", "22.04", "jammy"), ("ubuntu", "24.04", "noble"),
                 ("ubuntu", "26.04", "resolute"), ("debian", "12", "bookworm"),
                 ("debian", "13", "trixie"), ("fedora", "43", ""), ("fedora", "44", ""),
                 ("rhel", "8.10", ""), ("rhel", "9.6", ""), ("rhel", "10.0", ""),
                 ("centos", "9", ""), ("centos", "10", "")]
        for distro, version, codename in cases:
            with self.subTest(distro=distro, version=version):
                result = setup.distro_info({"ID": distro, "VERSION_ID": version,
                                           "VERSION_CODENAME": codename, "PRETTY_NAME": "CentOS Stream"}, "x86_64")
                self.assertEqual(result["id"], distro)

    def test_derivatives_need_explicit_known_mapping(self):
        result = setup.distro_info({"ID": "linuxmint", "VERSION_ID": "22", "VERSION_CODENAME": "wilma",
                                   "UBUNTU_CODENAME": "noble"}, "x86_64")
        self.assertEqual((result["id"], result["codename"], result["derivative"]), ("ubuntu", "noble", True))
        for distro in ("rocky", "almalinux"):
            self.assertEqual(setup.distro_info({"ID": distro, "VERSION_ID": "9.5"}, "x86_64")["id"], "rhel")

    def test_unknown_eol_mismatch_and_arch_rejected(self):
        for release, machine in [({"ID": "arch"}, "x86_64"),
                                 ({"ID": "ubuntu", "VERSION_ID": "20.04", "VERSION_CODENAME": "focal"}, "x86_64"),
                                 ({"ID": "ubuntu", "VERSION_ID": "24.04", "VERSION_CODENAME": "jammy"}, "x86_64"),
                                 ({"ID": "debian", "VERSION_ID": "12", "VERSION_CODENAME": "bookworm"}, "aarch64")]:
            with self.subTest(release=release), self.assertRaises(setup.SetupError):
                setup.distro_info(release, machine)

    def test_os_release_is_parsed_not_executed(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "os-release"
            file.write_text('ID=ubuntu\nPRETTY_NAME="Ubuntu Linux"\nEVIL="$(touch /tmp/never)"\n')
            self.assertEqual(setup.read_os(file)["EVIL"], "$(touch /tmp/never)")


class GpuTests(unittest.TestCase):
    def setUp(self):
        self.amd = gpu("AMD", "0000:03:00.0", "/dev/dri/renderD128", "amdgpu")
        self.nv = gpu("NVIDIA", "0000:04:00.0", "/dev/dri/renderD129", "nvidia")

    def test_mixed_decline_blacklists_all_nvidia_by_pci(self):
        second = gpu("NVIDIA", "0000:05:00.0", None, None)
        result = setup.gpu_plan([self.amd, self.nv, second])
        self.assertEqual(result["blacklist"], ["0000:04:00.0", "0000:05:00.0"])
        cfg = plan()
        cfg["gpu"] = result
        environment = setup.compose_document(cfg)["services"]["wolf"]["environment"]
        self.assertEqual(environment["WOLF_GPU_AUTO_SELECT"], "true")
        self.assertNotIn("WOLF_RENDER_NODE", environment)
        self.assertEqual(environment["WOLF_GPU_BLACKLIST"], "0000:04:00.0,0000:05:00.0")

    def test_nvidia_legacy_and_versioned_external_volume(self):
        cfg = plan()
        cfg["gpu"] = setup.gpu_plan([self.amd, self.nv], True)
        cfg["driver_volume"] = "wolf-nvidia-580.95.05"
        document = setup.compose_document(cfg, ["/dev/nvidia0", "/dev/nvidiactl"])
        environment = document["services"]["wolf"]["environment"]
        self.assertEqual(environment["WOLF_GPU_AUTO_SELECT"], "false")
        self.assertEqual(environment["WOLF_ENCODER_NODE"], self.nv["node"])
        self.assertEqual(document["volumes"]["nvidia-driver"]["name"], cfg["driver_volume"])
        self.assertTrue(document["volumes"]["nvidia-driver"]["external"])
        self.assertIn("/dev/nvidiactl:/dev/nvidiactl", document["services"]["wolf"]["devices"])

    def test_missing_or_nouveau_driver_rejected(self):
        for driver in (None, "nouveau"):
            self.nv["driver"] = driver
            with self.assertRaises(setup.SetupError):
                setup.gpu_plan([self.nv], True)
        with self.assertRaises(setup.SetupError):
            setup.gpu_plan([self.nv], False)

    def test_cuda_uuid_matches_selected_card_not_device_zero(self):
        with patch.object(setup, "output", return_value=(
                "00000000:03:00.0, GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\n"
                "00000000:04:00.0, GPU-11111111-2222-3333-4444-555555555555")):
            self.assertEqual(setup.nvidia_uuid("0000:04:00.0"), "GPU-11111111-2222-3333-4444-555555555555")
            with self.assertRaises(setup.SetupError):
                setup.nvidia_uuid("0000:06:00.0")

    def test_changed_driver_aborts_before_build(self):
        with patch.object(setup, "nvidia_version", return_value="590.1"), patch.object(setup, "run") as command:
            with self.assertRaises(setup.SetupError):
                setup.build_nvidia({"driver_version": "580.1"})
            command.assert_not_called()

    def test_sysfs_discovery_ignores_nvidia_audio_function(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for pci, pci_class in [("0000:03:00.0", "0x030000"), ("0000:03:00.1", "0x040300")]:
                path = root / "bus/pci/devices" / pci
                (path / "drm/renderD128").mkdir(parents=True)
                (path / "class").write_text(pci_class)
                (path / "vendor").write_text("0x10de")
            result = setup.detect_gpus(root)
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]["pci"], "0000:03:00.0")


class ConfigTests(unittest.TestCase):
    def test_both_channels_and_every_app_library_mount(self):
        template = (ROOT / setup.DEFAULT_CONFIG).read_text()
        original = setup.tomllib.loads(template)
        libraries = {"steam": "/games/SteamLibrary", "roms": "/games/ROMs", "installers": "/games/Installers"}
        for channel, (_, tag) in setup.CHANNELS.items():
            text, images = setup.configure_apps(template, channel, libraries, 'Wolf "β"')
            document = setup.tomllib.loads(text)
            self.assertEqual(document["gstreamer"], original["gstreamer"])
            self.assertEqual(document["hostname"], 'Wolf "β"')
            self.assertIn("ghcr.io/greaper88/wolf-ui:" + tag, images)
            self.assertEqual(len(images), 10)
            for profile in document["profiles"]:
                for app in profile.get("apps", []):
                    runner = app.get("runner", {})
                    if runner.get("type") == "docker":
                        self.assertIn("/games/ROMs:/ROMs:rw", runner["mounts"])
                        self.assertIn("/games/Installers:/Installers:rw", runner["mounts"])
                        self.assertIn("/games/SteamLibrary:/SteamLibrary:rw", runner["mounts"])
                        if app["title"] == "Steam":
                            self.assertIn("/games/SteamLibrary/steamapps:/home/retro/.steam/debian-installation/steamapps:rw", runner["mounts"])

    def test_no_libraries_preserves_app_mounts(self):
        template = (ROOT / setup.DEFAULT_CONFIG).read_text()
        text, _ = setup.configure_apps(template, "stable", {}, "Wolf")
        self.assertEqual(setup.tomllib.loads(text)["profiles"], setup.tomllib.loads(template)["profiles"])

    def test_selinux_app_settings_preserve_existing_security_options(self):
        template = (ROOT / setup.DEFAULT_CONFIG).read_text()
        text, _ = setup.configure_apps(template, "stable", {}, "Wolf", selinux=True)
        for profile in setup.tomllib.loads(text)["profiles"]:
            for app in profile.get("apps", []):
                if app["runner"]["type"] != "docker":
                    continue
                create = json.loads(app["runner"]["base_create_json"])
                self.assertIn("label=disable", create["HostConfig"]["SecurityOpt"])
                if app["title"] == "Steam":
                    self.assertIn("seccomp=unconfined", create["HostConfig"]["SecurityOpt"])

    def test_overlap_rejected_and_default_layout_allowed(self):
        cfg = plan()
        setup.validate_layout(cfg)
        cfg.update(state="/etc/wolf", config="/etc/wolf/cfg")
        setup.validate_layout(cfg)
        cfg["libraries"] = {"steam": "/etc/wolf/cfg", "roms": "/srv/roms"}
        with self.assertRaises(setup.SetupError):
            setup.validate_layout(cfg)
        cfg["libraries"] = {"steam": "/srv/games", "roms": "/srv/games/roms"}
        with self.assertRaises(setup.SetupError):
            setup.validate_layout(cfg)

    def test_systemd_orders_mounts_before_server_and_owns_restart(self):
        cfg = plan()
        unit = setup.service_text(cfg, "/usr/bin/docker")
        self.assertIn("RequiresMountsFor=/mnt/games/ROMs", unit)
        self.assertIn("--host unix:///var/run/docker.sock", unit)
        self.assertIn("--abort-on-container-exit --exit-code-from wolf", unit)
        self.assertEqual(setup.compose_document(cfg)["services"]["wolf"]["restart"], "no")

    def test_path_injection_and_system_roots_rejected(self):
        for path in ("relative", "/tmp/a b", "/tmp/a:b", "/tmp/$HOME", "/tmp/a\nB", "/", "/dev/sda", "/etc", "/srv/a%f"):
            with self.subTest(path=path), self.assertRaises(setup.SetupError):
                setup.valid_path(path)
        self.assertEqual(setup.valid_path("/srv/games"), "/srv/games")

    def test_existing_configuration_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            cfg = plan()
            cfg["config"] = folder
            path = Path(folder) / "config.toml"
            path.write_text("paired clients remain")
            with self.assertRaises(setup.SetupError):
                setup.preflight_existing(cfg)
            self.assertEqual(path.read_text(), "paired clients remain")

    def test_atomic_write_backup_and_symlink_refusal(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config"
            path.write_text("old")
            setup.atomic_write(path, "new", backup=True)
            self.assertEqual(next(Path(folder).glob("*.wolf-backup-*")).read_text(), "old")
            link = Path(folder) / "link"
            link.symlink_to(path)
            with self.assertRaises(setup.SetupError):
                setup.atomic_write(link, "bad")


class StorageTests(unittest.TestCase):
    def test_array_candidates_include_existing_data_but_never_active_disks(self):
        blank = disk()
        data = disk("/dev/sdc", children=[disk("/dev/sdc1", type="part", fstype="ext4", uuid="old-files")])
        root = disk("/dev/sda", children=[disk("/dev/sda1", type="part", mountpoints=["/"])])
        swap = disk("/dev/sdd", fstype="swap", mountpoints=["[SWAP]"])
        mapped = disk("/dev/sde", children=[disk("/dev/dm-0", type="crypt")])
        readonly = disk("/dev/sdf", ro=True)
        self.assertEqual(setup.blank_disks([blank, data, root, swap, mapped, readonly]), [blank])
        self.assertEqual(setup.inactive_disks([blank, data, root, swap, mapped, readonly]), [blank, data])

    def test_drive_identity_changes_detected_in_partitions(self):
        old = disk(children=[disk("/dev/sdb1", type="part", uuid="old")])
        changed = copy.deepcopy(old)
        changed["children"][0]["uuid"] = "new"
        with patch.object(setup, "block_inventory", return_value=[changed]), self.assertRaises(setup.SetupError):
            setup.ensure_unused(old, True)

    def test_raid_member_counts(self):
        for level, count in [("0", 2), ("1", 2), ("5", 3), ("6", 4), ("10", 4)]:
            setup.validate_raid(level, count)
        for level, count in [("0", 1), ("1", 1), ("5", 2), ("6", 3), ("10", 5), ("99", 10)]:
            with self.assertRaises(setup.SetupError):
                setup.validate_raid(level, count)

    def test_declined_confirmation_never_wipes_formats_or_creates_array(self):
        with tempfile.TemporaryDirectory() as folder:
            storage = {"mode": "raid", "devices": [disk(), disk("/dev/sdc")], "level": "1", "mount": folder, "filesystem": "xfs"}
            with patch.object(setup, "prepare_filesystem"), patch.object(setup, "install_packages"), patch.object(setup, "ensure_unused"), \
                    patch.object(setup, "ask", return_value="no"), patch.object(setup, "run") as command, \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(setup.SetupError):
                setup.apply_storage(storage, {"manager": "apt"})
            command.assert_not_called()

    def test_accepted_erase_rechecks_all_drives_before_first_write(self):
        with tempfile.TemporaryDirectory() as folder:
            devices = [disk(), disk("/dev/sdc")]
            storage = {"mode": "raid", "devices": devices, "level": "1", "mount": folder, "filesystem": "xfs"}
            events = []

            def ensure(device, allow_data):
                events.append(("check", device["name"], allow_data))
                # Simulate a device becoming mounted after the user typed ERASE.
                if len(events) == 4:
                    raise setup.SetupError("became mounted")

            with patch.object(setup, "prepare_filesystem"), patch.object(setup, "install_packages"), patch.object(setup, "ensure_unused", side_effect=ensure), \
                    patch.object(setup, "ask", return_value="ERASE /dev/sdb /dev/sdc"), patch.object(setup, "run") as command, \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(setup.SetupError):
                setup.apply_storage(storage, {"manager": "apt"})
            self.assertEqual(len(events), 4)
            command.assert_not_called()

    def test_raid_wipes_child_signatures_before_partition_table(self):
        with tempfile.TemporaryDirectory() as folder:
            devices = [disk(children=[disk("/dev/sdb1", type="part", fstype="ext4")]), disk("/dev/sdc")]
            storage = {"mode": "raid", "devices": devices, "level": "1", "mount": folder, "filesystem": "xfs"}
            calls = []

            def command(*args, **kwargs):
                calls.append(args)
                if args[0] == "mdadm":
                    raise setup.SetupError("stop fixture before actual array creation")

            with patch.object(setup, "prepare_filesystem"), patch.object(setup, "install_packages"), patch.object(setup, "ensure_unused"), \
                    patch.object(setup, "ask", return_value="ERASE /dev/sdb /dev/sdc"), patch.object(setup, "run", side_effect=command), \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(setup.SetupError):
                setup.apply_storage(storage, {"manager": "apt"})
            self.assertEqual(calls[:3], [("wipefs", "--all", "/dev/sdb1"), ("wipefs", "--all", "/dev/sdb"),
                                         ("blockdev", "--rereadpt", "/dev/sdb")])
            self.assertEqual(calls[-1][0:3], ("mdadm", "--create", "/dev/md/wolf"))


class FilesystemTests(unittest.TestCase):
    def test_menu_defaults_to_xfs_and_records_all_four_choices_for_disk_and_array(self):
        self.assertEqual(list(setup.FILESYSTEMS), ["xfs", "f2fs", "ext4", "ntfs"])
        for mode in (2, 3):
            for index, fs_type in enumerate(setup.FILESYSTEMS):
                with self.subTest(mode=mode, fs_type=fs_type), tempfile.TemporaryDirectory() as folder:
                    picks = [mode, index] if mode == 2 else [mode, 0, index]
                    with patch.object(setup, "choose", side_effect=picks) as menu, \
                            patch.object(setup, "ask", return_value="1" if mode == 2 else "1 2"), \
                            patch.object(setup, "path_question", return_value=folder), contextlib.redirect_stdout(io.StringIO()):
                        result = setup.select_storage([disk(), disk("/dev/sdc")])
                    self.assertEqual(result["filesystem"], fs_type)
                    self.assertEqual(result["mode"], "format" if mode == 2 else "raid")
                    self.assertEqual(menu.call_args.args[0], "Filesystem")
                    self.assertIn("not recommended", menu.call_args.args[1][3])
                    self.assertNotIn("default", menu.call_args.kwargs)  # choose defaults to the first entry.

    def test_native_and_array_format_commands(self):
        expected = {"xfs": ("mkfs.xfs", "-L"), "f2fs": ("mkfs.f2fs", "-l"),
                    "ext4": ("mkfs.ext4", "-L"), "ntfs": ("mkntfs", "--quick")}
        for fs_type in expected:
            with self.subTest(fs_type=fs_type), patch.object(setup, "run") as run:
                self.assertEqual(setup.format_filesystem(fs_type, "/dev/md/wolf"), "/dev/md/wolf")
                self.assertEqual(run.call_args.args[:2], expected[fs_type])
                self.assertEqual(run.call_args.args[-1], "/dev/md/wolf")
                self.assertNotIn("--force", run.call_args.args)

    def test_ntfs_disk_gets_verified_gpt_partition_before_format(self):
        node = "/dev/nvme0n1p1"
        table = {"partitiontable": {"partitions": [{"node": node, "type": "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"}]}}
        inventory = {"blockdevices": [disk("/dev/nvme0n1", children=[disk(node, type="part")])]}
        with patch.object(setup, "run") as run, patch.object(setup, "output", side_effect=[json.dumps(table), json.dumps(inventory)]):
            self.assertEqual(setup.format_filesystem("ntfs", "/dev/nvme0n1", partition=True), node)
        self.assertEqual(run.call_args_list[0].args, ("sfdisk", "--lock", "/dev/nvme0n1"))
        self.assertIn("label: gpt", run.call_args_list[0].kwargs["input_text"])
        self.assertEqual(run.call_args_list[-1].args, ("mkntfs", "--quick", "--label", "wolf-storage", node))

    def test_ntfs_wrong_partition_never_reaches_mkfs(self):
        table = {"partitiontable": {"partitions": [{"node": "/dev/sda1", "type": "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"}]}}
        inventory = {"blockdevices": [disk(children=[disk("/dev/sdb1", type="part")])]}
        with patch.object(setup, "run") as run, patch.object(setup, "output", side_effect=[json.dumps(table), json.dumps(inventory)]):
            with self.assertRaises(setup.SetupError):
                setup.format_filesystem("ntfs", "/dev/sdb", partition=True)
        self.assertFalse(any(call.args[0] == "mkntfs" for call in run.call_args_list))

    def test_filesystem_package_mapping(self):
        for fs_type, package in [("xfs", "xfsprogs"), ("f2fs", "f2fs-tools"), ("ext4", "e2fsprogs")]:
            for manager in ("apt", "dnf"):
                self.assertEqual(setup.filesystem_packages(fs_type, {"manager": manager}), [package])
        self.assertEqual(setup.filesystem_packages("ntfs", {"manager": "apt"}, True), ["ntfs-3g", "fdisk"])
        self.assertEqual(setup.filesystem_packages("ntfs", {"manager": "dnf"}, True), ["ntfs-3g", "ntfsprogs", "util-linux"])

    def test_missing_f2fs_kernel_support_fails_before_destructive_commands(self):
        with patch.object(setup, "install_packages"), patch.object(setup.shutil, "which", return_value="/usr/sbin/mkfs.f2fs"), \
                patch.object(setup.Path, "read_text", return_value="nodev sysfs\next4\nxfs\n"), \
                patch.object(setup, "run", return_value=subprocess.CompletedProcess([], 1, "", "module unavailable")) as run:
            with self.assertRaisesRegex(setup.SetupError, "no drives were erased"):
                setup.prepare_filesystem("f2fs", {"manager": "apt"})
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args, ("modprobe", "f2fs"))

    def test_mount_and_fstab_use_selected_filesystem_and_ntfs_owner(self):
        for fs_type in setup.FILESYSTEMS:
            with self.subTest(fs_type=fs_type), tempfile.TemporaryDirectory() as folder:
                storage = {"mode": "format", "devices": [disk()], "level": None,
                           "mount": folder, "filesystem": fs_type}
                with patch.object(setup, "prepare_filesystem"), patch.object(setup, "install_packages"), \
                        patch.object(setup, "ensure_unused"), patch.object(setup, "ask", return_value="ERASE /dev/sdb"), \
                        patch.object(setup, "format_filesystem", return_value="/dev/sdb1") as format_fs, \
                        patch.object(setup, "output", side_effect=["unique-uuid", "/dev/sdb1"]), \
                        patch.object(setup, "atomic_write") as write, patch.object(setup, "run") as run, \
                        contextlib.redirect_stdout(io.StringIO()):
                    setup.apply_storage(storage, {"manager": "apt"}, 1234, 5678)
                format_fs.assert_called_once_with(fs_type, "/dev/sdb", fs_type == "ntfs")
                mount_type, options, fsck_pass = setup.filesystem_mount(fs_type, 1234, 5678)
                self.assertIn(f"{mount_type} {options} 0 {fsck_pass}", write.call_args.args[1])
                self.assertEqual(run.call_args_list[0].args, ("mount", "-t", mount_type, "-o", options,
                                                            "UUID=unique-uuid", Path(folder)))
                if fs_type == "ntfs":
                    self.assertIn("uid=1234,gid=5678", options)

    def test_ntfs_libraries_use_mount_permissions_instead_of_acls(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(setup, "output", return_value="fuseblk"), \
                patch.object(setup, "run", return_value=subprocess.CompletedProcess([], 0, "", "")) as run, \
                patch.object(setup.os, "chown") as chown, patch.object(setup.os, "chmod") as chmod:
            setup.prepare_directory(Path(folder) / "ROMs", 1234, 5678)
        chown.assert_not_called()
        chmod.assert_not_called()
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0], "setpriv")

    def test_existing_ntfs_and_f2fs_can_be_reused_without_formatting(self):
        items = [disk("/dev/sdb1", type="part", fstype="ntfs", uuid="ntfs-uuid"),
                 disk("/dev/sdc1", type="part", fstype="f2fs", uuid="f2fs-uuid")]
        self.assertEqual(setup.unused_filesystems(items), items)

    def test_ntfs_shared_libraries_keep_private_config_and_homes_on_linux(self):
        cfg = plan()
        cfg["storage"] = {"filesystem": "ntfs", "mount": "/mnt/shared"}
        cfg["libraries"] = {"roms": "/mnt/shared/ROMs"}
        setup.validate_layout(cfg)
        cfg["state"] = "/mnt/shared/apps"
        with self.assertRaisesRegex(setup.SetupError, "Linux filesystem"):
            setup.validate_layout(cfg)


class EntryTests(unittest.TestCase):
    def test_dry_run_cannot_apply(self):
        fake_plan = plan()
        with patch.object(setup, "collect_plan", return_value=(fake_plan, "config", "rules")), \
                patch.object(setup.Path, "exists", return_value=True), \
                patch.object(setup.shutil, "which", return_value="/usr/bin/tool"), \
                patch.object(setup, "apply") as apply, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(setup.main(["--dry-run"]), 0)
            apply.assert_not_called()

    def test_source_checkout_must_match_selected_channel(self):
        with patch.object(setup, "output", return_value="auto-gpu-selection"), self.assertRaises(setup.SetupError):
            setup.load_sources("testing", ROOT)

    def test_downloaded_configs_use_one_resolved_commit(self):
        sha = "a" * 40
        with patch.object(setup, "fetch", side_effect=[json.dumps({"sha": sha}), "config", "rules"]) as fetch:
            result = setup.load_sources("testing")
        self.assertEqual(result, ("config", "rules", sha))
        self.assertTrue(fetch.call_args_list[0].args[0].endswith("/commits/dev-gpu-selection"))
        self.assertIn("/" + sha + "/", fetch.call_args_list[1].args[0])
        self.assertIn("/" + sha + "/", fetch.call_args_list[2].args[0])

    def test_full_preview_with_mixed_gpu_and_shared_libraries(self):
        args = setup.argparse.Namespace(channel="stable", source_dir=str(ROOT), dry_run=True)
        amd = gpu("AMD", "0000:03:00.0", "/dev/dri/renderD128", "amdgpu")
        nv = gpu("NVIDIA", "0000:04:00.0", None, None)
        # Decline NVIDIA; default config; accept each share/default path; default hostname.
        answers = iter(["no", "1", "yes", "", "yes", "", "yes", "", ""])
        with patch.object(setup, "detect_gpus", return_value=[amd, nv]), \
                patch.object(setup, "read_os", return_value={"ID": "ubuntu", "VERSION_ID": "24.04", "VERSION_CODENAME": "noble"}), \
                patch.object(setup.platform, "machine", return_value="x86_64"), \
                patch.object(setup, "output", side_effect=["auto-gpu-selection", "a" * 40]), \
                patch.object(setup, "preflight_existing"), patch("builtins.input", side_effect=lambda _: next(answers)), \
                contextlib.redirect_stdout(io.StringIO()):
            cfg, config_text, rules = setup.collect_plan(args)
        self.assertEqual(cfg["gpu"]["blacklist"], ["0000:04:00.0"])
        self.assertEqual(len(cfg["libraries"]), 3)
        self.assertIn("uinput", rules)
        self.assertEqual(setup.tomllib.loads(config_text)["hostname"], "Wolf")


class DockerInstallTests(unittest.TestCase):
    def test_official_apt_and_rpm_commands(self):
        for distro in [dict(id="ubuntu", manager="apt", codename="noble"),
                       dict(id="debian", manager="apt", codename="trixie"),
                       dict(id="fedora", manager="dnf"), dict(id="rhel", manager="dnf"),
                       dict(id="centos", manager="dnf")]:
            calls = []

            def command(*args, **kwargs):
                calls.append(args)
                return subprocess.CompletedProcess(args, 1 if args[0] in ("dpkg-query", "rpm") else 0, "", "")

            with patch.object(setup, "run", side_effect=command), patch.object(setup, "output", return_value="[]"), \
                    patch.object(setup, "atomic_write") as write, \
                    patch.object(setup.Path, "exists", return_value=False), patch.object(setup.Path, "glob", return_value=[]), \
                    patch.object(setup, "fetch", return_value="BEGIN PGP PUBLIC KEY BLOCK\n[docker-ce-stable]\ngpgcheck=1") as fetch:
                setup.install_docker(distro)
            self.assertIn("https://download.docker.com/linux/" + distro["id"] + "/", fetch.call_args.args[0])
            self.assertTrue(any("docker-compose-plugin" in call and "docker-ce" in call for call in calls))
            self.assertIn(("systemctl", "enable", "--now", "docker"), calls)
            self.assertIn(("docker", "compose", "version"), calls)
            self.assertTrue(write.called)

    def test_package_replacement_requires_consent(self):
        def command(*args, **kwargs):
            return subprocess.CompletedProcess(args, 0, "install ok installed", "")

        with patch.object(setup, "run", side_effect=command) as run, patch.object(setup, "yes", return_value=False), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(setup.SetupError):
            setup.install_docker(dict(id="ubuntu", manager="apt", codename="noble"))
        self.assertFalse(any("remove" in call.args for call in run.call_args_list))


if __name__ == "__main__":
    unittest.main()
