#!/usr/bin/env python3
"""Guided host installer. Importing this module never changes the host.

Requires Python >= 3.9 plus tomli on Python < 3.11. Commands use argument lists,
never shell interpolation. --dry-run performs discovery and prints the complete
plan without executing package, disk, Docker, or system configuration changes.
"""

import argparse
import fcntl
import glob
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import uuid

try:
    import tomllib
except ImportError:
    import tomli as tomllib


CHANNELS = {
    "stable": ("auto-gpu-selection", "latest"),
    "testing": ("dev-gpu-selection", "gpu-selection-dev"),
}
REPOSITORY = "Greaper88/wolf"
DEFAULT_CONFIG = "src/moonlight-server/state/default/config.v7.toml"
DOCKER_PACKAGES = ["docker-ce", "docker-ce-cli", "containerd.io",
                   "docker-buildx-plugin", "docker-compose-plugin"]
GPU_VENDORS = {"0x10de": "NVIDIA", "0x1002": "AMD", "0x8086": "Intel"}
FILESYSTEMS = {
    "xfs": {"label": "Default / hard drive (XFS)", "packages": ["xfsprogs"], "mkfs": ["mkfs.xfs", "-L", "wolf-storage"]},
    "f2fs": {"label": "Flash / SSD friendly (F2FS)", "packages": ["f2fs-tools"], "mkfs": ["mkfs.f2fs", "-l", "wolf-storage"]},
    "ext4": {"label": "Classic Linux (ext4)", "packages": ["e2fsprogs"], "mkfs": ["mkfs.ext4", "-L", "wolf-storage"]},
    "ntfs": {"label": "Windows compatible (not recommended) (NTFS)", "packages": ["ntfs-3g"],
             "mkfs": ["mkntfs", "--quick", "--label", "wolf-storage"]},
}
FS_TYPES = set(FILESYSTEMS) | {"btrfs"}
MARKER = "# Managed by Wolf setup"


class SetupError(RuntimeError):
    pass


