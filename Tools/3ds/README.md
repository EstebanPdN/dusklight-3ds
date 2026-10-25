# Native development tools

## Maintained public commands

| Command | Purpose |
| --- | --- |
| `python3 Tools/3ds/check_publication.py` | Check tracked content, public guide links and firmware hash. |
| `python3 Tools/3ds/test_public_host.py` | Run enumerated synthetic host checks with sanitizers. |
| `python3 Tools/3ds/build_public.py --profile game` | Configure and build original native game integration locally. |
| `python3 Tools/3ds/build_public.py --profile diagnostic` | Build the synthetic native diagnostic bootstrap. |
| `python3 Tools/3ds/twilight_pack.py --help` | Inspect local, user-supplied game-data preparation options. |
| `python3 Tools/3ds/verify_application_dump.py --help` | Inspect local application-dump validation options. |

Run from the repository root. [Building](../../docs/BUILDING.md),
[Testing](../../docs/TESTING.md) and [Game data](../../docs/GAME_DATA.md) describe
prerequisites and output locations.

## Existing research tools

The complete current tool source is retained, including earlier scoped tests,
packaging helpers, inventory probes and emulator analysis. These tools have
different prerequisites. Some require a private disc or historical baseline;
some use the older `/opt/devkitpro` SDK path. They are not all run by public CI.
See the [complete tool index](../../docs/TOOL_REFERENCE.md) before choosing one.

Keep generated outputs under ignored `Build/` or `Diagnostics/`. A tool that
prepares data locally does not authorize uploading its output. Do not run the
old checkout-completion helper against the vendored public source snapshot.
