# API response stream cleanup: 2026-09-24

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-24 13:21:01 UTC

The model designation is the maintainer-supplied session label.

## Failure and change

A client disconnected while several app images were downloading. The Docker pull
callbacks kept initiating writes to the closed API socket, producing approximately
ten `Error sending data: Bad file descriptor` messages per second. The API write
implementation also referenced temporary response/event strings after their
lifetimes ended and allowed overlapping asynchronous writes on the same socket.

API responses now own their buffers and queue writes on the API's single-threaded
io_context. A failed stream closes once, releases its queued buffers, and ignores
later progress updates while allowing the image pull to finish. SSE broadcasts use
the same queue and marshal subscriber access onto the API thread. Cancelled SSE
timers stop, and unknown routes retain their socket until the 404 is written.

## Verification

- Built `wolf` and `wolftests` in the existing isolated build container.
- Existing pairing, apps, profiles and SSE API cases: 58 assertions in four cases passed.
- Added `tests/api_socket_integration.py`, using a private fake Docker API; it never
  downloads images or connects to the real Docker daemon.
- The current published binary reproduced one broken-pipe error followed by 1,799
  bad-descriptor errors for a disconnected 1,800-record fake pull.
- The local patched image delivered both complete 1,800-record responses in order,
  continued the intervening pull after its client disconnected, delivered all six
  SSE start/end events intact, and returned a subsequent 404 successfully.
- Patched regression logs contained zero bad-descriptor errors. The two disconnected
  clients (pull response and SSE subscription) were each closed once at debug level.
- C++ formatting, Python syntax and `git diff --check` passed.

The candidate image is `wolf:api-socket-fix-candidate`, built by replacing the Wolf
binary in the current development image. It is a local test artifact, not a
published release. These changes do not alter GPU selection or video pipelines.

## Separate live Wolf Den repair

Wolf Den had a stale private `/app/wolf.sock` dated September 20 and no socat
listener. Its root startup probe could reach the real Wolf API, but the .NET app
could not reach its private proxy. After verifying the private socket had no
listener, removed that stale socket and restarted only Wolf Den. The proxy returned
HTTP 200 and the service remained running without automatic restarts. No API socket
permissions, user/group memberships, application data or Compose settings changed.
The stale private socket may require a startup fix in Wolf Den if it recurs after
an unclean exit; no Wolf Den source changes are included here.

## Tools and scope

Codex execution/patch and UTC clock tools; local Git, ripgrep and source inspection;
Docker status, bounded filtered logs, inspect, exec, copy, restart and isolated
build/test containers; CMake/Ninja and existing compiler dependencies; clang-format
21; Python socket/JSON/threading regression checks. No subagents, browser automation
or external research were used. Live session data was reduced to a count, and
credentials/session keys were not included in diagnostics or this record.
