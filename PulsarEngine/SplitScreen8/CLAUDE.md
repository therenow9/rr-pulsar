# PulsarEngine/SplitScreen8

The 8-player module: Kamek hooks into PAL Mario Kart Wii, built into Retro Rewind's `Code.pul`. Everything here protects one property: **with 1–4 local players the game is untouched vanilla Retro Rewind**, except three things the owner wants (root `CLAUDE.md`): same-character select in every multiplayer count (`SameCharacter.cpp`), the main menu's seven Multiplayer buttons (D63, `Entry.cpp`), and each player's own custom-character skin, voices included, in every offline multiplayer game (D72, D75). That one is RR code bound upstream, in RR's own `Driver/LocalPlayerSkins.cpp` on RR's slots (D83), and its voices are RR's own per-player aliasing (D85). 1P is untouched.

Breaking a rule below **fails silently** — it builds, it may even work with 8 players, and it breaks something unrelated. `docs/widening-inventory.md` and `docs/m1-same-character.md` (repo root) have the reasoning.

## Placing a hook

- **Grep `PulsarEngine` for the address and ±0x40 around it before using it.** rr-pulsar patches hundreds of sites; two hooks on one site both apply and the later wins.
- **Read the disassembly past the site** (`tools/mkwdis.py dis <addr> 20`) and list which registers are read before they are written. A `kmCall` clobbers LR (safe only if the function saved it in its prologue; a leaf's entry needs a `kmBranch` back through CTR), plus every register the asm touches.
- **In asm, `addi`, `addis` and a load/store's base read `r0` as the literal 0:** `addi r0, r0, -1` assembles to `li r0, -1`.
- **Use only command forms rr-pulsar already uses** (`kmWrite32`, `kmCall`, `kmBranch`, `kmPatchExitPoint`, `kmRuntimeUse`). WiiCompiled's translator rejects others at setup time.

## Players 5–8

- **Every hook outside those three early-returns for ≤ 4 local players.** RR's `LocalPlayerSkins.cpp` acts only in offline multiplayer (section 0x54, two or more players) and answers RR's own `selectedSlots` for player 1 and online rooms.
- **Indices 0–3 stay on the game's own storage; 4–7 go to a side table here**, sized by `kMaxLocal`, never a literal.
- **A game slot can be -1.** Check before indexing.
- **Menu input managers have 5 slots and read slot 4 with no mask** (`ManipulatorManager::Update`, `ControlsManipulatorManager::Update`). Anything that makes slot 4 answer a real holder (`Pause.cpp`) must be gone by the next section load, or every menu page sees that pad.
- The `widen-to-eight` skill is the procedure for any 4-wide structure.
