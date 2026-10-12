# PulsarEngine/SplitScreen8

The 8-player module: Kamek hooks into PAL Mario Kart Wii, built into Retro Rewind's `Code.pul`. Everything here protects one property: **with 1–4 local players the game is untouched vanilla Retro Rewind**, except four things the owner wants (root `CLAUDE.md`). Same-character select and each player's own skin and model are Retro Rewind's own since upstream `2794b039` (D137, D138), so they are vanilla. The four: same-character select's OK markers offline, each a half of the button with 2 players and a quarter with 3–4 and each in its player's colour (D95, D105; RR's `UI/SameCharacterSelect.cpp`, the upstream PR's file), and at the awards a Mii head only on a Mii when two players share a driver (PD12, RR's `Race/CustomCharacters.cpp`); the main menu's seven Multiplayer buttons (D63, `Entry.cpp`); a race name balloon's portrait read once per player per race and kept on the base character in private rooms, as RR's name is (D130, D139; RR's `UI/CustomCharacters.cpp`); and the menu voices (character, kart and team select) following each player's skin, 1P and online included: a skin with its own voice files says one of its own trick lines, a silent skin nothing, a skin without voice files the base line (D133, D136; RR's `Race/CustomCharacterVoice.cpp`, the upstream fix PR's hook on per-player slots). 1P is otherwise untouched.

Breaking a rule below **fails silently** — it builds, it may even work with 8 players, and it breaks something unrelated. `docs/widening-inventory.md` and `docs/m1-same-character.md` (repo root) have the reasoning.

## Placing a hook

- **Grep `PulsarEngine` for the address and ±0x40 around it before using it.** rr-pulsar patches hundreds of sites; two hooks on one site both apply and the later wins.
- **Read the disassembly past the site** (`tools/mkwdis.py dis <addr> 20`) and list which registers are read before they are written. A `kmCall` clobbers LR (safe only if the function saved it in its prologue; a leaf's entry needs a `kmBranch` back through CTR), plus every register the asm touches.
- **In asm, `addi`, `addis` and a load/store's base read `r0` as the literal 0:** `addi r0, r0, -1` assembles to `li r0, -1`.
- **Use only command forms rr-pulsar already uses** (`kmWrite32`, `kmCall`, `kmBranch`, `kmPatchExitPoint`, `kmRuntimeUse`). WiiCompiled's translator rejects others at setup time.

## Players 5–8

- **Every hook outside those four early-returns for ≤ 4 local players.** A skin slot is `SkinSlot(hud, c)` (`Entry.cpp`): RR's `selectedSlots[4][24]` for huds 0–3, `extSlots` for 4–7. RR's reads by `12 + hud`, by archive holder and by a race player's hud go through it; RR's own per-hud reads (`RequestDriverModel`, the cycle) stay below the game's local count, which is 4 at most. Nothing here passes a hud of 4 or more to RR's 4-wide `customModels`, `selectedSlots` or `LoadDriverBRRES`.
- **Indices 0–3 stay on the game's own storage; 4–7 go to a side table here**, sized by `kMaxLocal`, never a literal.
- **A game slot can be -1.** Check before indexing.
- **Menu input managers have 5 slots and read slot 4 with no mask** (`ManipulatorManager::Update`, `ControlsManipulatorManager::Update`). Anything that makes slot 4 answer a real holder (`Pause.cpp`) must be gone by the next section load, or every menu page sees that pad.
- The `widen-to-eight` skill is the procedure for any 4-wide structure.
