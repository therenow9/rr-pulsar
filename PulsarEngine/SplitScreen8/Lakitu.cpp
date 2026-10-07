#ifdef SS8_DEBUG_SCREENS
#include <kamek.hpp>
#include <MarioKartWii/Kart/KartManager.hpp>
#include <MarioKartWii/Lakitu/LakituManager.hpp>
#include <MarioKartWii/Race/RaceData.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// Lakitu for local players at hud slots 4-7 (D37): the countdown and the respawn carrier on their own
// tiles. Lakitu::Manager keeps players[4] with its count right after, so the extras live in a side
// table, and the manager functions that walk players[0..count) are replaced to walk it too. Every
// outside user of the manager goes through these functions.

namespace SplitScreen8 {

static Lakitu::Player *extLakitu[kMaxLocal - kGameLocal];

typedef void *(*NewFn)(u32);
typedef Lakitu::Player *(*PlayerCtorFn)(void *, Kart::Player *);
typedef void (*PlayerHudFn)(Lakitu::Player *, u32);
typedef void (*PlayerFn)(Lakitu::Player *);
typedef u8 (*PlayerIdxFn)(const Lakitu::Player *);
typedef void (*CreatePlayersFn)(Lakitu::Manager *);
typedef bool (*IsLocalFn)(const Kart::Player *);
static const NewFn gameNew = reinterpret_cast<NewFn>(0x80229dcc);
static const PlayerCtorFn playerCtor = reinterpret_cast<PlayerCtorFn>(0x80721514);
static const PlayerHudFn loadGraphics = reinterpret_cast<PlayerHudFn>(0x80722504);
static const PlayerFn createEnableActions = reinterpret_cast<PlayerFn>(0x80721ec0);
static const PlayerFn startRespawnAction = reinterpret_cast<PlayerFn>(0x807223a4);
static const PlayerFn startCountdownAction = reinterpret_cast<PlayerFn>(0x807223bc);
static const PlayerHudFn toggleModelsScreenVisibility = reinterpret_cast<PlayerHudFn>(0x80721090);
static const PlayerHudFn unknown80722418 = reinterpret_cast<PlayerHudFn>(0x80722418);
static const PlayerIdxFn playerIdx = reinterpret_cast<PlayerIdxFn>(0x8072239c);
static const CreatePlayersFn createPlayers = reinterpret_cast<CreatePlayersFn>(0x8071e480);
static const IsLocalFn isLocal = reinterpret_cast<IsLocalFn>(0x80590650);

// Lakitu::Player's virtual Init (vf 0xC), Update (0x10) and deleting dtor (0x8), as the manager calls them.
static void VirtualCall(Lakitu::Player *player, u32 slot, u32 arg) {
    typedef void (*Fn)(Lakitu::Player *, u32);
    reinterpret_cast<Fn>((*reinterpret_cast<void ***>(player))[slot / 4])(player, arg);
}

// The manager's players[0..count), then the side table.
static Lakitu::Player *LakituAt(const Lakitu::Manager &mgr, u32 i) {
    if (i < mgr.localPlayerCount) return mgr.lakituPlayers[i];
    i -= mgr.localPlayerCount;
    if (i >= kMaxLocal - kGameLocal) return nullptr;
    return extLakitu[i];
}
static u32 LakituCount(const Lakitu::Manager &mgr) {
    return mgr.localPlayerCount + kMaxLocal - kGameLocal;
}

// Lakitu::Manager::Init+0x3C calls CreatePlayers, which builds one per local kart's hud slot below the
// scenario's 4 screens. Locals at hud 4+ get theirs here, built as a VS one (widened races are VS):
// kind 2, then CreateEnableActions. Spare CPUs get none, as vanilla's spare 3P screen has none. Init's
// next loop initialises players[4] only, so the extras are initialised here.
static void CreatePlayersWide(Lakitu::Manager *mgr) {
    for (u32 i = 0; i < kMaxLocal - kGameLocal; ++i) extLakitu[i] = nullptr;
    createPlayers(mgr);
    if (raceScreenCount == 0) return;
    for (u32 hud = kGameLocal; hud < raceScreenCount; ++hud) {
        const s32 id = Racedata::sInstance->GetPlayerIdOfLocalPlayer(hud);
        if (id < 0) continue;
        Kart::Player *kart = Kart::Manager::sInstance->GetKartPlayer(id);
        if (!isLocal(kart)) continue;
        void *memory = gameNew(0x218);
        if (memory == nullptr) continue;
        Lakitu::Player *player = playerCtor(memory, kart);
        loadGraphics(player, hud);
        *reinterpret_cast<u32 *>(reinterpret_cast<u8 *>(player) + 0x1d0) = 2;
        createEnableActions(player);
        VirtualCall(player, 0xc, 0);
        extLakitu[hud - kGameLocal] = player;
    }
}
kmCall(0x8071e674, CreatePlayersWide);

// Lakitu::Manager::__dt+0x78 replaces "cmpwi r27, 0" after the loop that deletes players[4] (r27 = the
// manager); the extras are deleted the same way. CR0 is set again here; the prologue saved LR.
static void DeleteExtLakitu() {
    for (u32 i = 0; i < kMaxLocal - kGameLocal; ++i) {
        if (extLakitu[i] != nullptr) VirtualCall(extLakitu[i], 0x8, 1);
        extLakitu[i] = nullptr;
    }
}

asmFunc DeleteExtLakituStub() {
    ASM(
        nofralloc;
        stwu r1, -0x10(r1);
        mflr r0;
        stw r0, 0x14(r1);
        bl DeleteExtLakitu;
        lwz r0, 0x14(r1);
        mtlr r0;
        addi r1, r1, 0x10;
        cmpwi r27, 0;
        blr;)
}
kmCall(0x8071e408, DeleteExtLakituStub);

// The six manager functions that walk players[0..count), replaced whole.
static void Update(Lakitu::Manager *mgr) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i)
        if (Lakitu::Player *player = LakituAt(*mgr, i)) VirtualCall(player, 0x10, 0);
}
kmBranch(0x8071e6c0, Update);

