# AI assistance disclosure: Mesa GPU isolation

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-20 07:08:42 UTC

The timestamp records this disclosure's preparation during the final review session;
implementation and validation occurred across preceding turns. The model designation
is the session label supplied by the maintainer.

## Contribution

AI assistance was used to inspect the existing automatic-selection implementation,
write and revise C++ changes, add regression tests, update documentation, diagnose
user-provided logs, build development images, and run the checks listed below.
The maintainer directed scope and reported a successful FurMark check inside XFCE:
only the assigned GPU was visible. That report is human validation, not an automated
FurMark test performed by the agent.

The changes set per-application Mesa PCI selectors, validate Docker device exposure,
fix DRM descriptor lifetime handling, and rebind saved device-specific VA encoder
templates to a selected GPU in the same codec and normal/low-power family.
Only the automatic-selection branch is included.

## Tools used

- Codex execution tools (`functions.exec`, `exec_command`, `write_stdin`) and Bash
  for repository inspection, editing, builds, tests, and log review.
- Git for branch/status/diff checks, commit preparation, and publication.
- ripgrep and standard file-reading tools for targeted code and log searches.
- Python 3 for targeted source edits and disposable integration fixtures; ctypes
  called the Vulkan loader to enumerate physical devices in diagnostic containers.
- clang-format 21 for C++ formatting.
- CMake, its configured native build tools, and CTest for six standalone GPU suites;
  AddressSanitizer and UndefinedBehaviorSanitizer for additional test runs
  (`ASAN_OPTIONS=detect_leaks=0`).
- Docker/BuildKit and the repository's `docker/wolf.Dockerfile` for image builds;
  Docker inspection, temporary containers, and exec for configuration and GPU checks.
- Mesa `eglinfo` for OpenGL renderer checks and the Vulkan loader for enumeration.
- GStreamer and the Wolf API/RTSP/RTP paths for disposable live-session and video tests.
- Codex web search/open tools to consult Mesa PRIME/device-selection documentation
  and investigate Vulkan PCI-property behavior; official documentation:
  https://docs.mesa3d.org/envvars.html#dri-prime
- Codex task-reading tools to check the separate manual-selection task's scope.
- Codex asynchronous questions for maintainer scope/disclosure requirements and
  the UTC clock tool for the disclosure timestamp.

## Validation and limits

All six focused GPU suites passed normally and with the sanitizers above. The full
Wolf image built successfully as `wolf:gpu-isolation-dev`. OpenGL selected the
assigned WX4100 and RX7600 in separate device-restricted containers. Vulkan exposed
one assigned hardware GPU in each restricted test. A modern Vulkan instance also
selected the intended PCI device with all DRM devices exposed; an older instance
without the necessary PCI-property support demonstrated why process-runner settings
alone are insufficient for device isolation.

Two disposable live Wolf app processes received selectors matching their respective
GPU assignments. After reproducing the saved-template mismatch in unit tests, an
isolated Wolf server using templates naming renderD129 produced actual H.264 video
packets from sessions on both renderD129 and renderD130. Regression tests cover
H.264, HEVC, AV1, normal/low-power families, and invalid or mismatched templates.
The maintainer's subsequent logs show XFCE using the assigned WX4100 nodes and an
HEVC pipeline, consistent with their successful FurMark visibility check.

The complete hardware-dependent Catch2 suite was not run. These checks do not claim
hostile-container security isolation, NVIDIA support, or exhaustive driver/game
compatibility. Process runners receive environment selection only. Running app
containers must be recreated to adopt the device restrictions. Disposable servers
used temporary configuration, no host Docker socket, and no user app state.

For future AI-assisted changes on this fork, retain the AI-Agent, AI-Model-Version,
and AI-Inference-Date fields and record the actual tools and validation used for
that change; do not copy this timestamp or these results as evidence for later work.
