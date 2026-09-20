# Manual GPU selection

This branch starts at upstream stable `2273b2e` and extracts device discovery,
hardware encoder verification, and stream switching from `dev-gpu-selection`.
It does not include the automatic selector, load thresholds, reservations,
blacklists, admission scoring, automatic retries on another GPU, or configuration
migration. The launcher continues to use its configured GPU.

## API and UI

`GET /api/v1/gpus` returns the GPUs visible inside Wolf, human-readable names,
render nodes, nullable VRAM total/used bytes and GPU/encoder utilization, verified
codec availability, connected Wolf viewers, and resident app lobbies. Counts are
not operating-system login counts. A viewer counts once on the GPU currently
streaming to that viewer. An app with no connected viewers remains in the app
count. Unknown telemetry is absent/null, never a fabricated zero.

`POST /api/v1/lobbies/create` accepts optional `gpu_id` and `source_session_id`.
An explicit choice requires an active source session and a verified encoder for
its negotiated codec. The server sets both runner and compositor render nodes;
clients cannot substitute a different node with the selected identity. Omitting
`gpu_id` preserves the existing create-lobby behavior.

Joining a manually selected lobby checks every viewer's negotiated codec, moves
that viewer's encoder to the selected GPU, and preserves packet/frame counters
across pipeline restarts. Leaving returns to the original launcher pipeline.
Single-user and co-op launches use the same selection API.

The companion Wolf-UI branch asks for a GPU before each new launch. Reconnecting
to a running app keeps its GPU. The header GPU count opens a panel refreshed every
five seconds with device names, VRAM use/total, GPU utilization, connected viewers,
and running apps. A configured-GPU choice preserves the existing launch behavior.

## Current implementation limits

Manual routing accepts verified VA (AMD/Intel) and NVIDIA NVENC encoders for SDR
8-bit 4:2:0 H.264/HEVC/AV1 where the device passes an actual encode probe. NVIDIA
selection resolves PCI identity through the CUDA driver, checks that the encoder
factory belongs to that CUDA device, and supplies that same device to the pipeline
context. It never substitutes device zero for a failed manual lookup. The existing
configured/default GPU path retains stable behavior.

QSV-only, Vulkan, HDR and 4:4:4 manual routing are not advertised as verified.
Zero-copy is required for manual selection. The producer and encoder are bound
to the selected GPU. VA paths require DMA-BUF input, the device-specific VA
postprocessor and VA-memory encoder input; NVIDIA requires CUDA memory and a
shared explicitly selected CUDA context. No CPU converter or upload fallback is
inserted. A missing GPU-memory path makes that GPU unavailable for manual
selection. Runtime negotiation failures stop the stream rather than silently
copying frames through CPU memory. This still requires hardware verification,
especially on NVIDIA models affected by upstream interop issues. The default
launcher pipelines are unchanged.

Capability probes run once at startup in bounded child processes. Restart Wolf
after changing devices/drivers. Devices must be exposed inside the container;
Wolf cannot report devices hidden by the container configuration. AMD sysfs and
NVIDIA NVML provide telemetry when available; drivers without a utilization or
local-memory reading show unavailable. No polling response runs a test encode.

## Inherited device-isolation limitation

This branch does not change container GPU isolation. Stable, `dev-gpu-selection`,
and `upstream/feat/vulkan-image` share the same runner device-exposure code. In
particular, the NVIDIA Container Toolkit path defaults to all NVIDIA device IDs
and `NVIDIA_VISIBLE_DEVICES=all`. Binding the selected compositor/encoder does not
guarantee that a guest application's Vulkan/OpenGL renderer cannot enumerate or
choose another exposed GPU. Restricting guest device visibility and auditing
Vulkan/OpenGL selection are separate follow-up work, intentionally outside this
branch. Normal linked-device mounts provide narrower exposure, but custom mounts
and runtime settings still matter.

## Related upstream work

- https://github.com/games-on-whales/wolf/issues/449 — manual per-launch selection.
- https://github.com/games-on-whales/wolf/pull/450 — `feat/vulkan-image`, native
  Vulkan encode/HDR and image changes. Not used as the baseline for this branch.
- `upstream/codex/party-mode-split-render` — separate GPU-aware party compositor.

## Validation

Run `wolftests '[gpu]'` and `manual_gpu_encoder_tests` (through CTest to load its
fake CUDA library), then the existing API, serialization and RTSP tests.
Hardware acceptance requires launching on each selectable GPU, returning to the
launcher, joining with a second compatible/incompatible client, and confirming
live usage/VRAM readings. Do not equate successful compilation with this hardware
acceptance test.

### Hardware check executable

Build `manual_gpu_zero_copy_smoke` and run it with a render-node basename, e.g.
`manual_gpu_zero_copy_smoke renderD129`, inside a runtime exposing that GPU.
It uses the production discovery, encoder probe and pipeline builder, runs the
compositor and encoder in separate pipelines connected by interpipe, and checks
actual DMA-BUF/VA memory plus at least 60 encoded frames. This opt-in executable
currently covers VA; NVIDIA needs a separate shared-context hardware run.

### Validation recorded for this working tree

- Wolf-UI: Debug build and Linux release export succeeded. A Godot UI check with
  sample API data verified GPU response decoding, disabled incompatible entries,
  selected device identity, and the rendered picker/status panel.
- Wolf: focused GPU, API, RTSP and serialization checks passed 301 assertions in
  17 cases; the standalone encoder/CUDA identity checks also passed.
- RX 7600 (`renderD129`): production pipeline builder passed the hardware smoke
  test at 1280x720/60 with 60 encoded frames. The compositor emitted DMA-BUF
  allocations; the encoder received VA-memory allocations. Both negotiated
  square pixels. The real probe child also returned the matching encoder and
  device-specific VA converter.
- Pro WX 4100 (`renderD130`): the installed VA converter advertises no DMA-BUF
  sink capability. The production builder rejects this manual path instead of
  substituting CPU conversion. This is not a successful zero-copy test for that GPU.
- No NVIDIA hardware was available. CUDA identity tests use a fake driver and
  do not establish real NVIDIA rendering/encoding interoperability.
- Developer-reported live check: one user successfully launched an app, saw the
  status panel refresh, and confirmed that Steam identified the selected GPU.
  Return-to-launcher, co-op/viewer counts and measured latency remain pending. HDR/4:4:4, QSV-only paths and container device isolation remain outside
  the verified scope described above.
