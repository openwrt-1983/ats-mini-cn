# AGENTS.md

Arduino firmware for the ATS Mini ESP32-S3/SI4732 receiver.

- Firmware lives in `ats-mini/`; Sphinx documentation in `docs/source/`.
- Read `docs/source/development.md` before making changes.
- For receiver screenshots, follow [the screenshot skill](.agents/skills/ats-mini-screenshot/SKILL.md).
- Follow surrounding C++ style; avoid unrelated reformatting.
- Internal DRAM/heap is scarce; PSRAM is plentiful. Balance memory savings with performance, safety, and simplicity.
- Build firmware changes with `./build.sh` from the repository root. It pins the
  board options (OPI PSRAM, 8 MB flash, custom partition table) and gives the
  bundled SI4732 fork in `ats-mini/libraries/` priority over the official
  library. Calling `arduino-cli` directly is not equivalent: `sketch.yaml` is
  deliberately kept disabled in this repository, so a plain
  `arduino-cli compile ats-mini` would use the default board options (no OPI
  PSRAM, wrong flash size, wrong partition table) and drop the fork.
- Do not compile anything unless asked.
- Do not create new unit tests unless explicitly asked.
- Run repository checks: `uv run prek run --all-files`.
- The FQBN in `build.sh` is the single source of truth for board settings; keep
  it consistent with `ats-mini/partitions.csv` and `ats-mini/build_opt.h`.
- For user-visible firmware changes, edit an existing unreleased Towncrier fragment for the same feature in `changelog/`, or create one if none exists. Skip documentation changes, dependency bumps, and internal changes with no user-visible effect. See `pyproject.toml` for categories.
- Update documentation when user-facing behavior changes. Stay brief, use the existing writing style, avoid describing the implementation details.
- Report validation performed and any behavior needing hardware testing.
