# PulsarEngine/SplitScreen8

The 8-player module: Kamek hooks into PAL Mario Kart Wii, built into Retro Rewind's `Code.pul`. Everything here protects one property: **with 1–4 local players the game is untouched vanilla Retro Rewind**, except same-character select (`SameCharacter.cpp`), which the owner wants in every multiplayer count.

Breaking a rule below **fails silently** — it builds, it may even work with 8 players, and it breaks something unrelated. `docs/widening-inventory.md` and `docs/m1-same-character.md` (repo root) have the reasoning.

## Placing a hook

- **Grep `PulsarEngine` for the address and ±0x40 around it before using it.** rr-pulsar patches hundreds of sites; two hooks on one site both apply and the later wins.
- **Read the disassembly past the site** (`tools/mkwdis.py dis <addr> 20`) and list which registers are read before they are written. A `kmCall` clobbers LR (safe only if the function saved it in its prologue), plus every register the asm touches.
- **Use only command forms rr-pulsar already uses** (`kmWrite32`, `kmCall`, `kmBranch`, `kmPatchExitPoint`, `kmRuntimeUse`). WiiCompiled's translator rejects others at setup time.

## Players 5–8

- **Every hook outside `SameCharacter.cpp` early-returns for ≤ 4 local players.**
- **Indices 0–3 stay on the game's own storage; 4–7 go to a side table here**, sized by `kMaxLocal`, never a literal.
- **A game slot can be -1.** Check before indexing.
- The `widen-to-eight` skill is the procedure for any 4-wide structure.
