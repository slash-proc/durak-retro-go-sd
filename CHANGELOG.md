# Changelog

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v1.0.0`).

When you cut a release:

1. Move items from `[Unreleased]` into a new `## [vX.Y.Z] - YYYY-MM-DD` section.
2. Commit the changelog update.
3. Push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`

CI reads the matching section and uses it as the GitHub Release notes. The tag
is also used in staged asset names (`<binary>-<tag>.bin`, `<binary>-<tag>.zip`).

## [Unreleased]

### Added

- The project adopts the [GWRG distribution model](https://github.com/slash-proc/gwrg-dist-spec).
  A tagged release now carries a `manifest.json` describing what it installs
  and where, an offline bundle holding every file that manifest names, and a
  GitHub Pages mirror a web installer can fetch across origins.
- The shared dist scripts, taken verbatim from the canonical set:
  `make_manifest.py`, `build_dist.py`, `make_bundle.py` and `stage_release.py`.
  They read every project-specific value out of the Makefile, so they stay
  byte-identical across projects and a fix lands everywhere at once.
- `SIDECARS`, which is the shared scripts' name for what this project already
  called `RELEASE_EXTRAS`: the 5.6 MiB `Duren.pak` the game opens from its own
  folder. `RELEASE_EXTRAS` stays the source of truth so merging from upstream
  needs no edit here. The manifest declares the pack as an artifact in its own
  right, with its own hash, and the install zip puts both files under
  `/homebrews/` exactly as the README requires.
- `COVER_FULL`, so the unscaled `src/assets/cover_src.png` is published beside
  the release next to the 128x96 copy packed into the binary.

## [v0.0.1]

Initial version of Duren / Durak homebrew.

### Added

- Nothing

### Changed

- Nothing

### Fixed

- Nothing

### Install

- Unzip the release archive onto the SD card root (`homebrews/Duren.bin` + `homebrews/Duren.pak`).
- Requires firmware whose ABI matches `SDK_VERSION` in this repository.
