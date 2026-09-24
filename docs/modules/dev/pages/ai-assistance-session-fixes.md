# AI assistance disclosure: connection-fix integration

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 04:34:14 UTC

The timestamp records preparation of this integration disclosure. The model designation
is the session label supplied by the maintainer.

## Contribution and provenance

AI assistance was used to locate commits, review their interaction with automatic GPU
selection, cherry-pick them onto `dev-gpu-selection`, validate the combined source, and
update documentation. Original commit authorship and source hashes are preserved with
`git cherry-pick -x`. The upstream contribution is not attributed to AI assistance.
The integration also updates the RTSP test fixture to provide a nonempty H.264 pipeline:
the automatic-selection branch rejects an empty pipeline as an unsupported codec.
This fixture emits stream events without running a real encoder.

- Upstream `40858c3cea0231bf6b918b6d557e7427348331ec`: avoid a second HTTP response
  after successful resume. This branch already returned after success; the cherry-pick
  also confines the missing-session error to its failure branch.
- Fork `72ca30d1439e380c959ce2f8637b7abed1a3f61c`: prefer session identifiers for
  control/RTSP routing, reject ambiguous IP fallback, and preserve asynchronous RTSP
  response/connection lifetimes; includes shared-IP regression tests.
- Fork `8448999f0b10bd2fca33852d29d3ec96aeb2e6d4` and
  `70ed710aec9e138339a04f880fdcd6bce6216cf0`: shared-IP limitations and requirements.

## Tools used

- Codex execution tools (`functions.exec`, `exec_command`, `write_stdin`), Bash,
  ripgrep, and standard text utilities for repository inspection and build-log review.
- Git for fetching source histories, comparing commits, switching branches,
  cherry-picking with provenance, reviewing diffs, and committing documentation.
- Codex `apply_patch` for documentation and test-fixture edits, and the UTC clock tool
  for this timestamp.
- clang-format 21 in dry-run/error mode for all six changed C++ source/header files.
- Docker for an isolated builder container with a snapshot of the committed source,
  without host devices, the Docker socket, or user application state.
- CMake and Ninja for the server/test build; Catch2 and CTest for regression checks.

## Validation and limits

- The Wolf server and Catch2 test executable built successfully using
  `wolf:shared-ip-builder`, with hardware-dependent test options disabled.
- Catch2 `[shared-ip]`: 23 assertions passed across two test cases.
- The broader filter `[RTSP],*Control*,control joypad input packets,[serialization]`
  passed 198 assertions across nine test cases after correcting the stale H.264 fixture.
  Before that correction, four ANNOUNCE assertions correctly received 400 instead of
  the fixture's expected 200; no production-code change was needed.
- All six standalone GPU suites passed, with `TEST_GPU_ENCODER_PROBE=ON`.
- clang-format dry-run checks passed for every changed C++ file; `git diff --check`
  passed for the documentation update.

This integration does not make identifier-less legacy clients fully compatible with
multiple sessions sharing one public IP. Such ambiguous connections are rejected.
No live Moonlight reconnect, GPU hardware, image publication, or deployment test is
claimed by this integration record. GPU ownership and reconnect admission remain
the automatic-selection branch's existing implementation.
