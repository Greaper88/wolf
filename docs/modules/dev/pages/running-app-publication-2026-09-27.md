# Running-app status and codec guidance publication: 2026-09-27

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-27 01:36:49 UTC

The model designation is the maintainer-supplied session label.

## Changes

Wolf-UI restores the running indicator and Connect/Stop controls when resuming a
persistent app. It handles both API owner fields, reconciles missed events, rejects
stale snapshots, retains reused app IDs and preserves running state if Stop fails.
The maintainer confirmed the running indicator and controls in the live session.

Wolf now explains when a persistent app's GPU cannot encode the viewer's negotiated
codec. The error names the codec and GPU, lists verified alternatives, and explains
reconnecting with a supported codec or stopping/restarting the app, including the
risk of losing unsaved progress. GPU pinning and encoder admission are unchanged.

Both fixes were committed and pushed to `dev-gpu-selection` and `auto-gpu-selection`
in their respective repositories. Wolf's `stable` and Wolf-UI's upstream-derived
`main` refs were verified unchanged. Existing README image pointers remain correct.

## Published images

These Linux/amd64 images were built locally with the repository Dockerfiles. Each
has OCI source/revision/channel/creation labels and AI agent/model/inference-date
labels. The source commits use `[skip ci]` to avoid duplicate hosted publication;
this release was built, checked and published locally.

| Published tag | Build-input revision | Public manifest digest |
| --- | --- | --- |
| `ghcr.io/greaper88/wolf:gpu-selection-dev` | [`f29fb80`](https://github.com/Greaper88/wolf/commit/f29fb802a606d843bcb289e6623def9ecce5a174) | `sha256:add67a4acfdb5c935e999b59bd759c4176dfb1c9fb40b5b1307d689c7b75eb5b` |
| `ghcr.io/greaper88/wolf:latest` | [`95c31c6`](https://github.com/Greaper88/wolf/commit/95c31c6ca645006280843c3db9ae5286ae61a836) | `sha256:095254c9d45b64b27f86d85c6f36a667c4b2798c0538a4b432a2f6c8d4209011` |
| `ghcr.io/greaper88/wolf-ui:gpu-selection-dev` | [`d93024a`](https://github.com/Greaper88/wolf-ui/commit/d93024aa0e6230831008d769c9b4510db98867b4) | `sha256:9659cf2e3ada41a2ada965fc19d346a2e134d8b097fd82ecfac5fd752dc72585` |
| `ghcr.io/greaper88/wolf-ui:latest` | [`1fb3618`](https://github.com/Greaper88/wolf-ui/commit/1fb3618186b384ac6ba6cff116ee24d6206963d3) | `sha256:9b3a22d7046247c08222ad13ef481c303879f7cdf75eedb3f42663b195de6f69` |

Each image also has a `sha-<full build-input revision>` tag. Anonymous GHCR manifest
requests verified that each channel and corresponding revision tag resolve to the
same expected image digest. The companion UI tags were published before their
matching Wolf channel tags. This record is a subsequent documentation-only commit;
the source revisions above identify the actual build inputs.

## Validation

- Wolf server and Catch2 builds succeeded; 56 assertions in four existing
  lobby/session API cases passed during implementation.
- The .NET 8 UI build succeeded with zero warnings/errors. Fourteen real Godot
  scene regression checks passed against an isolated fake API, including running
  indicators, reconnects, ownership, stale/failed snapshots and Stop behavior.
  The original UI failed the live-create-event regression check.
- Release development Wolf and both UI images have exactly the same filesystem
  layers as the validated local candidates. Both exported UI executables report
  Godot 4.4.1 successfully.
- The freshly generated primary Wolf configuration selects `wolf-ui:latest`; its
  isolated API responded successfully. This check used no host devices, host state,
  Docker socket or published ports, with automatic GPU selection disabled. Initial
  harness attempts used the deployment-specific socket and an unavailable curl
  executable; the check passed using the default socket and included Python runtime.
  Disposable containers and anonymous volumes were removed.
- Source whitespace checks passed in all four worktrees. The release builds include
  the earlier tested code; no new GPU, codec or streaming capabilities are claimed.

## Local runtime and rollback

The maintainer's current session was not restarted during publication. The active
Wolf container remained the already-deployed validated candidate, running with zero
restarts. Local `wolf:gpu-selection-dev` and `wolf-ui:gpu-selection-dev` aliases now
point to their published equivalents with accurate provenance for future launches.
The existing local Compose override remains in place; user configuration was not
modified by this publication.

The pre-fix local images remain under the corresponding repositories' existing
`before-running-app-status-20260927` tags. Previous public channel digests:

- `ghcr.io/greaper88/wolf:gpu-selection-dev` previously resolved to `sha256:1c3c667afe49727eb2c450a9116abdc0c0aaa5f5759e8d89bc92fde705988ec9`.
- `ghcr.io/greaper88/wolf:latest` previously resolved to `sha256:faeb9a962dc21ff617896d03e55722e092d5f9d39a7b39d30eb73d29256dc592`.
- `ghcr.io/greaper88/wolf-ui:gpu-selection-dev` previously resolved to `sha256:0010941ef47d511b7c2823f3ab55ca797216631a8b1f1b20c76fd91985bf194e`.
- `ghcr.io/greaper88/wolf-ui:latest` previously resolved to `sha256:9a3415a94290ab62b156966810339210f88199b0594cb04be2451dba6d2b15c1`.

## Tools and disclosure

Codex execution, patch and UTC clock tools; Git status/diff/add/commit/push/ref
checks; Bash, ripgrep and standard file utilities; Python 3 with JSON, TOML,
subprocess and urllib; Docker/BuildKit build, inspect, run, tag and push; GitHub CLI
for existing registry authentication. Implementation validation used CMake/Ninja,
the C++ compiler, Catch2, clang-format, .NET 8 and Godot 4.4.1 Mono as detailed in
the component implementation records. No subagents or external messaging were used.

GHCR authentication used a private temporary Docker configuration that was removed
after publication. Credentials, session keys and personal profile contents were not
written into this release record or printed during registry authentication.
