# Manual GPU selection for Wolf and Wolf-UI

## Goal

Implement issue #449's API-first manual GPU choice. Keep automatic startup
capability probing; leave automatic placement and load-balancing policies on
`dev-gpu-selection`. Correctness and the requested features take priority over a
line-count target. Prefer existing Wolf components wherever they fit.

Baseline: upstream stable `2273b2e` for Wolf and upstream main `60908b9` for Wolf-UI.
Both new worktrees use `dev-manual-gpu-selection`; the automatic branches remain
intact.

## Reviewable pieces

1. Discover GPUs and probe hardware encoding on each at startup. Reuse discovery,
   physical-device binding and bounded test encoding from the automatic branch.
   Keep this independent of any assignment policy.
2. Add `GET /api/v1/gpus` and optional `gpu_id`/`source_session_id` on the existing
   lobby-create endpoint. Validate the selected GPU against the client's negotiated
   format. Reuse existing lobby runner/compositor render-node settings.
3. Move the viewer's encoder when entering a selected lobby, preserve stream
   counters, and restore its original launcher pipeline when leaving. Resolve
   NVIDIA PCI identity through CUDA rather than assuming device ordering.
4. Add the Wolf-UI launch picker. Separately review the header GPU count and
   refreshable status panel: names, VRAM total/use, utilization and viewer/app counts.
   Status metrics inform the user; they never drive placement.

The status panel remains part of the requested deliverable. Its presentation and
telemetry can be reviewed separately without removing manual selection or probing.

## Existing foundations and unresolved scope

Stable already supplies lobby render-node choices, a GStreamer pipeline builder,
encoder configuration and stream-producer switching. Public `feat/vulkan-image`
(PR #450) changes Vulkan encoding/image handling; it is not an API picker to import.
Public `fix/nvidia-zero-copy` and `fix/nvidia-blackwell-zero-copy` also contain
experimental NVIDIA memory-handoff fixes. Their presence is not proof that they
work on every NVIDIA model. No unpublished dependency is assumed.

The current extraction verifies SDR 8-bit 4:2:0 H.264/HEVC/AV1 with VA or NVENC.
It does not yet verify HDR, 4:4:4 or QSV-only paths. These must remain unavailable
for explicit selection until their probing and routing are implemented and tested.
Same-device zero-copy is a hard acceptance requirement, not a deferred optimization.
Manual pipelines must use DMA-BUF/VA memory or shared CUDA memory with a
converter and encoder bound to the selected GPU. No CPU fallback is allowed.
Actual GPU-memory negotiation and latency must be checked on hardware.

Container-level Vulkan/OpenGL device isolation is deliberately deferred at the
user's request. Neither the automatic branch nor the inspected Vulkan branch
changes stable's broad NVIDIA Container Toolkit device exposure.

## Validation and acceptance

The RX 7600 production-path smoke test has passed with actual DMA-BUF and VA
allocations. The WX 4100 path was correctly rejected with this installed driver.
The developer reports successful single-user app launch, a refreshing status
panel, and Steam identifying the selected GPU. Return-to-launcher, co-op/viewer
counts, measured latency and real NVIDIA validation remain pending.


- Build Wolf and Wolf-UI; run focused API, capability, CUDA identity, lobby
  compatibility, serialization and RTSP checks.
- Exercise actual UI controls against a sample API, including unavailable metrics,
  an incompatible GPU and cancellation. This is UI validation, not hardware proof.
- On real multi-GPU hardware, launch on each device, return to the launcher,
  reconnect and join co-op; verify actual rendering and encoding device identities.
- Test mixed vendors and multiple NVIDIA devices. Confirm session limits produce
  an actionable error without silently assigning a different GPU.
- Verify GPU-memory negotiation across the compositor/interpipe/converter/encoder
  boundary and measure stream continuity and latency. A host-memory fallback is
  a failed acceptance test.

Do not present successful compilation or sample telemetry as completed hardware
acceptance. See `docs/modules/dev/pages/manual-gpu-selection.md` for the current
API contract and implementation limits.
