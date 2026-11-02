#!/usr/bin/env bash
# Download this file, review it, then run: sudo bash setup.sh
set -euo pipefail

preview=false
for arg in "$@"; do
    if [[ $arg == --help || $arg == -h ]]; then
        echo 'Usage: sudo bash setup.sh [--channel stable|testing] [--dry-run] [--source-dir CHECKOUT]'
        echo 'Interactive Wolf setup. --dry-run requires Python already installed; makes no persistent host changes.'
        exit 0
    fi
    [[ $arg != --dry-run ]] || preview=true
done
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
python=python3
if ! command -v "$python" >/dev/null || ! "$python" -c 'import sys; assert sys.version_info >= (3, 9); import importlib.util; assert importlib.util.find_spec("tomllib") or importlib.util.find_spec("tomli")' 2>/dev/null; then
    if $preview; then
        echo 'Preview needs Python >= 3.9 and tomllib (Python 3.11+) or the distro python3-tomli package.' >&2
        exit 1
    fi
    [[ $EUID == 0 ]] || { echo 'Run with sudo.' >&2; exit 1; }
    # OS metadata is supplied by the host distribution.
    . /etc/os-release
    case "$ID" in
        ubuntu|debian|linuxmint|pop|raspbian)
            apt-get update
            apt-get install -y python3 python3-tomli ca-certificates curl
            ;;
        rhel|centos|rocky|almalinux)
            if [[ ${VERSION_ID%%.*} == 8 || ${VERSION_ID%%.*} == 9 ]]; then
                dnf install -y python3.11 ca-certificates curl
                python=python3.11
            else
                dnf install -y python3 ca-certificates curl
            fi
            ;;
        fedora) dnf install -y python3 ca-certificates curl ;;
        *) echo "Unsupported distro: $ID. See Docker's supported distributions." >&2; exit 1 ;;
    esac
fi

if [[ -f $script_dir/setup_wolf.py ]]; then
    exec "$python" "$script_dir/setup_wolf.py" "$@"
fi
# The installer is distributed from the primary branch. It resolves the user's
# selected release channel separately and pins all configuration downloads to one commit.
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT
"$python" - "$tmp_dir/setup_wolf.py" <<'PY'
import pathlib, sys, urllib.request
url = 'https://raw.githubusercontent.com/Greaper88/wolf/auto-gpu-selection/scripts/setup_wolf.py'
with urllib.request.urlopen(url, timeout=60) as response:
    pathlib.Path(sys.argv[1]).write_bytes(response.read())
PY
"$python" "$tmp_dir/setup_wolf.py" "$@"
