#!/usr/bin/env python3
"""Provision immutable, version-matched NVIDIA app driver volumes at Wolf startup."""
import argparse
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import platform
import re
import socket
import sys
import tarfile
import urllib.parse
import uuid

SCHEMA = "1"
MANIFEST = ".wolf-driver-manifest.json"
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+\Z")
NAME_PATTERN = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*\Z")
CORE64 = ("libcuda", "libnvidia-ml", "libnvidia-encode", "libnvidia-allocator",
          "libGLX_nvidia", "libEGL_nvidia", "libnvidia-glcore", "libnvidia-eglcore")
CORE32 = ("libGLX_nvidia", "libEGL_nvidia", "libnvidia-glcore", "libnvidia-eglcore")

def log(message):
    print("[NVIDIA drivers] " + message, file=sys.stderr, flush=True)

def check_version(version):
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError("Invalid loaded NVIDIA driver version")
    return version

def loaded_version():
    # No Docker requests, NVML loads or provisioning on AMD/Intel-only systems.
    module = Path("/sys/module/nvidia/version")
    if not module.is_file():
        return None
    return check_version(module.read_text().strip())

def required_files(version):
    files = {f"lib/{name}.so.{version}": 2 for name in CORE64}
    files.update({f"lib32/{name}.so.{version}": 1 for name in CORE32})
    files["lib/gbm/nvidia-drm_gbm.so"] = 2
    return files

def contained(root, path):
    if not path.resolve(strict=True).is_relative_to(root.resolve()):
        raise ValueError("Driver path escapes its volume: " + str(path))

def probe_files(root, version):
    for name, elf_class in required_files(version).items():
        path = root / name
        contained(root, path)
        with path.open("rb") as source:
            header = source.read(5)
        if header != b"\x7fELF" + bytes([elf_class]):
            raise ValueError("Missing or incorrect driver architecture: " + name)
    for name in ("share/glvnd/egl_vendor.d/10_nvidia.json", "share/vulkan/icd.d/nvidia_icd.json"):
        json.loads((root / name).read_text())

def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()

def seal(root, version):
    check_version(version)
    probe_files(root, version)
    files, links = {}, {}
    for path in sorted(root.rglob("*")):
        name = str(path.relative_to(root))
        if name == MANIFEST:
            continue
        if path.is_symlink():
            contained(root, path)
            links[name] = os.readlink(path)
        elif path.is_file():
            files[name] = digest(path)
    (root / MANIFEST).write_text(json.dumps(dict(schema=SCHEMA, version=version, files=files, links=links)))

def validate(root, version):
    check_version(version)
    manifest = json.loads((root / MANIFEST).read_text())
    if manifest.get("schema") != SCHEMA or manifest.get("version") != version:
        raise ValueError("Driver manifest does not match the loaded kernel driver")
    probe_files(root, version)
    for name, checksum in manifest["files"].items():
        path = root / name
        contained(root, path)
        if path.is_symlink() or digest(path) != checksum:
            raise ValueError("Driver volume file changed: " + name)
    for name, target in manifest["links"].items():
        path = root / name
        contained(root, path)
        if not path.is_symlink() or os.readlink(path) != target:
            raise ValueError("Driver volume link changed: " + name)

class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path, timeout=900):
        super().__init__("localhost", timeout=timeout)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

class Docker:
    def __init__(self, path):
        self.path = path

    def request(self, method, path, body=None, missing_ok=False, raw_output=False):
        connection = UnixHTTP(self.path)
        try:
            connection.request(method, path, None if body is None else json.dumps(body),
                               {"Content-Type": "application/json"})
            response = connection.getresponse()
            raw = response.read()
            if missing_ok and response.status == 404:
                return None
            if not 200 <= response.status < 300:
                raise RuntimeError(f"Docker {method} {path}: {response.status}: {raw.decode(errors='replace')[:500]}")
            return raw if raw_output else (json.loads(raw) if raw else None)
        finally:
            connection.close()

    def build(self, version):
        image = "wolf-nvidia-driver:" + version + "-v" + SCHEMA
        if self.request("GET", "/images/" + image + "/json", missing_ok=True):
            return image
        log("Building driver package " + version + " (first use requires network access)")
        context = io.BytesIO()
        with tarfile.open(fileobj=context, mode="w") as archive:
            for source, target in [(Path(__file__).with_name("nvidia-driver.Dockerfile"), "Dockerfile"),
                                   (Path(__file__), "nvidia-driver-volume.py")]:
                archive.add(source, arcname=target)
        query = urllib.parse.urlencode(dict(t=image, version="1", rm="1",
                                            buildargs=json.dumps({"NV_VERSION": version})))
        connection = UnixHTTP(self.path)
        try:
            connection.request("POST", "/build?" + query, context.getvalue(), {"Content-Type": "application/x-tar"})
            response = connection.getresponse()
            if response.status != 200:
                raise RuntimeError("Driver build request failed: " + response.read().decode(errors="replace")[:500])
            for line in response:
                message = json.loads(line)
                if "error" in message:
                    raise RuntimeError("Driver package build failed: " + message["error"])
                if "stream" in message:
                    print(message["stream"], end="", file=sys.stderr, flush=True)
        finally:
            connection.close()
        self.request("GET", "/images/" + image + "/json")
        return image

    def helper(self, image, volume, command, readonly):
        container = self.request("POST", "/containers/create", dict(
            Image=image, Entrypoint=[], Cmd=command, User="0:0", NetworkDisabled=True,
            Labels={"wolf.nvidia.helper": "true"},
            HostConfig=dict(Binds=[volume + ":/usr/nvidia:" + ("ro" if readonly else "rw")],
                            NetworkMode="none", CapDrop=["ALL"], SecurityOpt=["no-new-privileges"])))
        cid = container["Id"]
        try:
            self.request("POST", "/containers/" + cid + "/start")
            status = self.request("POST", "/containers/" + cid + "/wait?condition=not-running")
            if status["StatusCode"] != 0:
                output = self.request("GET", "/containers/" + cid + "/logs?stdout=1&stderr=1", raw_output=True)
                lines = []
                while len(output) >= 8:
                    size = int.from_bytes(output[4:8], "big")
                    lines.append(output[8:8 + size].decode(errors="replace"))
                    output = output[8 + size:]
                log("Volume check failed: " + "".join(lines).strip()[:1000])
            return status["StatusCode"] == 0
        finally:
            self.request("DELETE", "/containers/" + cid + "?force=1&v=1")

