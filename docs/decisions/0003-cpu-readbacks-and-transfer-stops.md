# ADR 0003: Read render targets with the CPU; classify transfer stops

Date: 2026-11-15. Status: adopted. Derived from console evidence; the fix
itself is verified in Azahar only and awaits a console run.

## Context

On a New 3DS every session that entered a gameplay stage stopped on its first
gameplay picture. The sequence was the same in every console log: the minimap
renders into a 216x216 offscreen target, the game copies it as an indexed
texture, and the port read the target back with a tiled-to-linear
`DisplayTransfer`. That transfer completed. The next job of the transfer
engine (a screen-capture or bloom `TextureCopy`) never signalled, and the
command list queued after it stayed silent. Thousands of identical copies had
completed in the title screen, where no target is 216 pixels wide. Azahar
completes every transfer immediately and cannot show this.

The console GPU guard then misread the stop. It blamed the draw that happened
to run alone after the silent transfer (a capture or bloom quad) and left it
out, or switched every later start to running each draw alone, which played
at 3-5 pictures per second.

## Decision

Read-backs need no GPU job. After the GPU has completed, the CPU reads the
finished target straight from VRAM (the RSF maps it read-only), after
`GSPGPU_InvalidateDataCache` on the range. Colour is de-tiled exactly as the
`DisplayTransfer` wrote it; depth is copied as the raw tiled words; depth
peeks read only the words they need. The SD switch `readback-transfer.txt`
restores the transfer engine for comparison and `readback-check.txt` runs both
paths and logs whether they agree.

The GPU guard treats a silent transfer, or a job that stays silent within
15 seconds of a transfer counted complete without its event, as a transfer
stop. It learns nothing about draws. The next start avoids transfer jobs
inside a frame (level 1: GPU-resident captures take the CPU path, bloom is
off); a second stop also leaves offscreen targets alone (level 2: the map
texture is not redrawn). Running every draw alone is a one-shot diagnostic for
an unexplained stop. Learned entries are kept per build. Each hang record
holds the last 32 GSP jobs.

## Alternatives

- Pad offscreen targets to a size the transfer engine is known to handle:
  depends on an undocumented hardware rule and changes the game's EFB mapping.
- Keep the transfer read-back and only recover after a stop: leaves the game
  unable to enter gameplay on the first attempt.

## Consequences

- A minimap update reads about 186 KB from VRAM with the CPU; a depth peek
  reads a few words.
- The remaining transfer jobs in a frame are the capture and bloom copies,
  which completed thousands of times on the console, and the LCD output.
- If the console still stops, the record names the transfer and the next
  start runs without in-frame transfers instead of slowing every draw.

## Verification and reconsideration

Azahar: CPU and transfer-engine read-backs are byte-identical for the
240x400 frame and the 216x216 minimap; the title-to-game route, the
START+SELECT test, both fallback levels and a simulated transfer stop pass.
The console has not run this build yet. Reconsider if a console run records a
transfer stop with this build: the job list in the record then identifies the
remaining transfer.
