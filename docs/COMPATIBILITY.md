# Compatibility and development status

Scope: the current New3DS source snapshot, first published in October 2026.
This is an experimental adaptation. No supported binary release is available.

## Platform and data

| Item | Current scope |
| --- | --- |
| Target consoles | New Nintendo 3DS, New Nintendo 3DS XL, New Nintendo 2DS XL |
| Old 3DS / Old 2DS | Outside the current target |
| Presentation target | 400×240 top screen, 2D; diagnostic lower screen |
| Simulation/performance goal | 30 Hz; sustained hardware 30 FPS is unverified |
| Native runtime data | GameCube USA revision 0, `GZ2E01` |
| Other GameCube revisions / Wii discs | Not claimed for this native adapter |
| Desktop upstream | Retained source; not revalidated by this publication |

## Implemented systems and evidence limits

| System | Source contains | Remaining acceptance |
| --- | --- | --- |
| Original engine | Complete gameplay link gate and original startup integration | Beginning-to-end natural progression |
| New native graphics | Independent Citro3D library, prepared resource formats, shaders and partial actual model/camera hooks | No original scene or physical acceptance; complete live inputs, UI/effects and full-game migration remain pending |
| Retained compatibility graphics | GX state/commands, geometry, TEV planning, textures and EFB copies | Historical route evidence belongs to this path, not the new native renderer |
| Input | Buttons, Circle Pad, C-Stick and digital shoulders | Broad physical gameplay calibration |
| Audio | Original software DSP, NDSP PCM and embedded OpenDSP | Whole-game audio behavior and physical audible output |
| Save provider | Native SD card container and staged writes | Reliable story-state save/reload across the full game |
| Diagnostics | Application memory, LCD/VRAM capture and verification tools | Physical completeness, coherence and incident recovery |

Earlier local development exercised title/menu paths and a sequence of scoped
scene-entry routes in Azahar. Direct-stage routes initialize fresh game state;
they are not a completed playthrough or proof that quest progression is loaded.
Those historical observations are not results from the new native renderer
([migration status](NATIVE_RENDERER.md)) and are not promoted to hardware acceptance
by publishing the source.

## Known limitations

- Complete story progression, transitions, all bosses/items, long sessions and
  late-game save/reload have not been accepted end to end.
- Performance remains a major engineering task; do not infer hardware frame
  rate from emulator timing or the 30 Hz simulation target.
- GX/PICA precision and unsupported material cases remain active work.
- Optional heap, background, geometry and texture experiments are not a general
  compatibility guarantee. Report their exact settings.
- HOME, suspend/resume, SD failures and shutdown need physical validation.
- The diagnostic profile does not run the original game.

[Verification status](STATUS.md) records what was checked for the public tree.
[Testing](TESTING.md) defines the evidence needed to strengthen these claims.