def run(*args, capture=False, check=True, input_text=None):
    command = [str(x) for x in args]
    if not capture:
        print("+ " + shlex.join(command), flush=True)
    return subprocess.run(command, check=check, text=True, input=input_text,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


def output(*args):
    return run(*args, capture=True).stdout.strip()


def ask(question, default=None):
    suffix = f" [{default}]" if default is not None else ""
    try:
        answer = input(question + suffix + ": ").strip()
    except EOFError as exc:
        raise SetupError("Interactive input ended; nothing further will be applied.") from exc
    return answer or (str(default) if default is not None else "")


def yes(question, default=False):
    while True:
        answer = ask(question + " (yes/no)", "yes" if default else "no").lower()
        if answer in ("y", "yes", "n", "no"):
            return answer in ("y", "yes")


def choose(question, options, default=0):
    print(question)
    for i, option in enumerate(options, 1):
        print(f"  {i}. {option}")
    while True:
        value = ask("Selection", default + 1)
        if value.isdigit() and 1 <= int(value) <= len(options):
            return int(value) - 1


def read_os(path=Path("/etc/os-release")):
    result = {}
    for line in path.read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            words = shlex.split(value)
            result[key] = words[0] if words else ""
    return result


def distro_info(release, machine):
    # Published Wolf images and the manual NVIDIA build currently target amd64.
    if machine not in ("x86_64", "amd64"):
        raise SetupError("This fork publishes amd64 Wolf images; this installer requires x86_64.")
    distro = release.get("ID", "")
    version = release.get("VERSION_ID", "")
    codename = release.get("VERSION_CODENAME", "")
    derivative = False
    if distro in ("linuxmint", "pop") and release.get("UBUNTU_CODENAME"):
        distro, codename, derivative = "ubuntu", release["UBUNTU_CODENAME"], True
        version = {"jammy": "22.04", "noble": "24.04", "resolute": "26.04"}.get(codename, "")
    if distro in ("rocky", "almalinux"):
        distro, derivative = "rhel", True
    supported = {
        "ubuntu": {"22.04": "jammy", "24.04": "noble", "26.04": "resolute"},
        "debian": {"12": "bookworm", "13": "trixie"},
        "fedora": {"43": "", "44": ""},
        "centos": {"9": "", "10": ""},
        "rhel": {"8": "", "9": "", "10": ""},
    }
    key = version if distro == "ubuntu" else version.split(".")[0]
    if distro not in supported or key not in supported[distro]:
        raise SetupError(f"Unsupported or unverified OS: {release.get('PRETTY_NAME', release)}. "
                         "No nearest-version Docker repository will be guessed.")
    if distro == "centos" and "Stream" not in release.get("PRETTY_NAME", ""):
        raise SetupError("Only maintained CentOS Stream releases are supported.")
    expected = supported[distro][key]
    if expected and codename != expected:
        raise SetupError(f"OS version/codename mismatch: {version}/{codename}; expected {expected}.")
    return {"id": distro, "version": key, "codename": codename,
            "manager": "apt" if distro in ("ubuntu", "debian") else "dnf",
            "derivative": derivative, "name": release.get("PRETTY_NAME", distro)}


def detect_gpus(sysroot=Path("/sys")):
    gpus = []
    for device in sorted((sysroot / "bus/pci/devices").glob("*")):
        try:
            if not (device / "class").read_text().strip().startswith("0x03"):
                continue
            vendor = GPU_VENDORS.get((device / "vendor").read_text().strip(), "Other")
            nodes = sorted((device / "drm").glob("renderD*"))
            gpus.append({"pci": device.name, "vendor": vendor,
                         "node": "/dev/dri/" + nodes[0].name if nodes else None,
                         "stable_node": f"/dev/dri/by-path/pci-{device.name}-render"
                         if Path(f"/dev/dri/by-path/pci-{device.name}-render").exists() else None,
                         "driver": (device / "driver").resolve().name if (device / "driver").exists() else None})
        except FileNotFoundError:
            continue
    return gpus


def gpu_plan(gpus, enable_nvidia=False, nvidia_index=0):
    nvidia = [g for g in gpus if g["vendor"] == "NVIDIA"]
    mesa = [g for g in gpus if g["vendor"] in ("AMD", "Intel") and g["node"]]
    if enable_nvidia:
        usable = [g for g in nvidia if g["node"] and g["driver"] == "nvidia"]
        if not usable or nvidia_index >= len(usable):
            raise SetupError("NVIDIA needs a loaded proprietary host driver and a DRM render node. "
                             "Install the distro's NVIDIA driver, enable nvidia-drm.modeset=1, reboot, and rerun.")
        return {"mode": "nvidia", "node": usable[nvidia_index].get("stable_node") or usable[nvidia_index]["node"],
                "pci": usable[nvidia_index]["pci"], "blacklist": []}
    if not mesa:
        raise SetupError("No AMD/Intel DRM render node is available. Install the host GPU driver first.")
    return {"mode": "mesa", "node": None, "blacklist": [g["pci"] for g in nvidia]}


def select_gpu(gpus):
    print("\nDetected display controllers:")
    for gpu in gpus:
        print(f"  {gpu['pci']}  {gpu['vendor']}  {gpu['node'] or 'no render node'}  "
              f"driver={gpu['driver'] or 'not loaded'}")
    nvidia = [g for g in gpus if g["vendor"] == "NVIDIA"]
    mesa = [g for g in gpus if g["vendor"] in ("AMD", "Intel") and g["node"]]
    enable = bool(nvidia) and not mesa
    if nvidia:
        print("NVIDIA uses the legacy manual path: one selected render/encode GPU, "
              "without automatic multi-GPU balancing. The driver container does not install host drivers.")
        if mesa:
            enable = yes("Enable NVIDIA and use this manual path instead of AMD/Intel automatic selection?")
    index = 0
    if enable:
        usable = [g for g in nvidia if g["node"] and g["driver"] == "nvidia"]
        if len(usable) > 1:
            index = choose("Select the NVIDIA GPU for rendering and encoding",
                           [f"{g['pci']} ({g['node']})" for g in usable])
    return gpu_plan(gpus, enable, index)


def block_inventory():
    return json.loads(output("lsblk", "--json", "--bytes", "--paths", "--output",
                             "NAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS,UUID,RO,RM,MODEL,SERIAL,WWN,MAJ:MIN"))["blockdevices"]


def flatten(devices):
    for device in devices:
        yield device
        yield from flatten(device.get("children", []))


def blank_disks(devices):
    return [d for d in devices if d["type"] == "disk" and not d.get("children")
            and not d.get("fstype") and not any(d.get("mountpoints") or [])
            and not d.get("ro") and not d.get("rm") and int(d["size"]) > 0]


def inactive_disks(devices):
    """Whole disks with no active descendants. Signatures/data are allowed here."""
    return [d for d in devices if d["type"] == "disk" and int(d["size"]) > 0
            and all(not item.get("ro") and not any(item.get("mountpoints") or [])
                    and item["type"] in ("disk", "part") for item in flatten([d]))]


def unused_filesystems(devices):
    return [d for d in flatten(devices) if d.get("fstype") in FS_TYPES and d.get("uuid")
            and not any(d.get("mountpoints") or []) and not d.get("children") and not d.get("ro")]


def show_storage(devices):
    print("\nDrives and partitions:")
    for d in flatten(devices):
        print(f"  {d['name']:<22} {int(d['size']) / 2**30:9.1f} GiB  {d.get('fstype') or 'unformatted':<14} "
              f"{','.join(x for x in d.get('mountpoints', []) if x) or 'not mounted'}  "
              f"{d.get('model') or ''} {d.get('serial') or ''}")
    print("\nMounted filesystem space (size, used, available):")
    run("df", "-hT", "--exclude-type=tmpfs", "--exclude-type=devtmpfs")


def valid_path(value):
    # Wolf's current startup.sh expands its state path without shell quoting.
    # Also exclude Compose, fstab, systemd and Docker bind separators.
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", value):
        raise SetupError("Use an absolute path containing only letters, digits, /, -, _, and dots.")
    path = Path(value).resolve()
    if path in map(Path, ("/", "/etc", "/usr", "/var", "/home", "/opt", "/srv", "/mnt", "/media")) or any(path == p or p in path.parents for p in
                                map(Path, ("/dev", "/proc", "/sys", "/boot", "/run"))):
        raise SetupError(f"Not a suitable data path: {path}")
    if path.exists() and not path.is_dir():
        raise SetupError(f"Not a directory: {path}")
    return str(path)


def path_question(question, default):
    while True:
        try:
            return valid_path(ask(question, default))
        except SetupError as exc:
            print(exc)


def select_storage(devices):
    blank, unused, inactive = blank_disks(devices), unused_filesystems(devices), inactive_disks(devices)
    if not blank and not unused and not inactive:
        return None
    print("\nUnused storage was found. Array members may be blank or contain data. "
          "Selecting an existing drive for an array ERASES its partitions and filesystems. "
          "Mounted drives, swap and active device mappings cannot be selected.")
    mode = choose("Optional storage setup", ["Keep current storage", "Mount an existing filesystem (preserve data)",
                                            "Format one blank disk (choose filesystem)", "Create an mdadm array from selected drives (ERASE)"])
    if mode == 0:
        return None
    pool = unused if mode == 1 else inactive if mode == 3 else blank
    if not pool:
        raise SetupError("There are no eligible drives for that option; rerun and choose existing storage.")
    for i, d in enumerate(pool, 1):
        print(f"  {i}. {d['name']}  {int(d['size']) / 2**30:.1f} GiB  "
              f"{d.get('model') or ''} {d.get('serial') or ''}")
        for item in flatten([d]):
            if item.get("fstype") or item is not d:
                print(f"       {item['name']}  {item.get('fstype') or 'partition'}  UUID={item.get('uuid') or '-'}")
    while True:
        value = ask("Drive number(s), separated by spaces" if mode == 3 else "Drive number")
        try:
            indices = [int(v) - 1 for v in value.split()]
            if not indices or len(set(indices)) != len(indices) or any(i < 0 or i >= len(pool) for i in indices):
                raise ValueError
            if mode != 3 and len(indices) != 1:
                raise ValueError
            selected = [pool[i] for i in indices]
            break
        except ValueError:
            print("Select valid, unique drive numbers.")
    level = None
    if mode == 3:
        level = ["1", "0", "5", "6", "10"][choose("Array layout", [
            "RAID1 mirror (2+ disks)", "RAID0 stripe (2+ disks; no redundancy)",
            "RAID5 (3+ disks; one-disk redundancy)", "RAID6 (4+ disks; two-disk redundancy)",
            "RAID10 (4+ disks, even count)"])]
        validate_raid(level, len(selected))
        print("Array capacity is limited by its smallest member. RAID is not a backup.")
    fs_type = selected[0]["fstype"] if mode == 1 else list(FILESYSTEMS)[choose(
        "Filesystem", [settings["label"] for settings in FILESYSTEMS.values()])]
    if fs_type == "ntfs":
        print("NTFS is not recommended for Linux app homes or Proton prefixes. "
              "Use it for shared files; Linux ownership/ACLs and some filenames behave differently.")
        if mode == 3:
            print("NTFS is Windows-compatible, but Windows cannot directly assemble an mdadm array.")
    mount = path_question("Mount this storage at", "/mnt/wolf-storage")
    if Path(mount).exists() and (os.path.ismount(mount) or any(Path(mount).iterdir())):
        raise SetupError("The mount destination must be empty and not already mounted.")
    return {"mode": "existing" if mode == 1 else "raid" if mode == 3 else "format",
            "devices": selected, "level": level, "mount": mount, "filesystem": fs_type}


def validate_raid(level, count):
    minimum = {"0": 2, "1": 2, "5": 3, "6": 4, "10": 4}
    if level not in minimum or count < minimum[level] or (level == "10" and count % 2):
        raise SetupError(f"Invalid member count {count} for RAID{level}.")


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Wolf-setup", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read().decode("utf-8")


def load_sources(channel, source_dir=None):
    branch, _ = CHANNELS[channel]
    if source_dir:
        root = Path(source_dir)
        actual = output("git", "-C", root, "branch", "--show-current")
        if actual != branch:
            raise SetupError(f"--source-dir is on {actual}, but {channel} requires {branch}.")
        revision = output("git", "-C", root, "rev-parse", "HEAD") + " (local checkout)"
        return (root / DEFAULT_CONFIG).read_text(), (root / "85-wolf.rules").read_text(), revision
    ref = urllib.parse.quote(branch, safe="")
    revision = json.loads(fetch(f"https://api.github.com/repos/{REPOSITORY}/commits/{ref}"))["sha"]
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise SetupError("GitHub did not return a valid source commit.")
    base = f"https://raw.githubusercontent.com/{REPOSITORY}/{revision}/"
    return fetch(base + DEFAULT_CONFIG), fetch(base + "85-wolf.rules"), revision


def toml_value(value):
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, bool):
        return str(value).lower()
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{" + ", ".join(f"{json.dumps(k)} = {toml_value(v)}" for k, v in value.items()) + "}"
    raise SetupError(f"Unsupported config value: {type(value)}")


