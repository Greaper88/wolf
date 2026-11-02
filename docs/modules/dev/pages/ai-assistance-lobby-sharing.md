# GPU-pinned co-op lobbies and shared video encoding

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-10-04 00:32:05 UTC

The model designation above is the maintainer-provided session label; the exact
serving variant is not independently exposed to the agent. This change was
prepared on October 3, 2026 in America/Chicago. Human review remains pending.

## Behavior

Automatic GPU selection now accepts co-op lobby creation and joining. The app,
compositor, and zero-copy buffer mode stay on their original GPU. Each lobby has
a weak registry of encoding groups. Matching compares the full encoding chain,
render node, resolution, frame rate, bitrate, colors, and slice settings.

Compatible viewers reuse an encoder before any new capacity admission. Different
settings allocate another encoder on the same GPU, using existing-app admission.
A dedicated launcher encoder can hand its capacity to its replacement; the new
group waits for that encoder to retire before starting. Encoders still serving
other viewers are never treated as retiring capacity.

Compressed frames are shared before Moonlight packetization. Each viewer retains
its own packet size, FEC, sequence counters, UDP endpoint, and input devices. Late
joins and recovery request a keyframe with codec headers. Bounded per-viewer
queues discard dependent frames after overflow until a new keyframe arrives.
Raw GPU buffers stay in the existing compositor/conversion path; fanout only
handles encoded samples. Audio remains separately streamed per viewer.

The encoder reservation belongs to the encoding group, independently of the
first viewer's launcher. Each group's final route/stream owner stops and joins
its encoder worker before releasing capacity. An empty lobby has no encoders,
even when its persistent app and compositor remain running. Rejoining creates a
new encoder after admission. Stopping the app stops its producers and returns
connected viewers to their launchers.

Private apps retain their profile ownership and one-viewer restrictions. Lobby
PIN checks remain in the API. Changing private/co-op mode requires stopping the
existing app rather than launching another container against the same state
folder. Pipelines without a safely recognizable split use dedicated encoders.
Unsupported negotiated codecs still return the cached codec guidance.

The companion Wolf-UI change surfaces co-op join failures, retains the running
app after a refusal, and prompts for the PIN when re-entering a protected co-op
app from its owner's tile.

## Validation

- Wolf server and Catch2 executable compiled in the cached isolated builder,
  including the previously reviewed upstream CUDA-context update at `19b40a5`.
- 182 assertions passed in 11 Catch2 cases: new sharing tests plus existing
  lobby/session API and shared-IP regressions.
- A real GStreamer H.264 test encoder exercised late joins, independent UDP
  packetization, a second encoding group, first-viewer departure, last-viewer
  cleanup, and later restart. This test uses x264 and synthetic video in the
  isolated container; production automatic routing still requires verified VA
  hardware encoders.
- Admission and route tests use simulated GPU inventory to cover cross-GPU
  joining, reuse while saturated, capacity refusal for a second encoder, codec
  refusal without disturbing another viewer, and private ownership.
- All six GPU test suites passed, including encoder discovery/probing, cache,
  runtime admission, pipeline lifecycle, and application isolation.
- Companion UI compiled and passed 17 real Godot scene checks using a private
  fake Wolf API. Existing analyzer warnings and headless shutdown resource
  warnings remain.
- clang-format 18.1.8 and whitespace checks passed for changed C++ sources.

The multi-encoder test found an interpipe listener-name collision; instantiated
groups now have unique listener names while matching uses canonical names.
No live AMD/Intel multi-client Moonlight session or controller co-op game was
tested during source validation. At that stage, no running service was restarted,
image published, or Git commit/push performed. Changes are in the primary `auto-gpu-selection`
worktrees; the development worktrees were not modified.

## Local deployment: October 3, 2026

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium (maintainer-provided session label)
AI-Inference-Date: 2026-10-04 00:49:14 UTC

At the maintainer's request, the local stack was brought back up with this code.
The stack was already down before deployment. The tested Wolf binary was packaged
over the previous validated runtime layers; Wolf-UI was rebuilt and exported in
release mode after removing its regression harness, then packaged over its
previous runtime. This was a local candidate build, not a full runtime rebuild or
a registry publication. The source remains uncommitted in the primary worktrees.

