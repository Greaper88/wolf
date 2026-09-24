# Automatic GPU zero-copy integration

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 07:58:27 UTC

The model designation is the maintainer-supplied session label. The timestamp records
preparation of this disclosure. AI assistance implemented the changes, tests and
documentation and performed local build and hardware validation.

## Behavior

Automatic VA sessions now prefer the selected GPU's verified zero-copy path. An
isolated startup probe tests eight compositor frames through linear XR24 DMA-BUF,
a device-bound VA postprocessor, NV12 VA surfaces and the selected hardware encoder.
Read-only device properties establish the converter/encoder identity. The probe
checks actual DMA memory and a VA surface on that original memory after import,
rejecting the VA plugin's hidden CPU-copy fallback. The optional VA inspection
library is loaded only for the probe; missing inspection support fails closed.

Basic hardware encoding and zero-copy run in separate child processes, each with
its own timeout. A failed/hung zero-copy check therefore preserves an independently
verified CPU-buffer hardware encoder. Results are cached; app launch/reconnect does
not run another encode trial. Private probe Wayland sockets avoid live sessions.

`WOLF_USE_ZERO_COPY=true` remains the default. `WOLF_GPU_REQUIRE_ZERO_COPY=true`
excludes GPUs without verified H.264 zero-copy and excludes unverified codecs.
The strict setting defaults to false, and conflicts with explicitly disabling
zero-copy are rejected. Explicit manual GPU pins retain their legacy behavior.

The producer's buffer mode follows its retained GPU assignment. Both launchers and
profile sub-apps use it; disconnect, cross-device join, and return to launcher
preserve their respective producer modes. Zero-copy producers advertise only codecs
that passed that path. Logs identify available startup paths and the actual path
chosen for each producer. The diagnostic `wolf-gpu-probe` reports both paths.

Zero-copy here avoids CPU readback/upload of raw frames; GPU RGB-to-NV12 conversion
still occurs. The verified path is 8-bit linear XR24, not arbitrary modifiers or HDR.
Startup probes do not promise maximum resolution, frame rate or concurrent capacity.
This work adds no NVIDIA routing or multi-viewer lobby support and makes no measured
WAN latency improvement claim.

## Validation

- The complete server and Catch2 test binary built in the Docker toolchain.
- All six standalone GPU suites passed, covering buffer-mode retention, strict
  exclusion, explicit disable, converter binding, isolated probe failure/timeout,
  encoder capability checks, and existing admission/isolation behavior.
- RTSP/control/serialization regressions passed: 198 assertions in nine test cases.
- RX7600 (`renderD129`) passed the DMA-memory/VA-import check and H.264, HEVC and
  AV1 encoding. WX4100 (`renderD130`) passed CPU-buffer H.264/HEVC but did not expose
  the required VA DMA-BUF input path. Its AV1 encoder and the Intel device's hardware
  codec tests remained unavailable. These are results for this driver stack, not a
  claim about every possible driver configuration for these cards.
- An isolated two-GPU server produced HEVC RTP frames on both cards. Its persistent
  sub-app used RX7600 DMA-BUF, accepted a viewer arriving from the WX4100 launcher,
  retained increasing frame IDs, restored the launcher's CPU-buffer path on leave,
  and survived the original launcher's shutdown. Profile mismatch and a concurrent
  second viewer remained rejected.
- The production `docker/wolf.Dockerfile` image build succeeded. Existing dependency
  deprecation and exception-handler warnings remain; no warning-free build is claimed.
- Three fresh production-image fixtures passed: mixed DMA-BUF/CPU-buffer app routing,
  strict zero-copy admission with real streaming and WX4100 exclusion after probes
  finished, and explicit zero-copy disable with both GPUs and persistent app routing.
  Producer logs matched each mode, with no `not-negotiated` errors.

The fixture uses no host Docker socket, external network, live configuration, or real
paired-client data. Its missing-encoder-template and API-socket-directory setup issues
were corrected before successful streaming checks. These were fixture errors, not
changes to the production runtime.

## Tools and references

- Codex execution and patch tools, Bash, Git, ripgrep, standard file utilities and UTC
  clock; no sub-agents were used.
- Docker/BuildKit, CMake, Ninja, GCC, ccache, GStreamer inspection/test pipelines,
  Catch2, CTest, Python standard-library HTTP/socket/RTP fixtures, and clang-format 21.
- Official GStreamer documentation and pinned 1.26.7 source inspected using web tools
  and curl, including [DMA-BUF negotiation](https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html),
  [VA postprocessing](https://gstreamer.freedesktop.org/documentation/va/vapostproc.html),
  and [the VA import/copy implementation](https://github.com/GStreamer/gstreamer/blob/1.26.7/subprojects/gst-plugins-bad/sys/va/gstvabase.c).

Publication and deployment details are recorded separately with the final image digest.