def toml_dumps(data):
    lines = []

    def table(obj, path=(), header=None):
        if header:
            lines.extend(("", header))
        for key, value in obj.items():
            if not isinstance(value, dict) and not (isinstance(value, list) and value and isinstance(value[0], dict)):
                lines.append(f"{json.dumps(key)} = {toml_value(value)}")
        for key, value in obj.items():
            names = path + (key,)
            name = ".".join(json.dumps(x) for x in names)
            if isinstance(value, dict):
                table(value, names, f"[{name}]")
            elif isinstance(value, list) and value and isinstance(value[0], dict):
                for item in value:
                    table(item, names, f"[[{name}]]")

    table(data)
    text = "\n".join(lines) + "\n"
    if tomllib.loads(text) != data:
        raise SetupError("Configuration failed its TOML round-trip validation.")
    return text


def configure_apps(template, channel, libraries, hostname, selinux=False):
    cfg = tomllib.loads(template)
    if cfg.get("config_version") != 7 or not cfg.get("profiles"):
        raise SetupError("Unrecognized branch configuration format.")
    cfg["uuid"], cfg["hostname"] = str(uuid.uuid4()), hostname
    images, ui_found = set(), False
    for profile in cfg["profiles"]:
        for app in profile.get("apps", []):
            runner = app.get("runner", {})
            if runner.get("type", "").lower() != "docker":
                continue
            if app["title"] == "Wolf UI":
                runner["image"] = f"ghcr.io/greaper88/wolf-ui:{CHANNELS[channel][1]}"
                ui_found = True
            if selinux:
                create = json.loads(runner.get("base_create_json") or "{}")
                security = create.setdefault("HostConfig", {}).setdefault("SecurityOpt", [])
                if "label=disable" not in security:
                    security.append("label=disable")
                runner["base_create_json"] = json.dumps(create)
            mounts = runner.setdefault("mounts", [])
            # Every configured Docker app can browse these libraries; mounts on
            # the parent Wolf container alone do not propagate to child apps.
            for name, host in libraries.items():
                target = {"steam": "/SteamLibrary", "roms": "/ROMs", "installers": "/Installers"}[name]
                mounts.append(f"{host}:{target}:rw")
                if name == "steam" and "steam" in runner["image"].split("/")[-1].split(":")[0].lower():
                    mounts.append(f"{host}/steamapps:/home/retro/.steam/debian-installation/steamapps:rw")
            images.add(runner["image"])
    if not ui_found:
        raise SetupError("Branch config has no Wolf UI runner; refusing mismatched channels.")
    return toml_dumps(cfg), sorted(images)


def compose_document(plan, nvidia_devices=()):
    env = {"HOST_APPS_STATE_FOLDER": plan["state"], "WOLF_SOCKET_PATH": "/var/run/wolf/wolf.sock",
           "WOLF_DEFAULT_RUN_UID": str(plan["uid"]), "WOLF_DEFAULT_RUN_GID": str(plan["gid"]),
           "WOLF_GPU_AUTO_SELECT": "true", "WOLF_GPU_AUTO_BLACKLIST": "true"}
    volumes = [f"{plan['state']}:{plan['state']}:rw", "/var/run/docker.sock:/var/run/docker.sock:rw",
               "/dev:/dev:rw", "/run/udev:/run/udev:rw", "/var/run/wolf:/var/run/wolf:rw"]
    # A separately chosen config directory is overlaid at the path startup.sh uses.
    volumes.append(f"{plan['config']}:{plan['state']}/cfg:rw")
    gpu = plan["gpu"]
    if gpu["blacklist"]:
        env["WOLF_GPU_BLACKLIST"] = ",".join(gpu["blacklist"])
    service = {"image": f"ghcr.io/greaper88/wolf:{CHANNELS[plan['channel']][1]}",
               "environment": env, "volumes": volumes, "devices": ["/dev/dri:/dev/dri", "/dev/uinput:/dev/uinput", "/dev/uhid:/dev/uhid"],
               "device_cgroup_rules": ["c 13:* rmw"], "network_mode": "host", "restart": "no"}
    document = {"name": "wolf-setup", "services": {"wolf": service}}
    if gpu["mode"] == "nvidia":
        env.update({"WOLF_GPU_AUTO_SELECT": "false", "WOLF_RENDER_NODE": gpu["node"],
                    "WOLF_ENCODER_NODE": gpu["node"], "NVIDIA_DRIVER_VOLUME_NAME": plan["driver_volume"]})
        if gpu.get("cuda_uuid"):
            # NVENC otherwise defaults to CUDA device zero, which can be a
            # different GPU from the chosen DRM node on a multi-NVIDIA host.
            env["CUDA_VISIBLE_DEVICES"] = gpu["cuda_uuid"]
        volumes.append("nvidia-driver:/usr/nvidia:ro")
        service["devices"].extend(f"{d}:{d}" for d in nvidia_devices)
        document["volumes"] = {"nvidia-driver": {"external": True, "name": plan["driver_volume"]}}
    if plan.get("selinux"):
        service["security_opt"] = ["label=disable"]
    return document


