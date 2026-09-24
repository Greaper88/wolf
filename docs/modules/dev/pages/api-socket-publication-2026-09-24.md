# API response fix publication: 2026-09-24

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 19:52:14 UTC

The model designation is the maintainer-supplied session label. Implementation and
regression details are in [the assistance disclosure](ai-assistance-api-socket-writes.md).

## Published image

- Source: `785045d56d077ba4293100013692b8be2c1735bd` on `dev-gpu-selection`.
- Tags: `ghcr.io/greaper88/wolf:gpu-selection-dev` and
  `ghcr.io/greaper88/wolf:gpu-selection-785045d`.
- Digest: `sha256:dfca09c3ff18f9385f74cf3736a7f692dfdb43d0f68d661a5c39d8830ec20f3c`.
- Linux/AMD64, built from the committed source with `docker/wolf.Dockerfile`, two
  compiler jobs and the existing GStreamer 1.26.7 base. OCI labels include the exact
  source revision, fork source URL, version, agent, model designation and UTC date.
- Both tags passed anonymous GHCR manifest retrieval with the same digest.
- Local `wolf:gpu-selection-dev` points to this release. Wolf-UI is unchanged.

The production image passed the fake-Docker regression before publication: two
complete, ordered 1,800-record progress responses; an intervening download whose
client disconnected; six intact SSE start/end events; and a subsequent 404 response.
Its logs contained zero repeated response-write errors. The earlier API test run
passed 58 assertions in four cases. No additional stream/latency claims are made.

## Deployment

After the maintainer explicitly requested publication and deployment, recreated only
the Wolf service using the existing `/root/wolf/docker-compose.yml`, with a 45-second
graceful shutdown allowance. No Compose edits were needed. The prior Steam and
Wolf-UI session containers were removed during shutdown; profile data remains on
the existing persistent mounts.

Post-deployment checks verified the exact release digest and source revision, zero
container restarts, and RUNNING status for Wolf and embedded PulseAudio. The Wolf
API returned HTTP 200 with zero sessions. Moonlight serverinfo returned HTTP/XML
200. Wolf Den and its private socket proxy both returned HTTP 200, with the proxy
check performed as its application user.

Startup verification again confirmed RX7600 H.264/HEVC/AV1 zero-copy, WX4100
H.264/HEVC CPU-buffer fallback, and exclusion of the Intel GPU's failed hardware
codecs. No bad-descriptor flood appeared in the new startup logs. A real Moonlight
client reconnect was not performed by the assistant after deployment.

The previous release remains available as
`ghcr.io/greaper88/wolf:gpu-selection-bf90b0f`, digest
`sha256:6412763a626b41645f62f096b097f4d514f4a7bd6f1cd7b5a80d79f339895770`.

## Publication tools

Codex execution, patch and UTC clock tools; Git/GitHub CLI; Docker/BuildKit, Compose,
image metadata inspection, tagging/push and isolated regression containers; Python
HTTP/XML/JSON/socket checks; supervisorctl and curl for service verification.
Registry credentials were passed through stdin using a temporary Docker config
directory removed after publication. A temporary Docker helper read the root-owned
Compose directory and accessed the Docker socket for the authorized service update.
The production regression container and its disposable volume were removed.

This is a later documentation-only record. The image's exact source revision is
listed above; CI-skip commits avoid inherited workflows publishing unrelated tags.
