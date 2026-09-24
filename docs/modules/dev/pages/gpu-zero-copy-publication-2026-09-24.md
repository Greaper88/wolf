# Zero-copy image publication: 2026-09-24

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 08:04:15 UTC

The model designation is the maintainer-supplied session label. This timestamp records
preparation of the deployment report. Implementation, limitations and test details are
in [the zero-copy assistance disclosure](ai-assistance-zero-copy.md).

## Published image

- Source: `bf90b0f704fcc4e052f322f0cfff4427197c14b4`, `dev-gpu-selection`.
- Tags: `ghcr.io/greaper88/wolf:gpu-selection-dev` and
  `ghcr.io/greaper88/wolf:gpu-selection-bf90b0f`.
- Digest: `sha256:6412763a626b41645f62f096b097f4d514f4a7bd6f1cd7b5a80d79f339895770`.
- Linux/AMD64; built with `docker/wolf.Dockerfile`, two compiler jobs, its default
  GStreamer 1.26.7 base, and source/AI disclosure OCI labels.
- The release filesystem layers exactly match the tested candidate. Release labels
  were added after committing the verified source. Anonymous GHCR manifest retrieval
  succeeded. The companion Wolf UI image is unchanged.

## Deployment

After the maintainer requested trying the update, only the Wolf Compose service was
recreated. The existing `/root/wolf/docker-compose.yml` already selected the public
development tag with `pull_policy: always`; no Compose edit was necessary.

The running container matched the published digest and source revision with zero
container restarts. Wolf and embedded PulseAudio reported RUNNING. Moonlight's
server-info endpoint returned HTTP/XML 200; the Unix-socket API and Wolf Den returned
HTTP 200. There were no active sessions at this check. The previous XFCE container
had been removed and the previous Wolf UI container had exited.

Live startup verification confirmed RX7600 H.264/HEVC/AV1 zero-copy, WX4100 H.264/HEVC
CPU-buffer fallback, and exclusion of the Intel device's failed hardware codecs.
Strict zero-copy exclusion remains off by default for this first live trial.
The maintainer can enable it with `WOLF_GPU_REQUIRE_ZERO_COPY=true` and recreate Wolf.

No live Moonlight client or latency measurement was performed after this restart.
The isolated production-image streaming tests passed before publication in mixed,
strict and explicitly disabled zero-copy modes. The previous published image remains
available as `ghcr.io/greaper88/wolf:gpu-selection-ed64fe7` for rollback.

## Publication tools

In addition to the implementation tools listed in the assistance disclosure: Git and
GitHub CLI for source publication/account authentication; Docker/BuildKit, image
inspection, tagging, push and anonymous manifest checks; Docker Compose and
supervisorctl for deployment; Python HTTP/XML/JSON/socket checks; Codex execution,
patch and UTC clock tools. Registry credentials were passed through stdin using a
temporary Docker config directory removed after publishing. A temporary Docker helper
read the existing root-owned Compose directory and accessed the Docker socket for the
authorized service recreation. No credential values were committed or put in labels.

This is a later documentation-only record; the image's exact source revision is above.
CI-skip commits avoid inherited workflows that publish unrelated image tags.
