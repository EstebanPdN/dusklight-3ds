# Native build configuration

The source of truth is [CMakeLists.txt](../Source/platforms/3ds/CMakeLists.txt).
The public helper selects explicit profiles rather than relying on an old
CMake cache. Use separate build directories for different configurations.

## Public helper

```sh
python3 Tools/3ds/build_public.py --profile diagnostic
python3 Tools/3ds/build_public.py --profile game --jobs 4
python3 Tools/3ds/build_public.py --profile game --configure-only
```

`DEVKITPRO` defaults to `/opt/devkitpro`; `DEVKITARM` defaults to its `devkitARM`
child. Arguments `--devkitpro` and `--devkitarm` override those environment
values. `--build-dir` is relative to the current directory when not absolute.
The default output is `Build/public-PROFILE`. Extra `--cmake-option=-D...` values
are passed last and override an individual profile setting.

## Important CMake options

| Option | Native CMake default | Meaning |
| --- | --- | --- |
| `DUSK_SCENE_TEXTURES` | ON | Render original material commands in the scene path. |
| `DUSK_SCENE_STARTUP` | OFF | Include the original scoped scene owners. |
| `DUSK_SCENE_ENVIRONMENT_STARTUP` | OFF | Link original scene/environment owners. |
| `DUSK_GAMEPLAY_LINK_GATE` | OFF | Require original player, camera, play-scene and process roots. |
| `DUSK_GAME_STARTUP` | OFF | Run original machine/process startup and the game loop. |
| `DUSK_COMMAND_PROBE_STARTUP` | OFF | Start the synthetic command/GPU diagnostic. |
| `DUSK_SCENE_FOG_INSPECTION` | OFF | Inspection path with fog omitted; changes the rendered workload. |
| `DUSK_SCENE_MATERIAL_INSPECTION` | OFF | Inspection path omitting unsupported materials; not normal gameplay. |
| `DUSK_BG_BUDGET_EXPERIMENT_76` | OFF | Experimental bounded background-instance admission. |
| `DUSK_SCENE_CREATE_PROBE` | OFF | Instrument scene creation. |
| `DUSK_GROUND_CREATE_PROBE` | OFF | Instrument ground creation. |
| `DUSK_PROFILE_LINK_SELECTION` | empty | A bounded developer profile list; incompatible with full game startup. |
| `DUSK_DIAGNOSTIC_EXIT_FRAMES` | 0 | Optional diagnostic exit bound. |
| `DUSK_DUMP_BUILD_ID` | unsealed integration label | Fingerprint embedded in diagnostic metadata for game startup. |
| `ICON` | repository `assets/3ds/icon-48.png` | SMDH icon input. |

The `game` helper profile enables scene startup, environment startup, the complete
gameplay link gate and game startup. `diagnostic` explicitly disables those four
paths. Both profiles disable CMake inspection/creation/background experiments
unless overridden. Neither profile creates a tag, release or network upload.

Some runtime experiments are controlled by local SD configuration rather than
CMake. Do not assume that selecting the game build enables every experimental
conversion or heap partition. The implementation and historical research tools
remain the detailed reference for those developer-only controls. They are not
a supported end-user settings interface.

The full startup gate refuses a partial original-profile selection. This prevents
a diagnostic link configuration from masquerading as complete engine linkage.

## Native renderer migration

`DUSK_PICA_GAME_DRAW` defaults ON; it takes effect only with
`DUSK_GAME_STARTUP=ON`. It links `dusk_pica_engine` and selects the actual native
model-packet entrypoints across the original game build groups. Missing native
resources or producer inputs fail explicitly. UI/effects migration is incomplete.

Setting it OFF selects the retained compatibility route for development and is
not progress toward native-renderer acceptance. The public build helper does not
set it OFF. This option is a CMake build choice, not an SD runtime switch.
See [Native renderer](NATIVE_RENDERER.md) for the temporary data contract and
remaining work.
