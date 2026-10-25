# Native fixtures and regression source

These C++ fixtures exercise production adapters using synthetic data or explicit
local runtime inputs. Host primitive, thread and GPU substitutes are test
backends; they do not prove PICA rendering or physical service behavior.

Use the root [public host runner](../../../../Tools/3ds/test_public_host.py) for
the enumerated data-free checks. It compiles the existing disc, texture, PICA
math, bootstrap and controller fixtures with sanitizers, then runs synthetic
dump verification. [Testing](../../../../docs/TESTING.md) defines the limits.

The other fixtures are retained for source completeness and focused research.
Their corresponding `Tools/3ds/test_*.py` runners may need a private disc, a
particular SDK, public reference checkout or historical baseline not included
in Git. Do not infer that all fixtures form a clean-checkout test suite.

Keep synthetic assets small and generated inside tests. Place local results
under ignored `Build/` or `Diagnostics/`; never add a real game's captured
resource, savedata or memory image as a public fixture.
