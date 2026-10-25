# ARM32 bootstrap inventory

This target produces objects and diagnostics only. It is neither a linkable game
nor a 3DS shell. It contains no renderer, data preparation, ISO access or packaging.
The explicit JSON list selects process/list/heap foundations and one framework
integration boundary. It is not a complete initialization dependency closure.

From the private workspace root:

```sh
cmake -S Source/platforms/3ds/compile -B Build/arm32-inventory \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/Source/platforms/3ds/compile/devkitarm.cmake"
python3 Tools/3ds/arm32_inventory.py --label current
```

The runner compiles every translation unit independently, retaining diagnostics
after failures. Its nonzero exit means at least one unit failed. Missing libraries
are reported, not replaced with fake desktop headers. Successful objects do not
prove OS implementations, TLS support, heap budgets or game initialization.
`TARGET_PC=1` is an experimental host ABI selection; the private G2a audit must
be completed before these definitions can be used for a runnable platform.
