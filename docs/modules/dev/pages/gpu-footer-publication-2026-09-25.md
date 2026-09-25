# GPU footer publication and deployment: 2026-09-25

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-25 03:29:48 UTC

The model designation is the maintainer-supplied session label.

## Published artifacts

Both source changes were pushed to `dev-gpu-selection`. Release images were built
using each repository's Dockerfile, with OCI source/revision/creation labels and
AI agent/model/inference-date labels. Wolf used two compilation jobs.

| Image | Source revision | Versioned tag | Development tag |
| --- | --- | --- | --- |
| `ghcr.io/greaper88/wolf` | `82bac5091ec7839f4666125783993ffb8322ebb9` | `gpu-selection-82bac50` | `gpu-selection-dev` |
| `ghcr.io/greaper88/wolf-ui` | `11218a6d9296484d1075dd5c8c3d38a62295255a` | `gpu-selection-11218a6` | `gpu-selection-dev` |

Published manifest digests:

- Wolf: `sha256:1c3c667afe49727eb2c450a9116abdc0c0aaa5f5759e8d89bc92fde705988ec9`
- Wolf-UI: `sha256:0010941ef47d511b7c2823f3ab55ca797216631a8b1f1b20c76fd91985bf194e`

Anonymous registry requests verified that both the versioned and development tags
resolve to these digests. Source revisions identify the build inputs; this record
is a subsequent documentation-only change.

## Release verification and deployment

The final Wolf image passed disposable two-GPU HEVC streaming, readable names,
GPU usage metadata, persistent-app cross-device handoff, return to the launcher GPU
and app survival after launcher shutdown. The final Wolf-UI executable started
successfully in its runtime image. Earlier unit, API and rendered-footer checks
are documented in [the implementation record](ai-assistance-gpu-footer.md).

The local stack was stopped before deployment. Two exited Wolf-UI containers were
removed without deleting volumes or their bind-mounted home directories. The
existing Compose configuration started Wolf and Wolf Den. Its server tag remains
`ghcr.io/greaper88/wolf:gpu-selection-dev`; its configured local UI tag
`wolf-ui:gpu-selection-dev` now references the matching published UI image.

The running server revision/digest and configured UI revision/digest were verified.
Wolf and embedded PulseAudio were running; the Wolf API, Moonlight serverinfo,
Wolf Den webpage and its private API proxy returned HTTP 200. No sessions were
active during verification. New connections launch the updated UI. The startup
log contained no repeated bad-descriptor write errors.

The previous local images are retained as `wolf:before-footer-20260925` and
`wolf-ui:before-footer-20260925`. The preceding published Wolf version remains
available as `ghcr.io/greaper88/wolf:gpu-selection-785045d`.

## Publication tools

Codex execution, patch and UTC clock tools; Git fetch/commit/push; GitHub CLI for
account verification and existing registry credentials; Docker build/tag/push,
inspect, isolated run/exec/copy, removal of the two stopped UI containers and
Compose startup. Python checked anonymous registry manifests, image metadata,
HTTP/Unix-socket endpoints and release stream behavior. Registry authentication
used a temporary Docker configuration that was removed after publication;
credentials and session keys were not printed or included in this record.
