# Experimental per-session GPU selection

## Implemented

Mesa app runners receive a PCI-address `DRI_PRIME` selector with Vulkan's `!` suffix, independently
for each launcher/sub-app. Docker overrides are checked against the selected device's linked DRM
nodes and conflicting Mesa environment entries are replaced. Broad GPU exposure is rejected.
Process runners receive environment selection only. Existing containers must be recreated to adopt it.

Wolf verifies each eligible device's hardware codecs at startup/in the background using bounded
subprocess probes. Launches use cached capabilities plus fresh load measurements; they do not run
another test encode. Each session carries its selected device, verified encoder factories and video
context. Explicit environment GPU pins retain the legacy path. Eligible app configuration preserves
selection intent rather than saving the automatically resolved node as a manual override.

Automatic pipelines currently use VA hardware encoders and ordinary video buffers. No software
fallback is used. The UI shows the selected GPU, available telemetry and stream errors.

Single-user Wolf UI sub-apps inherit the launcher's GPU and hold independent retained assignments.
Disconnecting or closing the original launcher does not move or close the sub-app. Its profile ID
and runner state folder identify the persistent instance. A viewer joining from another device must
use that app's GPU and a supported codec; only the viewer's encoder is moved. Returning to Wolf UI
restores the launcher's target. Multiple simultaneous viewers of an automatic sub-app are rejected.
Both server and UI changes are required for these source-session/profile API fields.

Reservations distinguish pending encoders, active encoders and retained applications. Disconnecting
releases encoder demand after pipeline teardown while preserving the application and device.
Same-GPU producer switches preserve existing encoder admission. Reconnecting the same retained
session checks device readiness, memory and encoder capacity, but does not reject solely for overall
graphics-core load: the app is already rendering. Starting another viewer on a different launcher's
route currently goes through fresh targeted admission, including the graphics-load gate.

Encoder failures pause the stream and preserve the app. API errors explain retrying or explicitly
force-closing/restarting with possible loss of unsaved work. Moonlight's mid-stream control termination
has no custom error-text field, so the client may show a generic disconnect. Stop cancels admission
and holds ownership through teardown; late callbacks cannot release a replacement session's lease.

## Validation at the initial development push

- Wolf and Wolf UI Docker development images built successfully.
- All five standalone GPU suites pass normally and under ASan/UBSan. Leak detection was disabled;
  no leak-check result is claimed. Coverage includes device binding, subprocess timeout, cached
  verification, concurrency, retained ownership, encoder epochs, same-GPU handoff and resume gates.
- Hardware probes passed H.264/HEVC/AV1 on an RX 7600 and H.264/HEVC on a WX4100. The available Intel
  driver stack failed verification and was excluded.
- Disposable-server tests exercised real HEVC RTP output on both AMD GPUs, same-GPU joins,
  cross-device joins, preserved frame progression, return to launcher, profile mismatch rejection,
  and sub-app survival after the original launcher stopped. The script is
  `tests/gpu/subapps_integration.py`; it requires an explicitly disposable two-GPU fixture.
- Live Moonlight/Wolf UI/Steam testing confirmed separate profile state folders and GPU assignments,
  app launch and same-device reconnect. This is development validation, not a load/latency benchmark.
- Full server Catch2 tests, including the new serialization tests, have not been run. CI is skipped
  for this initial source checkpoint because the inherited workflows publish shared image tags.

## Known limitations and next work

- Automatic NVIDIA/NVENC contexts, custom video overrides, zero-copy and multi-user lobby sharing
  remain unsupported. Device-specific encoder discovery alone does not enable NVENC session routing.
- A new sub-app currently inherits the launcher GPU as a hard assignment. Preferred-GPU placement
  with fallback before the app starts remains to be implemented. Existing apps must never migrate.
- AMD/Intel encoder utilization remains unknown in the selector. AMD fdinfo encoder timing counters
  are present on the tested host but are not collected. Measuring Wolf alone would not establish
  whole-device usage; external desktops/encoders and deduplication must be accounted for.
- Unknown encoder telemetry uses session-count policy by default. Startup codec probes establish basic
  support, not maximum resolution/frame rate or guaranteed concurrent-stream capacity.
- The legacy IP-only session fallback can confuse devices behind the same public IP. This predates
  the GPU work and is not addressed here.
- Automatic lobby creation requires an automatic source session when the global runtime is enabled;
  mixed manual/automatic app configurations need further compatibility coverage.
- Session GPU counts describe tracked stream sessions, not unique human users or all resident apps.
- Existing multi-compositor Smithay warnings and full load/latency/shutdown stress testing remain
  follow-up work. A pre-existing compiler warning about IPv6 ancillary-buffer sizing also needs review.

## Reproduce standalone tests and builds

```sh
cmake -S tests/gpu -B build-gpu-policy -DTEST_GPU_ENCODER_PROBE=ON
cmake --build build-gpu-policy -j4
ctest --test-dir build-gpu-policy --output-on-failure
docker build -f docker/wolf.Dockerfile -t wolf:gpu-selection-dev .
# In the matching wolf-ui checkout:
docker build -t wolf-ui:gpu-selection-dev .
```

Never install the tests' fake CUDA/NVML libraries into a runtime environment. The optional
`wolf-gpu-probe` executable should run with real driver libraries and access to the selected device.
The Docker build caches dependencies and compiler outputs, cleans stale object outputs before the
build, and defaults to four compiler jobs (`WOLF_BUILD_JOBS`).

## Deferred: Wolf UI placement policy

Requested controls (spelling supplied by the user): `WOLF_UI_AUTO-UNIFIED_GPU` and
`WOLF_UI_SINGLE_GPU_SELCTION`. Proposed conventional spellings, pending final naming decision:
`WOLF_UI_AUTO_UNIFIED_GPU` and `WOLF_UI_SINGLE_GPU_SELECTION`. These are planned controls,
not implemented environment variables.

- Auto-unified mode prefers a usable integrated GPU for Wolf UI rendering and encoding. Reuse
  startup capability verification and ordinary capacity admission; integrated status alone is not
  evidence of support. Proposed fallback: normal automatic placement when no eligible iGPU exists.
- Explicit mode accepts one GPU identity, preferably a stable PCI identity, for all new Wolf UI
  instances. It takes precedence over auto-unified mode. Codec/capacity checks still apply; an
  unsupported codec or unavailable pinned GPU yields an actionable rejection, not silent migration.
- Apply this policy specifically to the Wolf UI launcher, with an explicit app identity/configuration
  marker rather than relying on its display title. Existing manual overrides need documented precedence.
- Decouple fresh sub-app placement from the launcher: a new game/app uses the ordinary selector,
  even when Wolf UI is fixed to an iGPU. The current sub-app implementation inherits the source GPU;
  this must change before enabling a UI-only placement policy.
- Rejoining a profile's existing sub-app always uses that app's retained GPU and context; the viewer
  encoder follows it after capacity/codec checks. Returning to Wolf UI restores the launcher's GPU.
- Never move running or paused apps to satisfy either control. Policy changes affect new instances.
- Confirm naming, fallback behavior and reliable integrated-GPU classification before implementation.
