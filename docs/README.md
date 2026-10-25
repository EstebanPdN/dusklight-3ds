# Documentation

The root [README](../README.md) describes the project and its source-only status.
The guides below describe the New3DS adaptation, not a supported consumer release.

## Start here

| Task | Read |
| --- | --- |
| Understand what works and what is pending | [Compatibility](COMPATIBILITY.md), [Verification status](STATUS.md) |
| Build a clean checkout | [Building](BUILDING.md), [Configuration](CONFIGURATION.md) |
| Find the owner of a subsystem | [Repository map](REPOSITORY.md), [Architecture](ARCHITECTURE.md) |
| Prepare your own local runtime inputs | [Game data and controls](GAME_DATA.md) |
| Run tests or investigate a defect | [Testing](TESTING.md), [Diagnostics](DIAGNOSTICS.md) |
| Contribute a change | [Contributing](../CONTRIBUTING.md), [Changelog](../CHANGELOG.md) |
| Check attribution and redistribution scope | [Upstream](UPSTREAM.md), [Third-party notices](legal/THIRD_PARTY_NOTICES.md) |

[Complete native tool index](TOOL_REFERENCE.md) lists the retained development commands.

## Upstream documentation

[Source/docs](../Source/docs/) retains the original Dusklight documentation.
Its desktop installation and build instructions do not describe the 3DS target.
The New3DS entry point is `Source/platforms/3ds/CMakeLists.txt`.

Documentation changes belong in the same commit as the behavior they describe.
Keep status claims dated and distinguish a compilation result from a runtime,
progression or physical hardware result.
