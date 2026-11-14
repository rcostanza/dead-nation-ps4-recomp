# Changelog

Each release's section becomes the text of its GitHub release (`tools/release_notes.py`, run by
`.github/workflows/build.yml` when a `vN.N` tag such as `v0.1` is pushed). Add the section before
tagging: a tag without one does not publish. Keep each entry on one line: GitHub shows a line break
in release text as a break.

## v0.2

- Retuned game speed logic according to the FPS cap; rate of fire should now be consistent regardless
- Quitting to the main menu no longer drops the game to 30 FPS in slow motion

## v0.1

- First release
