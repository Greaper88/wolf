# GPU footer metadata: 2026-09-25

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-25 03:09:20 UTC

The model designation is the maintainer-supplied session label.

## Change

Session GPU metadata now includes device-wide GPU usage and the stream's negotiated
codec for the companion Wolf-UI bottom bar. The API refreshes telemetry and names
for the session's current GPU, including after joining a persistent app on another
GPU. Missing telemetry remains unknown. Existing encoder diagnostics stay in the
API; the UI no longer displays them.

GPU discovery resolves human-readable names from the firmware, the installed AMD
device/revision database, or the installed PCI vendor/device database. AMD revision
matching avoids confusing models that share a device ID. Generic vendor names are
used when the databases cannot identify a model. Production name databases are
loaded once rather than during every telemetry refresh.

GPU admission, load balancing, session pinning and stream-session counting are
unchanged. The footer's other-user count retains its existing session-count
semantics; it does not count distinct profiles.

## Verification

- Built `wolf` and `wolftests` in the isolated build container.
- Five standalone GPU suites passed, including added discovery tests for AMD
  revision matching, PCI vendor scoping, name fallbacks and unavailable telemetry.
- Existing pairing, apps, profiles, SSE and serialization cases passed:
  69 assertions in six cases.
- Disposable real-stream checks passed on the RX 7600 and Radeon Pro WX 4100.
  Both reported the correct human-readable names, HEVC and GPU usage. A second
  device joined a persistent app on the first GPU, continued receiving frames,
  and returned to its original launcher GPU. The app survived launcher shutdown.
- C++ formatting, Python syntax and `git diff --check` passed.
- The companion UI built with no warnings or errors and exported successfully.
  Its actual footer scene was rendered at ordinary and narrow widths, checking
  left alignment, truncation and separation from the right-side action hints.

Initial verification used local candidates `wolf:gpu-footer-candidate` and
`wolf-ui:gpu-footer-candidate`. Release builds, publication digests and deployment
are recorded in [the publication record](gpu-footer-publication-2026-09-25.md).

## Tools

Codex execution, patch, image-viewing and UTC clock tools; Git and ripgrep for local
source inspection; Docker build, exec, copy and isolated test containers;
CMake/Ninja and the existing compiler toolchain; clang-format 21; Python for API
and hardware-stream checks. Companion UI verification used .NET 8, Godot 4.4.1,
Xvfb and small temporary C#/GDScript fixtures. No subagents or external research
were used. Test containers have private state and no host Docker socket; reported
metadata excludes credentials and session keys.
