# Primary and development image channels: 2026-09-25

AI-Agent: ChatGPT Codex
AI-Model-Version: GPT-6 Astra Medium
AI-Inference-Date: 2026-09-25 03:56:34 UTC

The model designation is the maintainer-supplied session label.

## Change

Promote the tested automatic GPU-selection code to `auto-gpu-selection` in both
forks, with matching `ghcr.io/greaper88/wolf:latest` and
`ghcr.io/greaper88/wolf-ui:latest` images. The `dev-gpu-selection` branches and
`gpu-selection-dev` image tags remain the development channel for future NVIDIA
and vGPU work. Wolf's `stable` branch and Wolf-UI's upstream-derived `main` branch
are retained. Existing development checkouts remain on their development branches;
separate worktrees hold the primary branches.

Primary Wolf configuration templates select the companion `wolf-ui:latest` image.
Development templates keep `wolf-ui:gpu-selection-dev`. Existing saved user
configurations retain their selected image until the administrator changes it.
READMEs explain both channels and pulling the UI image separately from Compose.
No GPU selection, encoding or session lifecycle logic changes in this promotion.

Replace inherited upstream publishing jobs with GHCR publishing scoped to the
primary and development branches. Each branch updates only its own channel tag,
plus a full-commit `sha-` tag. Image names are lowercased from the fork repository.
Builds use the Dockerfiles' existing base-image defaults and separate channel
caches. Optional AI commit trailers become image labels. Fork documentation pushes
no longer dispatch publication jobs to the upstream documentation repository.

## Verification and tools

- Official actionlint 1.7.12 archive checksum verified; changed workflows passed
  actionlint, YAML parsing and Bash syntax checks.
- Executed the channel-selection script for both supported branches and rejected
  `stable`, `main` and `dev-manual-gpu-selection`.
- Parsed both primary configuration templates and checked the companion image is
  `ghcr.io/greaper88/wolf-ui:latest`.
- Git whitespace checks passed. Build and registry publication results are recorded
  separately after verification of the final images.

Tools: Codex execution, patch and UTC clock tools; Git branch/worktree/diff/commit
operations; GitHub CLI repository, permission and official action-version reads;
Docker build/tag/push and isolated runtime checks; Python, PyYAML, TOML parsing,
Bash, and temporary actionlint. No subagents were used. Registry credentials and
session keys are excluded from logs and disclosure records.
