# GPU-selection image publication: 2026-09-24

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 05:05:31 UTC

The model designation is the session label supplied by the maintainer. This timestamp
records preparation of this publication report; the image labels record build start
at 2026-09-24T04:48:17Z.

## Published artifact

- Source: `ed64fe7323ea1fff151d8d0f432b6401d1497d0e` on `dev-gpu-selection`.
- Development tag: `ghcr.io/greaper88/wolf:gpu-selection-dev`.
- Revision tag: `ghcr.io/greaper88/wolf:gpu-selection-ed64fe7`.
- Both tags: `sha256:92c7b87cd10af181e9da767b4e04bc52932a1481ae66530ee32654750c5a3e39`.
- Platform: Linux/AMD64. Built using `docker/wolf.Dockerfile`, four compiler jobs,
  and its default GStreamer base image. OCI labels identify the fork and source revision;
  `ai.*` labels record agent, model designation, and build timestamp.
- The existing companion `ghcr.io/greaper88/wolf-ui:gpu-selection-dev` remains unchanged.

The image contains the upstream resume-response correction and shared-IP fixes covered
by `ai-assistance-session-fixes.md`. This report is a later documentation-only commit;
the image source revision above is the exact committed build input.

## Validation

- Full production image build succeeded. Compilation emitted existing deprecation and
  exception-handler return-value warnings; it was not warning-free.
- Dynamic-library check found no missing dependencies for `/wolf/wolf`.
- A disposable server started with no network access, host Docker socket, published
  ports, or existing user data. It answered the local Moonlight server-info request
  with HTTP/XML status 200 and shut down gracefully. mDNS could not open sockets in
  that intentionally network-isolated container.
- Bounded probes using the built binary passed H.264/HEVC/AV1 on RX 7600
  (`renderD129`) and H.264/HEVC on WX4100 (`renderD130`). WX4100 AV1 and the Intel
  device's codecs were unavailable, matching previous validation.
- Both registry pushes completed, and anonymous manifest retrieval of the development
  tag succeeded. No registry authentication is required to pull the public image.
- Previous source validation: 198 assertions across nine RTSP/control/serialization
  test cases and all six standalone GPU suites passed; see the integration disclosure.
- After the maintainer explicitly authorized the immediate switch, the existing Compose
  file was backed up and its Wolf service changed to the published development tag with
  `pull_policy: always`. Only Wolf was recreated. It ran the expected image digest and
  revision with zero container restarts; Wolf and PulseAudio reported RUNNING. The live
  Moonlight endpoint, Unix-socket API, and Wolf Den each returned HTTP 200.

No end-to-end Moonlight streaming or reconnect test was performed after deployment.
Identifier-less legacy clients retain the documented shared-IP limitations.

## AI assistance and tools

AI assistance performed build/release orchestration, log review, bounded checks,
publication, the authorized Compose update, and this report. Tools used:

- Codex execution tools (`functions.exec`, `exec_command`, `write_stdin`), Bash,
  ripgrep, standard file utilities, and Git for source/status inspection and publication.
- Docker/BuildKit, CMake, Ninja, and the repository Dockerfile for the image build;
  Docker image inspection, temporary containers, tagging, pushing, and manifest checks.
- GitHub CLI authentication piped into Docker login using a temporary credential
  directory, removed after publishing; no token was written into source or image labels.
- Python 3 standard-library HTTP/XML/JSON/socket/subprocess utilities for smoke checks
  and the backed-up Compose edit; `ldd` for dynamic dependencies.
- Docker Compose and supervisorctl for deployment and process verification.
- Codex asynchronous user-input tool for restart timing, `apply_patch` for this report,
  UTC clock for timestamps, and bounded waits while build/push operations ran.

The source publication uses a CI-skip documentation commit because inherited workflows
publish unrelated shared tags. The image was built and checked locally as recorded here.
