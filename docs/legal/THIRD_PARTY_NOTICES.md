# Third-party notices and license scope

The root [LICENSE.md](../../LICENSE.md) preserves Dusklight's **CC0 1.0 Universal**
dedication. Original port contributions follow that scope. Dependencies and
artwork retain their own terms; no blanket license is assigned to Nintendo
content or to every third-party file in this tree.

| Component | Notice location / scope |
| --- | --- |
| Dusklight and reconstructed engine | [Source/LICENSE.md](../../Source/LICENSE.md); original upstream notices remain in source. |
| Aurora | [MIT license](../../Source/extern/aurora/LICENSE). Local native changes do not remove the upstream notice. |
| Borealis | [Component tree](../../Source/extern/borealis/); preserve its bundled license files and font/resource notices. |
| Cosmetics and randomizer | [Cosmetics](../../Source/mods/cosmetics/) and [randomizer](../../Source/mods/randomizer/); preserve their component notices. |
| OpenDSP | [BSD-2-Clause](../../Source/platforms/3ds/vendor/opendsp/LICENSE), [minimp3 notice](../../Source/platforms/3ds/vendor/opendsp/NOTICE.minimp3), [minimp3 dedication](../../Source/platforms/3ds/vendor/opendsp/LICENSE.minimp3). |
| libctru queue adapter | zlib notice and original source attribution in [ctru_gxqueue.c](../../Source/platforms/3ds/runtime/ctru_gxqueue.c). |
| devkitARM, libctru and Citro3D | Installed build dependencies; retain their SDK notices when distributing compiled products. |
| Kenney voiceover sample | The upstream audio-mod example includes CC0 `go.wav` and `go.opus`; see [its attribution](../../Source/mods/audio_mod/res/attribution.txt). This is upstream demo content, not extracted Nintendo audio. |
| Other engine libraries and resources | Preserve notices under [Source/libs](../../Source/libs/), [Source/res](../../Source/res/) and dependency directories. |
| Project presentation artwork | [Artwork and presentation notes](../images/README.md); separate from the source-code dedication. |

## Embedded open-source firmware

`opendsp.cdc` is an unmodified open-source replacement component from OpenDSP,
not a dump of an owner's Nintendo console firmware. Its expected size is
202,196 bytes and SHA-256 is
`1d4f126106e1715eed63393f9787d4fa796412bc60292a23b31b4b6650ca1ab4`.
CMake verifies that hash before embedding it in the game startup target.
The firmware's redistribution notices must accompany derived distributions.

## Game content and affiliation

Nintendo owns *The Legend of Zelda*, *Twilight Princess* and associated game
content and trademarks. This is an independent fan project. It is not endorsed
by Nintendo or Twilit Realm. Upstream links identify the original projects and
do not imply their endorsement of this adaptation or its development method.

No disc image, extracted Nintendo runtime content, prepared game-data bundle,
save or application-memory capture is distributed here. Preparing compatible
input locally does not make it redistributable. Source-code licenses do not
grant rights to Nintendo's original assets.
