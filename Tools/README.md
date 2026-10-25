# Project tools

The maintained public entry points live in [3ds](3ds/README.md). All tools run
locally. Source checks and synthetic tests need no original game data. Runtime
data preparation and emulator investigation need the developer's own inputs.

`fetch-public-git-checkouts.sh` is an optional historical research helper. It
fetches reference-only projects under ignored `Upstream/`; it is not required
to build this public snapshot and does not change the vendored dependencies.
