# Contributing

This repository maintains an experimental New3DS adaptation independently from
upstream Dusklight. Begin with [Architecture](docs/ARCHITECTURE.md),
[Repository map](docs/REPOSITORY.md) and [Building](docs/BUILDING.md).

## A focused change

1. Create a branch from `main` and describe one concrete problem or improvement.
2. Preserve the original source layout and use the existing platform guards.
3. Update affected documentation and component provenance with the change.
4. Run the publication checker, relevant synthetic tests and the native build
   when changing target code. Record exactly which checks ran.
5. For runtime changes, state the observed scene/input/behavior and evidence
   level. Identify unavailable data, emulator and hardware checks explicitly.
6. Open a pull request with the problem, resulting behavior, validation and
   remaining limits. Use short, plain English commit subjects.

File-organization commits use `Clean project files`. Avoid unrelated broad
formatting of reconstructed game code. Do not change original savedata or omit
unsupported draws just to turn a test green.

## Tests and reports

Use synthetic fixtures where they can exercise the real production contract.
Keep game data, saves, raw dumps and personal development records out of commits
and issue attachments. The [testing guide](docs/TESTING.md) explains historical
runner prerequisites and the [diagnostic guide](docs/DIAGNOSTICS.md) describes
useful sanitized reports.

## Licensing and attribution

Preserve upstream copyright and license notices. Any new dependency needs an
explicit source pin and a compatible license assessment. Reference-only GPL
projects do not automatically become code dependencies. Identify AI-assisted
changes honestly and review the produced source and actual results.

## Release status

The repository currently publishes source only. A build, pull request or merge
is not authorization to tag or publish a binary. There is no automatic release
workflow in this repository.