static void DoEnableRespawnAction(Lakitu::Manager *mgr, u8 playerId) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i) {
        Lakitu::Player *player = LakituAt(*mgr, i);
        if (player != nullptr && playerIdx(player) == playerId) {
            startRespawnAction(player);
            return;
        }
    }
}
kmBranch(0x8071e734, DoEnableRespawnAction);

static void ToggleVisible(Lakitu::Manager *mgr, u8 playerId, bool isVisible) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i) {
        Lakitu::Player *player = LakituAt(*mgr, i);
        if (player != nullptr && playerIdx(player) == playerId) {
            toggleModelsScreenVisibility(player, isVisible);
            return;
        }
    }
}
kmBranch(0x8071e7ac, ToggleVisible);

// 0x8071e82c clears the player's PlayerBase byte +0x5C.
static void ClearPlayer5C(Lakitu::Manager *mgr, u8 playerId) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i) {
        Lakitu::Player *player = LakituAt(*mgr, i);
        if (player != nullptr && playerIdx(player) == playerId) {
            *(reinterpret_cast<u8 *>(player) + 0x5c) = 0;
            return;
        }
    }
}
kmBranch(0x8071e82c, ClearPlayer5C);

static void StartCountdownAnm(Lakitu::Manager *mgr) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i)
        if (Lakitu::Player *player = LakituAt(*mgr, i)) startCountdownAction(player);
}
kmBranch(0x8071e8a4, StartCountdownAnm);

static void CallEach80722418(Lakitu::Manager *mgr, u32 arg) {
    for (u32 i = 0; i < LakituCount(*mgr); ++i)
        if (Lakitu::Player *player = LakituAt(*mgr, i)) unknown80722418(player, arg);
}
kmBranch(0x8071e90c, CallEach80722418);

}  // namespace SplitScreen8
#endif
