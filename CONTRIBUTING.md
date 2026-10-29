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

## Automated code review

CodeRabbit's repository configuration is maintained in `.coderabbit.yaml`.
After the GitHub App is installed for `EstebanPdN/dusklight-3ds`, non-draft pull
requests into the default branch receive automatic incremental reviews.
The five-commit automatic pause is disabled; service plan/rate limits still apply.
Direct pushes to `main` are not pull-request reviews. Existing GitHub Actions
checks remain independent and continue to run on pushes and pull requests.

Review guidance covers ARM/PICA resource ownership, shared JSystem changes,
local Aurora adaptations, offline conversion and evidence limits. Selected
unmodified dependencies and artwork are excluded. CodeRabbit is configured
for feedback, with automatic approvals and code-writing finishing touches off.
Maintainers still decide which suggestions to apply.

First-time activation requires the repository owner to install the
[CodeRabbit GitHub App](https://github.com/apps/coderabbitai/installations/new),
choose **Only select repositories**, and select **dusklight-3ds**. Committing
the YAML alone does not install the app. Verify activation on a real pull
request by its CodeRabbit review/status; if automatic review is absent, use
`@coderabbitai review` in that PR. Keep private game data out of review comments.

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