def atomic_write(path, text, mode=0o644, backup=False):
    path = Path(path)
    if path.is_symlink():
        raise SetupError(f"Refusing to replace symlink: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return
    if backup and path.exists():
        shutil.copy2(path, str(path) + f".wolf-backup-{time.time_ns()}")
    fd, temporary = tempfile.mkstemp(prefix=".wolf-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def install_packages(distro, packages):
    if distro["manager"] == "apt":
        run("apt-get", "install", "-y", "--no-install-recommends", *packages)
    else:
        run("dnf", "install", "-y", *packages)


def install_docker(distro):
    apt = distro["manager"] == "apt"
    def installed(package):
        result = run(*(["dpkg-query", "-W", "-f=${Status}", package] if apt else ["rpm", "-q", package]),
                     capture=True, check=False)
        return result.returncode == 0 and (not apt or result.stdout == "install ok installed")

    missing = [package for package in DOCKER_PACKAGES if not installed(package)]
    conflicts = (["docker.io", "docker-compose", "docker-compose-v2", "docker-doc", "docker-buildx", "podman-docker", "containerd", "runc"]
                 if apt else ["docker", "docker-client", "docker-client-latest", "docker-common", "docker-latest",
                              "docker-latest-logrotate", "docker-logrotate", "docker-selinux", "docker-engine-selinux",
                              "docker-engine", "podman-docker", "podman", "runc"])
    present = []
    for package in conflicts:
        if installed(package):
            present.append(package)
    if present:
        print("Conflicting packages: " + ", ".join(present))
        if not yes("Remove these packages to install official Docker CE? Existing workloads may be interrupted"):
            raise SetupError("Resolve the package conflicts before rerunning.")
        run("apt-get" if apt else "dnf", "remove", "-y", *present)
    if apt:
        run("apt-get", "update")
        install_packages(distro, ["ca-certificates", "curl", "acl", "kmod", "udev"])
        # Detect alternate Docker source definitions instead of introducing Signed-By conflicts.
        owned = Path("/etc/apt/sources.list.d/wolf-docker.sources")
        existing_repository = False
        for path in [Path("/etc/apt/sources.list"), *Path("/etc/apt/sources.list.d").glob("*.list"),
                     *Path("/etc/apt/sources.list.d").glob("*.sources")]:
            if path != owned and path.exists() and "download.docker.com" in path.read_text():
                content = path.read_text()
                if (f"https://download.docker.com/linux/{distro['id']}" not in content
                        or distro["codename"] not in content or owned.exists()):
                    raise SetupError(f"Incompatible or duplicate Docker repository in {path}; "
                                     "correct it before rerunning setup.")
                existing_repository = True
                print(f"Reusing Docker repository: {path}")
        if not existing_repository:
            key = fetch(f"https://download.docker.com/linux/{distro['id']}/gpg")
            if "BEGIN PGP PUBLIC KEY BLOCK" not in key:
                raise SetupError("Invalid Docker signing key response.")
            atomic_write("/etc/apt/keyrings/wolf-docker.asc", key)
            repo = ("Types: deb\n" + f"URIs: https://download.docker.com/linux/{distro['id']}\n"
                    + f"Suites: {distro['codename']}\nComponents: stable\nArchitectures: amd64\n"
                    + "Signed-By: /etc/apt/keyrings/wolf-docker.asc\n")
            atomic_write(owned, repo, backup=True)
            run("apt-get", "update")
    else:
        install_packages(distro, ["ca-certificates", "curl", "acl", "kmod", "systemd-udev"])
        repo = fetch(f"https://download.docker.com/linux/{distro['id']}/docker-ce.repo")
        if "[docker-ce-stable]" not in repo or "gpgcheck=1" not in repo:
            raise SetupError("Invalid Docker RPM repository response.")
        atomic_write("/etc/yum.repos.d/docker-ce.repo", repo, backup=True)
    # Avoid upgrading a functioning CE daemon just because setup was rerun.
    if missing:
        install_packages(distro, missing)
    run("systemctl", "enable", "--now", "docker")
    run("docker", "compose", "version")
    security = json.loads(output("docker", "info", "--format", "{{json .SecurityOptions}}"))
    if any("rootless" in s or "userns" in s for s in security):
        raise SetupError("Wolf setup requires a local rootful Docker daemon without userns-remap.")


def device_identity(device):
    return [(d["name"], d.get("size"), d.get("serial"), d.get("model"), d.get("wwn"),
             d.get("maj:min"), d.get("uuid"), d.get("fstype")) for d in flatten([device])]


def ensure_unused(expected, allow_data=False):
    candidates = inactive_disks(block_inventory()) if allow_data else blank_disks(block_inventory())
    actual = {d["name"]: d for d in candidates}.get(expected["name"])
    if actual is None or device_identity(actual) != device_identity(expected):
        raise SetupError(f"Drive identity or usage changed: {expected['name']}")
    for item in flatten([actual]):
        device = Path(item["name"])
        if not stat.S_ISBLK(device.stat().st_mode):
            raise SetupError(f"Not a block device: {device}")
        holders = Path("/sys/class/block") / device.name / "holders"
        if not holders.exists() or any(holders.iterdir()):
            raise SetupError(f"Device has holders or cannot be verified: {device}")
        signatures = json.loads(output("wipefs", "--json", "--no-act", str(device))).get("signatures", [])
        if signatures and not allow_data:
            raise SetupError(f"Existing signatures found on {device}; use the explicit array erase option.")
        # A process can use a raw device without mounting it.
        users = run("fuser", str(device), capture=True, check=False)
        if users.returncode != 1:
            raise SetupError(f"Device is open by a process or its use cannot be checked: {device}")
        # Do not leave boot-time filesystem or encrypted-device entries pointing at erased UUIDs.
        for config, source_column in ((Path("/etc/fstab"), 0), (Path("/etc/crypttab"), 1)):
            if not config.exists():
                continue
            for line in config.read_text().splitlines():
                fields = line.split()
                if not fields or line.lstrip().startswith("#") or len(fields) <= source_column:
                    continue
                source = fields[source_column]
                if source.startswith(("UUID=", "LABEL=", "PARTUUID=", "PARTLABEL=")):
                    resolved = run("findfs", source, capture=True, check=False)
                    source = resolved.stdout.strip() if resolved.returncode == 0 else source
                if source.startswith("/dev/") and Path(source).resolve() == device.resolve():
                    raise SetupError(f"{device} is configured in {config}. Remove that old entry explicitly "
                                     "before selecting this drive for erasure.")


def filesystem_packages(fs_type, distro, partition=False):
    packages = list(FILESYSTEMS[fs_type]["packages"]) if fs_type in FILESYSTEMS else ["btrfs-progs"]
    if fs_type == "ntfs" and distro["manager"] == "dnf":
        packages.append("ntfsprogs")  # Fedora/EL split mkntfs out of ntfs-3g.
    if partition:
        packages.append("fdisk" if distro["manager"] == "apt" else "util-linux")
    return packages


def prepare_filesystem(fs_type, distro, partition=False, formatting=True):
    """Verify tools and mount support before erasing any selected device."""
    if fs_type not in FS_TYPES or (formatting and fs_type not in FILESYSTEMS):
        raise SetupError(f"Unsupported filesystem choice: {fs_type}")
    try:
        install_packages(distro, filesystem_packages(fs_type, distro, partition))
    except subprocess.CalledProcessError as exc:
        raise SetupError(f"{fs_type} tools are unavailable in the enabled distro repositories. "
                         "Enable an appropriate distro repository or choose another filesystem; no drives were erased.") from exc
    commands = ([FILESYSTEMS[fs_type]["mkfs"][0]] if formatting else [])
    if fs_type == "ntfs":
        commands.append("ntfs-3g")
    if partition:
        commands.append("sfdisk")
    for command in commands:
        if not shutil.which(command):
            raise SetupError(f"Missing {command}; no drives were erased.")
    module = "fuse" if fs_type == "ntfs" else fs_type
    result = run("modprobe", module, capture=True, check=False)
    available = {line.split()[-1] for line in Path("/proc/filesystems").read_text().splitlines() if line.split()}
    if module not in available:
        raise SetupError(f"The running kernel cannot mount {fs_type}. "
                         f"Install its kernel support or choose another filesystem; no drives were erased. {result.stderr or ''}")


def format_filesystem(fs_type, target, partition=False):
    if fs_type not in FILESYSTEMS:
        raise SetupError(f"Unsupported format: {fs_type}")
    if partition:
        # A GPT/basic-data partition lets Windows discover standalone NTFS disks.
        # This is reached only after the exact-drive erase confirmation/recheck.
        run("sfdisk", "--lock", target, input_text=(
            "label: gpt\n\ntype=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n"))
        run("udevadm", "settle")
        table = json.loads(output("sfdisk", "--json", target))["partitiontable"]
        entries = table.get("partitions", [])
        if len(entries) != 1 or entries[0].get("type", "").upper() != "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7":
            raise SetupError("The new NTFS partition could not be verified; formatting stopped.")
        partition_node = entries[0]["node"]
        # Confirm the reported partition belongs to the selected physical disk.
        children = json.loads(output("lsblk", "--json", "--paths", "--output", "NAME,TYPE", target))["blockdevices"]
        if not any(d["name"] == partition_node and d["type"] == "part" for d in flatten(children)):
            raise SetupError("NTFS partition is not on the selected disk; formatting stopped.")
        target = partition_node
    run(*FILESYSTEMS[fs_type]["mkfs"], target)
    return target


def filesystem_mount(fs_type, uid, gid):
    if fs_type == "ntfs":
        return "ntfs-3g", f"defaults,nofail,uid={uid},gid={gid},umask=0002,windows_names", 0
    return fs_type, "defaults,nofail", 2 if fs_type == "ext4" else 0


def apply_storage(storage, distro, uid=1000, gid=1000):
    if storage is None:
        return
    mount = Path(storage["mount"])
    if mount.exists() and (os.path.ismount(mount) or any(mount.iterdir())):
        raise SetupError("Storage mount destination became occupied.")
    fstab = Path("/etc/fstab")
    existing = fstab.read_text()
    if any(len(fields := line.split()) >= 2 and not line.lstrip().startswith("#") and fields[1] == str(mount)
           for line in existing.splitlines()):
        raise SetupError(f"{mount} already has an fstab entry.")
    devices = storage["devices"]
    fs_type = devices[0]["fstype"] if storage["mode"] == "existing" else storage["filesystem"]
    partition = storage["mode"] == "format" and fs_type == "ntfs"
    prepare_filesystem(fs_type, distro, partition, formatting=storage["mode"] != "existing")
    if storage["mode"] != "existing":
        install_packages(distro, ["psmisc"] + (["mdadm"] if storage["mode"] == "raid" else []))
        allow_data = storage["mode"] == "raid"
        if allow_data and Path("/dev/md/wolf").exists():
            raise SetupError("An mdadm device named wolf already exists.")
        for device in devices:
            ensure_unused(device, allow_data)
        phrase = "ERASE " + " ".join(d["name"] for d in devices)
        print("\nThese drives will be formatted; all their data will be destroyed:")
        print("Selected filesystem: " + fs_type)
        for d in devices:
            print(f"  {d['name']}  {d.get('model')}  serial={d.get('serial')}  bytes={d['size']}")
            for item in flatten([d]):
                print(f"    {item['name']}: {item.get('fstype') or 'no filesystem'} UUID={item.get('uuid') or '-'}")
        if ask(f"Type exactly: {phrase}") != phrase:
            raise SetupError("Disk operation cancelled.")
        for device in devices:
            ensure_unused(device, allow_data)
        target = devices[0]["name"]
        if storage["mode"] == "raid":
            target = "/dev/md/wolf"
            validate_raid(storage["level"], len(devices))
            for device in devices:
                # Recheck each drive before removing its children's signatures, then
                # its partition table. Never use --force against a busy device.
                ensure_unused(device, True)
                for item in reversed(list(flatten([device]))):
                    run("wipefs", "--all", item["name"])
                run("blockdev", "--rereadpt", device["name"])
            run("udevadm", "settle")
            run("mdadm", "--create", target, "--metadata=1.2", "--name=wolf",
                "--level=" + storage["level"], "--raid-devices=" + str(len(devices)),
                "--run", *(d["name"] for d in devices))
            # Save assembly metadata BEFORE formatting so an interrupted run is recoverable.
            line = output("mdadm", "--detail", "--brief", target)
            if not line.startswith("ARRAY ") or "UUID=" not in line:
                raise SetupError("Array was created, but its assembly record could not be verified.")
            mdconf = Path("/etc/mdadm/mdadm.conf" if distro["manager"] == "apt" else "/etc/mdadm.conf")
            old = mdconf.read_text() if mdconf.exists() else ""
            atomic_write(mdconf, old.rstrip() + "\n" + line + "\n", backup=True)
        target = format_filesystem(fs_type, target, partition)
    else:
        d = devices[0]
        fresh = {x["name"]: x for x in unused_filesystems(block_inventory())}.get(d["name"])
        if fresh is None or any(fresh.get(k) != d.get(k) for k in ("uuid", "size", "fstype")):
            raise SetupError("Existing filesystem identity or usage changed.")
        target, fs_type = d["name"], d["fstype"]
    fs_uuid = output("blkid", "-s", "UUID", "-o", "value", target)
    if not re.fullmatch(r"[A-Za-z0-9-]+", fs_uuid):
        raise SetupError("Unable to verify filesystem UUID.")
    if any(line.split() and line.split()[0] == "UUID=" + fs_uuid for line in existing.splitlines()):
        raise SetupError("Selected filesystem already has an fstab entry; mount it there instead.")
    matches = output("blkid", "-t", "UUID=" + fs_uuid, "-o", "device").splitlines()
    if len(matches) != 1 or Path(matches[0]).resolve() != Path(target).resolve():
        raise SetupError("Filesystem UUID is ambiguous; no persistent mount was configured.")
    mount.mkdir(parents=True, exist_ok=True)
    mount_type, mount_options, fsck_pass = filesystem_mount(fs_type, uid, gid)
    run("mount", "-t", mount_type, "-o", mount_options, "UUID=" + fs_uuid, mount)
    # nofail permits host boot if absent; Wolf's RequiresMountsFor still blocks its launch.
    entry = f"UUID={fs_uuid} {mount} {mount_type} {mount_options} 0 {fsck_pass}\n"
    atomic_write(fstab, existing.rstrip() + "\n" + MARKER + "\n" + entry, backup=True)
    run("systemctl", "daemon-reload")
    if storage["mode"] == "raid":
        if shutil.which("update-initramfs"):
            run("update-initramfs", "-u")
        elif shutil.which("dracut"):
            run("dracut", "--force")


# Adapted from games-on-whales/gow images/nvidia-driver/Dockerfile at
# a65d67acf287c6b1dce83aa322d9b01958abdf0a (MIT). Builds user-space libraries only.
# Copyright (c) 2021 ABeltramo
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
NVIDIA_DOCKERFILE = """FROM ubuntu:22.04 AS installer
ARG NV_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates kmod pkg-config libglvnd-dev vulkan-tools && \\
    curl -fSL --retry 3 https://download.nvidia.com/XFree86/Linux-x86_64/${NV_VERSION}/NVIDIA-Linux-x86_64-${NV_VERSION}.run -o /tmp/nvidia.run && \\
    chmod +x /tmp/nvidia.run && mkdir -p /usr/nvidia && \\
    /tmp/nvidia.run --silent -z --skip-depmod --skip-module-unload --no-nvidia-modprobe --no-kernel-modules --no-kernel-module-source --opengl-prefix=/usr/nvidia --wine-prefix=/usr/nvidia --utility-prefix=/usr/nvidia --utility-libdir=lib --compat32-prefix=/usr/nvidia --compat32-libdir=lib32 --egl-external-platform-config-path=/usr/nvidia/share/egl/egl_external_platform.d --glvnd-egl-config-path=/usr/nvidia/share/glvnd/egl_vendor.d --no-distro-scripts && \\
    rm /tmp/nvidia.run
FROM scratch
COPY --from=installer /usr/nvidia/ /usr/nvidia/
COPY --from=installer /etc/vulkan/icd.d/nvidia_icd.json /usr/nvidia/share/vulkan/icd.d/
CMD ["unused-volume-seed"]
"""


def nvidia_version():
    version_file = Path("/sys/module/nvidia/version")
    version = version_file.read_text().strip() if version_file.exists() else ""
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", version):
        raise SetupError("The NVIDIA kernel driver is not loaded.")
    if tuple(map(int, version.split("."))) < (530, 30, 2):
        raise SetupError("Wolf needs NVIDIA host drivers >= 530.30.02.")
    mode = Path("/sys/module/nvidia_drm/parameters/modeset")
    if not mode.exists() or mode.read_text().strip() not in ("Y", "1"):
        raise SetupError("Enable nvidia-drm.modeset=1 on the host and reboot before rerunning setup.")
    return version


def nvidia_uuid(pci):
    rows = output("nvidia-smi", "--query-gpu=pci.bus_id,uuid", "--format=csv,noheader")
    for row in rows.splitlines():
        bus, identifier = (part.strip() for part in row.split(",", 1))
        domain, bus_number, function = bus.lower().split(":")
        normalized = f"{int(domain, 16):04x}:{bus_number}:{function}"
        if normalized == pci and re.fullmatch(r"GPU-[0-9a-fA-F-]+", identifier):
            return identifier
    raise SetupError(f"Cannot map NVIDIA PCI device {pci} to a CUDA UUID.")


def build_nvidia(plan):
    version = nvidia_version()
    if version != plan["driver_version"]:
        raise SetupError("NVIDIA driver version changed during setup; rerun discovery.")
    run("modprobe", "nvidia_uvm")
    if shutil.which("nvidia-modprobe"):
        run("nvidia-modprobe", "-u", "-c", "0")
    run("nvidia-smi", "-L")
    devices = sorted(p for p in glob.glob("/dev/nvidia*") + glob.glob("/dev/nvidia-caps/*")
                     if stat.S_ISCHR(os.stat(p).st_mode))
    for needed in ("/dev/nvidiactl", "/dev/nvidia-uvm", "/dev/nvidia-modeset"):
        if needed not in devices:
            raise SetupError(f"Missing {needed}; initialize NVIDIA host device nodes and rerun.")
    volume, image = plan["driver_volume"], "wolf-setup/nvidia-driver:" + version
    with tempfile.TemporaryDirectory(prefix="wolf-nvidia-") as folder:
        dockerfile = Path(folder) / "Dockerfile"
        dockerfile.write_text(NVIDIA_DOCKERFILE)
        run("docker", "build", "--pull", "--tag", image, "--build-arg", "NV_VERSION=" + version, folder)
    run("docker", "volume", "create", "--label", "wolf.setup.nvidia-version=" + version, volume)
    actual = output("docker", "volume", "inspect", "--format", '{{index .Labels "wolf.setup.nvidia-version"}}', volume)
    if actual != version:
        raise SetupError("Existing driver volume has an unexpected version label.")
    container = output("docker", "create", "--mount", f"type=volume,source={volume},target=/usr/nvidia", image)
    try:
        # docker create populates an empty volume; the scratch container is never executed.
        mount = output("docker", "volume", "inspect", "--format", "{{.Mountpoint}}", volume)
        if not list(Path(mount).glob("lib/libnvidia-encode.so*")):
            raise SetupError("NVIDIA driver volume did not populate correctly.")
    finally:
        run("docker", "rm", container)
    return devices


def prepare_directory(path, uid, gid):
    path = Path(path)
    created = not path.exists()
    path.mkdir(parents=True, exist_ok=True)
    fs_type = output("findmnt", "--noheadings", "--output", "FSTYPE", "--target", path)
    fixed_permissions = fs_type in ("ntfs", "ntfs3", "fuseblk")
    if created and not fixed_permissions:
        os.chown(path, uid, gid)
        os.chmod(path, 0o2775)
    # Do not chown a user's existing library recursively. Add scoped ACLs only after consent.
    code = "import os,sys; sys.exit(0 if os.access(sys.argv[1], os.R_OK|os.W_OK|os.X_OK) else 1)"
    result = run("setpriv", "--reuid", str(uid), "--regid", str(gid), "--clear-groups",
                 sys.executable, "-c", code, str(path), capture=True, check=False)
    if result.returncode:
        if fixed_permissions:
            raise SetupError(f"UID {uid} cannot use {path}; check the NTFS mount's uid={uid},gid={gid} "
                             "and directory traversal permissions. Linux ACL grants are not used on this mount.")
        if not yes(f"UID {uid} cannot use {path}. Grant it read/write directory ACLs recursively?"):
            raise SetupError(f"Adjust permissions on {path}, including parent directory traversal, and rerun.")
        run("setfacl", "-R", "-m", f"u:{uid}:rwX", path)
    if not fixed_permissions:
        run("setfacl", "-m", f"d:u:{uid}:rwx", path)


def service_text(plan, docker):
    paths = sorted({plan["install"], plan["state"], plan["config"], *plan["libraries"].values()})
    compose = f"{docker} --host unix:///var/run/docker.sock compose --project-name wolf-setup --file {plan['install']}/docker-compose.yml"
    return f"""{MARKER}
[Unit]
Description=Wolf game streaming server
Requires=docker.service
PartOf=docker.service
After=docker.service network-online.target
Wants=network-online.target
RequiresMountsFor={' '.join(paths)}

[Service]
Type=simple
WorkingDirectory={plan['install']}
ExecStartPre=/usr/bin/test -f {plan['config']}/config.toml
ExecStart={compose} up --no-color --no-build --pull never --abort-on-container-exit --exit-code-from wolf
ExecStop={compose} stop --timeout 30
Restart=always
RestartSec=10
TimeoutStopSec=60

[Install]
WantedBy=multi-user.target
"""


def preflight_existing(plan):
    for path in (Path(plan["config"]) / "config.toml", Path(plan["install"]) / "docker-compose.yml"):
        if path.exists():
            raise SetupError(f"Existing deployment file: {path}. Setup preserves paired clients and custom apps. "
                             "Use its existing Compose deployment; choose empty paths for a new install.")
    service = Path("/etc/systemd/system/wolf-setup.service")
    if service.exists():
        raise SetupError(f"{service} already exists; manage the existing installation with systemctl.")


def validate_layout(plan):
    state, config, install = (Path(plan[key]) for key in ("state", "config", "install"))
    if state == config or config in state.parents:
        raise SetupError("Config must be separate from app data, or in its cfg subdirectory.")
    if state in config.parents and config != state / "cfg":
        raise SetupError("Inside the app-data directory, config must use its cfg subdirectory.")
    if install in (state, config):
        raise SetupError("Use a separate directory for Compose/setup records.")
    storage = plan.get("storage")
    if storage and storage.get("filesystem") == "ntfs":
        mount = Path(storage["mount"])
        if any(path == mount or mount in path.parents for path in (state, config, install)):
            raise SetupError("Use a Linux filesystem for app homes, private configuration and setup records. "
                             "The NTFS option is available for shared libraries; keep the suggested Linux paths.")
    for library in map(Path, plan["libraries"].values()):
        if any(library == path or library in path.parents or path in library.parents
               for path in (state, config, install)):
            raise SetupError("Libraries must be separate from app data, config, and Compose directories.")
    libraries = list(map(Path, plan["libraries"].values()))
    for i, library in enumerate(libraries):
        if any(library == other or library in other.parents or other in library.parents for other in libraries[:i]):
            raise SetupError("Use distinct, non-overlapping library directories.")


def apply(plan, config, rules):
    os.umask(0o022)
    preflight_existing(plan)
    if shutil.which("docker") and Path("/var/run/docker.sock").exists():
        running = output("docker", "ps", "--format", "{{.Names}} {{.Image}}")
        if any("wolf" in line.lower() for line in running.splitlines()):
            raise SetupError("A Wolf-related container is already running; stop it before a new installation.")
    if Path("/var/run/wolf/wolf.sock").exists():
        raise SetupError("A Wolf API socket already exists; inspect the existing service before continuing.")
    install_docker(plan["distro"])
    # A new setup must not silently take over an existing Wolf instance or API socket.
    containers = output("docker", "ps", "--format", "{{.Names}} {{.Image}}")
    if any("wolf" in line.lower() for line in containers.splitlines()):
        raise SetupError("A Wolf-related container is already running; stop it before installing a new server.")
    if Path("/var/run/wolf/wolf.sock").exists():
        raise SetupError("A Wolf API socket already exists; inspect the existing service before continuing.")
    image = f"ghcr.io/greaper88/wolf:{CHANNELS[plan['channel']][1]}"
    # Fail on inaccessible images before any requested disk changes.
    for reference in [image, *plan["images"]]:
        run("docker", "pull", reference)
    apply_storage(plan["storage"], plan["distro"], plan["uid"], plan["gid"])
    Path(plan["install"]).mkdir(parents=True, exist_ok=True)
    Path(plan["config"]).mkdir(parents=True, exist_ok=True)
    for path in [plan["state"], *plan["libraries"].values()]:
        prepare_directory(path, plan["uid"], plan["gid"])
    if "steam" in plan["libraries"]:
        prepare_directory(plan["libraries"]["steam"] + "/steamapps", plan["uid"], plan["gid"])
    if run("getent", "group", "input", capture=True, check=False).returncode:
        run("groupadd", "--system", "input")
    atomic_write("/etc/udev/rules.d/85-wolf.rules", rules, backup=True)
    atomic_write("/etc/modules-load.d/wolf.conf", "uinput\nuhid\n", backup=True)
    for module in ("uinput", "uhid"):
        run("modprobe", module)
    run("udevadm", "control", "--reload-rules")
    run("udevadm", "trigger", "--subsystem-match=misc", "--sysname-match=uinput")
    run("udevadm", "trigger", "--subsystem-match=misc", "--sysname-match=uhid")
    run("udevadm", "settle")
    if not all(Path(x).exists() for x in ("/dev/uinput", "/dev/uhid", "/dev/dri", "/run/udev")):
        raise SetupError("Required input/DRM/udev devices are missing; check host kernel support.")
    nvidia_devices = build_nvidia(plan) if plan["gpu"]["mode"] == "nvidia" else []
    Path("/var/run/wolf").mkdir(exist_ok=True)
    # tmpfiles recreates the socket directory after /run is cleared on reboot.
    atomic_write("/etc/tmpfiles.d/wolf-setup.conf", "d /run/wolf 0755 root root -\n")
    compose_path = Path(plan["install"]) / "docker-compose.yml"
    compose = compose_document(plan, nvidia_devices)
    # JSON is a YAML subset and avoids hand-built YAML quoting.
    atomic_write(compose_path, json.dumps(compose, indent=2) + "\n")
    atomic_write(Path(plan["config"]) / "config.toml", config, mode=0o600)
    atomic_write(Path(plan["install"]) / "setup-plan.json", json.dumps(plan, indent=2) + "\n", mode=0o600)
    run("docker", "compose", "--file", compose_path, "config", "--quiet")
    atomic_write("/etc/systemd/system/wolf-setup.service", service_text(plan, shutil.which("docker")))
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "--now", "wolf-setup.service")
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2)
                client.connect("/var/run/wolf/wolf.sock")
                client.sendall(b"GET /api/v1/clients HTTP/1.0\r\nHost: localhost\r\n\r\n")
                if b" 200 " in client.recv(1024).split(b"\r\n", 1)[0]:
                    print("\nWolf is running and its API is responding. Pair this host in Moonlight.")
                    print("Service: systemctl status wolf-setup\nLogs: journalctl -u wolf-setup -f")
                    print(f"Configuration: {plan['config']}/config.toml")
                    return
        except OSError:
            pass
        time.sleep(2)
    raise SetupError("Wolf did not respond within 180 seconds. Inspect journalctl -u wolf-setup "
                     "and the saved setup-plan.json; configuration was preserved.")