| Local image (also tagged `gpu-selection-dev`) | Image ID |
| --- | --- |
| `wolf:lobby-sharing-20261003` | `sha256:5a4e49d1572edcda059a118c95667fe18a06025cb34492ad72815e02c5bd87e3` |
| `wolf-ui:lobby-sharing-20261003` | `sha256:02b5cf0feda1cf6d7845ef301d75f0988680395204072979e5c965c80bd88849` |

The existing Compose override selects `wolf:gpu-selection-dev` with pulling
disabled. The existing app configuration selects `wolf-ui:gpu-selection-dev`.
Neither configuration file needed editing. Previous local image aliases were
preserved as `wolf:before-lobby-sharing-20261003` and
`wolf-ui:before-lobby-sharing-20261003`.

Packaged runtime libraries and external GStreamer sharing elements passed their
checks. The deployed Wolf executable matches the tested binary's SHA-256:
`fe8b300208859a43b795c869fbf2aa6ad7f548039ec4573893f3b2e84724afb5`.
The companion executable reports Godot 4.4.1 Mono. Wolf and embedded PulseAudio
are running; the sessions/lobbies API, Moonlight server-info endpoint, and WolfDen
web page respond successfully. WolfDen was restarted after Wolf became ready to
clear its initial socket connection race. Its socket readiness check produced
one harmless API end-of-file log, with no recurring flood observed.

All four render nodes completed startup verification. The RX 7600 (`renderD129`)
verified H.264, HEVC, and AV1 with zero-copy. Intel (`renderD128`) and both WX 4100s
(`renderD130` and `renderD131`) verified H.264/HEVC with CPU-buffer fallback and no
AV1. No client was connected during the deployment checks; real multi-client
Moonlight/controller acceptance testing remains outstanding.

Additional tools: Docker/BuildKit build, tag, inspect, run, exec, copy, restart,
and stop; Docker Compose; curl; Python HTTP/Unix-socket and XML checks; SHA-256
checks; and Godot release export. No Git push or image push was performed. The
cached builders were stopped afterwards. Build recipes, image metadata, logs,
and rollback details are saved beside the regression results in
`wolf-lobby-sharing-results/2026-10-03`.

## Manual acceptance check

1. Start a co-op app and join it from another profile/device whose launcher is
   on a different GPU. Confirm both viewers route to the app GPU and their
   controllers work independently in the game/emulator.
2. Match encoding settings and check the `Reusing a compatible encoder` log.
   Change one client's supported encoding settings and check for another
   encoder reservation. Verify a codec unsupported by the app GPU is refused
   without interrupting the existing players.
3. Disconnect the first player and confirm the second keeps streaming. Leave
   the lobby with the final player and check `Released unwatched encoder` for
   every encoding group. The persistent app should remain available to resume.
4. Rejoin the empty lobby and confirm encoding restarts on the same GPU. Stop
   the app and confirm all remaining viewers return to their launchers.

## Tools

Codex execution, patch, and UTC clock tools; Bash, Git, ripgrep, Python standard
library, Docker copy/exec, CMake, Ninja, GCC, Catch2, GStreamer 1.26.7, .NET 8,
Godot 4.4.1 Mono, and clang-format 18.1.8. The formatter wheel was downloaded
from PyPI into `/tmp` and its SHA-256 checked against the package metadata.
No subagents were used. Build/test containers had no host device or Docker
socket mounts. Saved regression logs are in the workspace's
`wolf-lobby-sharing-results/2026-10-03` directory.

## First-trial repair and local redeployment

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium (maintainer-provided session label)
AI-Inference-Date: 2026-10-04 04:06:00 UTC

The maintainer reported first-frame-only guest video, stale lobby information,
and failed reconnects. Logs identified a shared-pipeline parse error on the
RX 7600: shorthand frame-rate caps adjacent to the existing DMA-BUF caps produced
`no element "video"`. An explicit capsfilter fixes that parse error. Shared
interpipe sources now use time segments and compensated timestamps; videorate
skips the interval before its first real frame. This makes encoder timing
independent without copying raw GPU pixels.