def helper_image(docker):
    override = os.environ.get("WOLF_NVIDIA_DRIVER_HELPER_IMAGE")
    if override:
        return override
    # The usual Docker hostname is its ID. Fall back to the hostname bind mount
    # when an administrator supplied a custom hostname.
    self_container = docker.request("GET", "/containers/" + urllib.parse.quote(socket.gethostname(), safe="") + "/json",
                                    missing_ok=True)
    if not self_container:
        match = re.search(r"/containers/([a-f0-9]{64})/hostname", Path("/proc/self/mountinfo").read_text())
        if match:
            self_container = docker.request("GET", "/containers/" + match[1] + "/json")
    if not self_container:
        raise RuntimeError("Cannot identify Wolf's helper image; set WOLF_NVIDIA_DRIVER_HELPER_IMAGE to this Wolf image")
    return self_container["Image"]

def select_volume(docker, version, checker, explicit=None):
    check_version(version)
    command = ["python3", "/wolf/nvidia-driver-volume.py", "--validate-volume", "/usr/nvidia", version]
    if explicit:
        if not NAME_PATTERN.fullmatch(explicit):
            raise ValueError("Invalid configured NVIDIA driver volume name")
        if not docker.request("GET", "/volumes/" + explicit, missing_ok=True):
            raise RuntimeError("Explicit NVIDIA_DRIVER_VOLUME_NAME does not exist")
        if not docker.helper(checker, explicit, command, True):
            raise RuntimeError("Explicit NVIDIA driver volume failed validation; use manual mode for unmanaged volumes")
        return explicit
    filters = json.dumps({"label": ["wolf.nvidia.managed=true", "wolf.nvidia.version=" + version]})
    volumes = docker.request("GET", "/volumes?" + urllib.parse.urlencode({"filters": filters})).get("Volumes") or []
    for volume in volumes:
        name = volume["Name"]
        if docker.helper(checker, name, command, True):
            log("Reusing verified driver volume " + name)
            return name
        log("Volume " + name + " failed validation; keeping it and creating a replacement")
    image = docker.build(version)
    # A unique generation is never filled over a volume used by a running app.
    name = "wolf-nvidia-driver-" + version + "-" + uuid.uuid4().hex[:12]
    docker.request("POST", "/volumes/create", dict(Name=name, Labels={
        "wolf.nvidia.managed": "true", "wolf.nvidia.version": version, "wolf.nvidia.schema": SCHEMA}))
    try:
        if not docker.helper(image, name, ["python3", "/opt/wolf/nvidia-driver-volume.py", "--validate-volume", "/usr/nvidia", version], False):
            raise RuntimeError("New driver volume failed validation")
        if not docker.helper(checker, name, command, True):
            raise RuntimeError("New driver volume failed independent validation")
    except Exception:
        # Only this unpublished candidate is removed. Docker refuses deletion
        # if any container still uses it; old driver volumes are always retained.
        docker.request("DELETE", "/volumes/" + name)
        raise
    log("Created verified 64-bit and 32-bit driver volume " + name)
    return name

def startup():
    mode = os.environ.get("WOLF_NVIDIA_DRIVER_MODE", "auto").lower()
    explicit = os.environ.get("NVIDIA_DRIVER_VOLUME_NAME")
    if mode == "manual":
        return explicit or ""
    if mode == "toolkit":
        if explicit:
            raise ValueError("Toolkit mode conflicts with NVIDIA_DRIVER_VOLUME_NAME")
        return ""
    if mode not in ("auto", "volume"):
        raise ValueError("WOLF_NVIDIA_DRIVER_MODE must be auto, volume, toolkit or manual")
    version = loaded_version()
    if version is None:
        if mode == "volume":
            raise RuntimeError("Driver volume requested, but no loaded NVIDIA driver was detected")
        return ""
    if platform.machine() != "x86_64":
        raise RuntimeError("Automatic NVIDIA driver packaging currently supports Linux x86_64")
    docker = Docker(os.environ.get("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"))
    return select_volume(docker, version, helper_image(docker), explicit)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env-file")
    parser.add_argument("--seal-volume", nargs=2, metavar=("PATH", "VERSION"))
    parser.add_argument("--validate-volume", nargs=2, metavar=("PATH", "VERSION"))
    args = parser.parse_args()
    try:
        if args.seal_volume:
            seal(Path(args.seal_volume[0]), args.seal_volume[1])
        elif args.validate_volume:
            validate(Path(args.validate_volume[0]), args.validate_volume[1])
        else:
            name = startup()
            if args.env_file:
                Path(args.env_file).write_text(name + "\n")
            else:
                print(name)
    except Exception as error:
        log(str(error))
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