def collect_plan(args):
    distro = distro_info(read_os(), platform.machine())
    print("Detected " + distro["name"] + "; Docker repository: " + distro["id"] + " " + distro["version"])
    if distro["derivative"]:
        print("This is a derivative distribution; Docker does not officially support derivatives.")
        if not yes("Use its identified parent distribution's Docker repository?"):
            raise SetupError("Parent-distribution Docker installation declined.")
    channel = args.channel or ("stable", "testing")[choose("Wolf release channel", [
        "Stable — auto-gpu-selection / latest", "Testing (beta) — dev-gpu-selection / gpu-selection-dev"])]
    gpu = select_gpu(detect_gpus())
    version = nvidia_version() if gpu["mode"] == "nvidia" else None
    if version:
        gpu["cuda_uuid"] = nvidia_uuid(gpu["pci"])
    custom = choose("Configuration", ["Default paths", "Custom paths and optional drive setup"]) == 1
    storage = None
    if custom:
        devices = block_inventory()
        show_storage(devices)
        storage = select_storage(devices)
    base = storage["mount"] + "/wolf" if storage else "/srv/wolf"
    app_base = "/srv/wolf" if storage and storage["filesystem"] == "ntfs" else base
    install, state, config = "/opt/wolf", "/etc/wolf", "/etc/wolf/cfg"
    if custom:
        install = path_question("Compose and setup records directory", "/opt/wolf")
        state = path_question("Program data and per-user app homes directory", app_base + "/apps")
        config = path_question("Wolf configuration directory", app_base + "/config")
    libraries = {}
    for key, name, folder in [("steam", "shared SteamLibrary", "SteamLibrary"),
                              ("roms", "shared ROM folder", "ROMs"),
                              ("installers", "shared installer library", "Installers")]:
        if yes("Configure a " + name + "?"):
            libraries[key] = path_question(name + " host path", base + "/" + folder)
    uid, gid = 1000, 1000
    if custom:
        while True:
            try:
                uid, gid = int(ask("User ID for app files", 1000)), int(ask("Group ID for app files", 1000))
                if not (0 < uid < 2**31 and 0 < gid < 2**31):
                    raise ValueError
                break
            except ValueError:
                print("Use positive numeric IDs; app containers must not run as root.")
    hostname = ask("Name shown in Moonlight", "Wolf")
    template, rules, revision = load_sources(channel, args.source_dir)
    selinux = Path("/sys/fs/selinux/enforce").exists() and Path("/sys/fs/selinux/enforce").read_text().strip() == "1"
    config_text, images = configure_apps(template, channel, libraries, hostname, selinux)
    plan = {"channel": channel, "branch": CHANNELS[channel][0], "revision": revision,
            "distro": distro, "gpu": gpu, "driver_version": version,
            "driver_volume": "wolf-nvidia-" + version if version else None,
            "storage": storage, "install": install, "state": state, "config": config,
            "libraries": libraries, "uid": uid, "gid": gid, "images": images, "selinux": selinux}
    validate_layout(plan)
    preflight_existing(plan)
    return plan, config_text, rules


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=CHANNELS)
    parser.add_argument("--dry-run", action="store_true", help="read-only discovery and plan; never installs or formats")
    parser.add_argument("--source-dir", help="use configs from a checkout on the selected branch (development/offline preview)")
    args = parser.parse_args(argv)
    if not args.dry_run and os.geteuid() != 0:
        raise SetupError("Run with sudo, or use --dry-run to preview without root.")
    if not Path("/run/systemd/system").exists():
        raise SetupError("A Linux host booted with systemd is required (not a container or WSL).")
    if not args.dry_run and run("systemd-detect-virt", "--container", "--quiet", capture=True, check=False).returncode == 0:
        raise SetupError("Run setup on the Linux host, not inside a container or WSL.")
    for command in ("lsblk", "df", "blkid", "wipefs", "setpriv", "findfs", "findmnt", "blockdev"):
        if not shutil.which(command):
            raise SetupError(f"Missing {command}; install your distro's util-linux package first.")
    # Never send GPU/storage bind mounts to a remote, rootless, or Desktop Docker context.
    os.environ["DOCKER_HOST"] = "unix:///var/run/docker.sock"
    for name in ("DOCKER_CONTEXT", "DOCKER_TLS_VERIFY", "DOCKER_CERT_PATH", "COMPOSE_FILE", "COMPOSE_PROJECT_NAME"):
        os.environ.pop(name, None)
    plan, config, rules = collect_plan(args)
    print("\nInstallation plan:\n" + json.dumps(plan, indent=2))
    print("Docker CE, Buildx and Compose will come from download.docker.com. "
          "All app images listed above will be pulled before launch.")
    if args.dry_run:
        print("\nPreview complete. No host changes were made.")
        return 0
    if not yes("Apply this setup and start Wolf?", True):
        print("Setup cancelled.")
        return 0
    # Prevent two installer processes from formatting or writing simultaneously.
    with open("/run/wolf-setup.lock", "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise SetupError("Another Wolf installer is running.") from exc
        apply(plan, config, rules)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (SetupError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"\nSetup stopped: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nSetup interrupted. Completed disk/package changes are not undone.", file=sys.stderr)
        sys.exit(130)