The UI companion fixes anonymous front-page joins, JSON lobby-event parsing,
periodic lobby reconciliation, and callback cleanup. Server ownership and lobby
PIN checks remain intact. The two empty lobbies observed during diagnosis were
real retained containers with keep-running enabled; the authorized restart
closed them, and the API then reported zero sessions and zero lobbies.

Validation: 188 assertions passed in 12 sharing/session/API cases. The opt-in
hardware case ran separately: 11 assertions passed with simultaneous 1080p HEVC
encoders for a 30 FPS owner and 144 FPS guest on a WX 4100, including sustained
distinct video frames over UDP. The caps regression failed before correction;
an expanded late-viewer case also failed before the timing change, then passed.
The exact Android first-frame symptom was not conclusively reproduced, so a
real two-client retry remains necessary. Wolf-UI passed 26 real-scene checks.
Source hashes, formatting, release export, runtime libraries, and executable
startup checked successfully.

Both repaired images also have the local `gpu-selection-dev` alias:

- `wolf:lobby-sharing-repair-20261003`: `sha256:aa14e3c7b5a32157f55281154e85b22b9eb62f714d8bd37715133c1c35f578e1`
- `wolf-ui:lobby-sharing-repair-20261003`: `sha256:5b628fe6eae5af3e1738e16ce9a7754f6fbcb206cfa8e238276f9e415e7f9632`

The previous images are retained under each repository's
`before-lobby-sharing-repair-20261003` tag. The deployed Wolf binary SHA-256 is
`ce8b20228c59985941cbaee6c052b000708e81589dde039f991760a9e2565642`.
Wolf was recreated with a 45-second shutdown allowance. All four GPUs completed
startup verification; Wolf, PulseAudio, Moonlight server info, and WolfDen
responded. Existing configuration and the separately updated WolfDen image were
preserved. No Git commit/push or registry publication was performed.

Tools: Codex execution, patch and clock tools; Git, Bash, ripgrep, Python, curl,
Docker/BuildKit and Compose; CMake, Ninja, GCC, Catch2, GStreamer, .NET, Godot,
clang-format, and SHA-256 checks. Primary
[interpipe source](https://github.com/RidgeRun/gst-interpipe/tree/master/gst/interpipe)
was read using web/HTTP tools during the timestamp review. Hardware tests had
render devices but no live app data or Docker socket. Cached builders were
stopped afterwards. Evidence and build recipes are in
`wolf-lobby-sharing-results/2026-10-03/repair`.

## Android high-frame-rate follow-up: October 4, 2026

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium (maintainer-provided session label)
AI-Inference-Date: 2026-10-04 05:16:29 UTC

The maintainer confirmed that the native Android guest's shared video works after
reducing the requested frame rate from 144 to 60 FPS. His private RX 7600 Plasma
stream had displayed normally; the shared app uses the WX 4100. This establishes
a working setting for this combination, but does not distinguish server encoder
capacity from Android decoder/display behavior at 144 FPS. No production code,
frame-rate cap, service restart, image build, or publication was needed during
this investigation. An unrelated disappearing emulator menu was traced by the
users to the emulator's rendering and excluded from this investigation.

The opt-in VA regression now reconstructs transmitted data shards and feeds the
HEVC frames into a persistent hardware decoder. It covers shared-to-shared and
dedicated RX 7600-to-shared WX 4100 encoder transitions. The hardware run passed
953 assertions across both sections; focused software/session regressions passed
188 assertions in 12 cases, with the hardware case skipped there. These checks
verify continued decoding, not sustained 144 FPS performance or Android behavior.
Set `WOLF_TEST_SHARED_VA_NODE` to the app/decoder render node and optionally
`WOLF_TEST_SHARED_VA_HOME_NODE` to another verified VA render node to exercise
the dedicated cross-GPU handoff.

Additional tools: Python AF_PACKET counters restricted to outgoing Wolf video
headers (no screen capture), Docker isolated test containers, CMake/Ninja, Catch2,
GStreamer VA encoding/decoding, clang-format, FFmpeg/ffprobe on synthetic clips,
read-only Git/rg checks, and official Moonlight Android decoder source reviewed
through web search and curl. Results are in
`wolf-lobby-sharing-results/2026-10-04/freeze-investigation` beside the repos.
