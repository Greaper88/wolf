# Codec mismatch guidance for persistent apps

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-26 00:42:38 UTC

The model designation is the maintainer-supplied session label. The timestamp
records preparation of this change.

## Change

When a viewer tries to enter a persistent app whose GPU cannot provide the
negotiated stream codec, the existing Wolf-UI error dialog now identifies the
codec, GPU name, and short render node. It lists the alternative codecs from the
app's already-bound pipelines, which reflect cached encoder verification and the
producer's fixed buffer mode. No new encoding probes run during this rejection.

The message explains how to disconnect Moonlight, choose an available codec, and
reconnect while keeping the app running. It also explains the alternative of
stopping the app in Wolf-UI and relaunching it on the current launcher GPU, with
an explicit warning about losing unsaved progress. An empty alternative list does
not recommend unverified codecs.

The rejection still occurs before GPU routing or lobby membership changes. It
does not automatically stop the app, disconnect the client, or change codecs.
The source change is applied to both `dev-gpu-selection` and
`auto-gpu-selection`; no Wolf-UI source change is necessary.

## Validation

- Incremental Wolf server and Catch2 executable builds succeeded in the existing
  isolated build container.
- Existing lobby/session API checks passed: 56 assertions in four test cases.
  An initial run lacked `XDG_RUNTIME_DIR` and stalled in the compositor fixture;
  the test process was stopped and the run passed with a private runtime directory.
- clang-format 21.1.8 checked the changed C++ lines; `git diff --check` passed in
  both maintained worktrees.
- Reviewed the existing Wolf-UI error dialog and the rejection's position before
  session mutation. No live Moonlight codec handoff or graphical dialog rendering
  was tested during the initial source change.

## Subsequent local deployment

On 2026-09-27 this change was built and deployed with the Wolf-UI running-app status
fix after the maintainer authorized restarting the dev stack. The local
`wolf:gpu-selection-dev` image is
`sha256:6810e424c2effd665a5fd8d02b2ad82e3a100faf52a1b8962871b036c44e5207`.
The paired local `wolf-ui:gpu-selection-dev` image is
`sha256:e203cd2a75a6693bf4fff5a867061102f7c56b73ad2b7bde5e3724cdb982c3a3`.

The repository Dockerfile build, isolated startup/API check, binary message check,
and deployed API/Moonlight/WolfDen health checks passed. Startup capability results
were unchanged. A new `/root/wolf/docker-compose.override.yml` selects the local
Wolf dev image with `pull_policy: never`; the original Compose file remains
unchanged. The prior images are retained under `before-running-app-status-20260927`
tags in the corresponding local repositories. No GHCR publication or Git push was
performed. See Wolf-UI's `AI_ASSISTANCE_RUNNING_APP_STATUS.md` for UI validation and
the full local deployment record.

## Tools

Codex execution, patch, UTC clock, and workspace dependency tools; Bash, Git,
ripgrep, standard text/file utilities, Docker copy/exec, CMake/Ninja, the cached
C++ compiler, and Catch2. The bundled Python runtime and pip installed
clang-format 21.1.8 into a temporary directory. No subagents were used.
