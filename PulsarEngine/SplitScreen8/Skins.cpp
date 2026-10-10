#include <kamek.hpp>
#include <CustomCharacters/CustomCharacters.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// D72: each local player keeps their own custom-character skin in an offline multiplayer game, and a
// player on a driver another player already shows gets their own preview model, so poses and skins
// stop being shared (PD5). docs/plans/m4-menu-flow.md, phase C, has the design.

// Retro Rewind's CustomCharacters functions this file calls. They are not in its header; a changed
// signature fails the link.
namespace Pulsar {
namespace CustomCharacters {
bool LoadReloadedMenuDriverModel(GameScene &scene, ScnMgr &scnMgr, CharacterId character, ModelDirector *&newModel,
                                 ToadetteHair *&newHair, EGG::ExpHeap *&newHeap);
bool LoadDefaultReloadedMenuDriverModel(GameScene &scene, ScnMgr &scnMgr, CharacterId character, ModelDirector *&newModel,
                                        ToadetteHair *&newHair, EGG::ExpHeap *&newHeap);
void ResetReloadedMenuDriverModel(MenuDriverModel &menuModel, CharacterId character);
bool ReloadMenuDriverModel(MenuDriverModelMgr &driverMgr, CharacterId character);
void PoseReloadedMenuDriverModel(CharacterId character);
void UnlockMenuModelHeaps(MenuModelMgr &modelMgr);
void DestroyModelDirector(ModelDirector *model);
void DestroyHeap(EGG::ExpHeap *&heap);
}  // namespace CustomCharacters
}  // namespace Pulsar

namespace SplitScreen8 {

namespace CC = Pulsar::CustomCharacters;

const u32 kDrivers = CC::MENU_DRIVER_MODEL_COUNT;
const u32 kModelSize = 0x28;

u8 menuLoadHud;

// Players 2..kMaxLocal's tables; player 1 keeps RR's own (selectedTable), shared with 1P and ghosts.
static u8 playerTables[kMaxLocal - 1][CC::CHARACTER_COUNT];

static bool OnlineRoom() {
    return CC::IsOnlineRoom(RKNet::Controller::sInstance);
}

u8 PlayerSkinTable(u8 hud, CharacterId character) {
    if (hud == 0 || hud >= kMaxLocal || OnlineRoom()) return CC::SelectedTable(character);
    const CharacterId state = CC::StateCharacter(character);
    if (!CC::IsCharacter(state)) return CC::TABLE_DEFAULT;
    const u8 table = playerTables[hud - 1][state];
    return CC::HasSkin(character, table) ? table : CC::TABLE_DEFAULT;
}

bool SetPlayerSkinTable(u8 hud, CharacterId character, u8 table) {
    if (hud == 0 || hud >= kMaxLocal || OnlineRoom()) return CC::SetSelectedTable(character, table);
    const CharacterId state = CC::StateCharacter(character);
    if (!CC::IsCharacter(state) || !CC::HasSkin(character, table)) return false;
    u8 &slot = playerTables[hud - 1][state];
    if (slot == table) return false;
    slot = table;
    return true;
}

void ResetPlayerSkinTables() {
    memset(playerTables, CC::TABLE_DEFAULT, sizeof(playerTables));
}

u8 LocalRaceSkinTable(u8 playerId, CharacterId character) {
    const Racedata *racedata = Racedata::sInstance;
    if (racedata == nullptr) return CC::TABLE_DEFAULT;
    const u32 count = racedata->racesScenario.localPlayerCount;
    for (u8 hud = 0; hud < count && hud < kGameLocal; ++hud) {
        if (racedata->GetPlayerIdOfLocalPlayer(hud) == playerId) return PlayerSkinTable(hud, character);
    }
    return CC::TABLE_DEFAULT;
}

// A hud's own preview model. MenuDriverModel's ctor is not in Kamek's externals, so the object is
// raw storage given models[0]'s vtable word; its +0x18 (character) is set by RR's reset.
struct PrivateModel {
    u32 storage[kModelSize / 4];
    EGG::ExpHeap *heap;
    ToadetteHair *hair;
    u8 table;
};
static PrivateModel privates[kGameLocal];
static MenuDriverModelMgr *owner;
static const GameScene *ownerScene;
// The skin each shared models[c] was built with, and the huds placed since the manager was built:
// its ctor parks every hud on Mario with a direct SetPlayerCharacter.
static u8 sharedTables[kDrivers];
static u8 placed;

static MenuDriverModel *Own(PrivateModel &p) {
    return reinterpret_cast<MenuDriverModel *>(p.storage);
}

static MenuDriverModel *Shared(MenuDriverModelMgr &mgr, u32 character) {
    return reinterpret_cast<MenuDriverModel *>(reinterpret_cast<u8 *>(mgr.models) + character * kModelSize);
}

static u32 ModelState(const MenuDriverModel *model) {
    return reinterpret_cast<const u32 *>(model)[0x8 / 4];
}

static u32 ModelCharacter(const MenuDriverModel *model) {
    return reinterpret_cast<const u32 *>(model)[0x18 / 4];
}

static u8 WantedTable(u8 hud, u32 character) {
    return PlayerSkinTable(hud, CC::MenuBRRESCharacter(static_cast<CharacterId>(character)));
}

static void Report(u8 hud, u32 character, u8 table, const char *how) {
#ifdef SS8_MENU_SCRIPT
    OS::Report("ss8 skin: hud %u character %#x table %u %s\n", hud, character, table, how);
#endif
}

static void DropPrivate(PrivateModel &p, bool destroy) {
    MenuDriverModel *model = Own(p);
    if (destroy && model->model != nullptr) {
        CC::DestroyModelDirector(p.hair);
        CC::DestroyModelDirector(model->model);
        CC::DestroyHeap(p.heap);
    }
    memset(&p, 0, sizeof(p));
}

static void DropAll(bool destroy) {
    for (u32 i = 0; i < kGameLocal; ++i) DropPrivate(privates[i], destroy);
    owner = nullptr;
    ownerScene = nullptr;
}

void DestroyPlayerMenuModels() {
    DropAll(owner != nullptr && ownerScene == GameScene::GetCurrent());
}

// Offline local multiplayer only (section 0x54): 1P, online and every other section keep RR's models.
static bool PerPlayerModels(const MenuDriverModelMgr *mgr) {
    const SectionMgr *sectionMgr = SectionMgr::sInstance;
    return mgr != nullptr && mgr->playerCount > 1 && sectionMgr != nullptr && sectionMgr->curSection != nullptr &&
           sectionMgr->curSection->sectionId == SECTION_LOCAL_MULTIPLAYER;
}

// A manager built since the last call drops the old privates (their heaps die with their scene) and
// records what its shared models were built with, before any skin can change.
static void Sync(MenuDriverModelMgr *mgr) {
    if (owner == mgr) return;
    if (owner != nullptr) DestroyPlayerMenuModels();
    if (!PerPlayerModels(mgr)) return;
    owner = mgr;
    ownerScene = GameScene::GetCurrent();
    placed = 0;
    for (u32 c = 0; c < kDrivers; ++c) sharedTables[c] = WantedTable(0, c);
#ifdef SS8_MENU_SCRIPT
    OS::Report("ss8 skin: adopt players %u +0x50 %08x\n", mgr->playerCount, mgr->unknown_0x50);
#endif
}

// RR's PoseReloadedMenuDriverModel for a model outside its table: both scn roots get the model's
// animations, so the first frame it is drawn is not its bind pose.
static void Pose(ModelDirector *model, ToadetteHair *hair) {
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    if (model == nullptr || scnMgr == nullptr) return;
    const u32 curRootIdx = scnMgr->curScnRootIdx;
    const u32 drawnRootIdx = curRootIdx ^ 1;
    model->Update(drawnRootIdx);
    if (hair != nullptr) hair->Update(drawnRootIdx);
    scnMgr->curScnRootIdx = drawnRootIdx;
    scnMgr->curScnRoot = scnMgr->scnRoots[drawnRootIdx];
    scnMgr->UpdateScnRoot(0);
    scnMgr->curScnRootIdx = curRootIdx;
    scnMgr->curScnRoot = scnMgr->scnRoots[curRootIdx];
    model->Update(curRootIdx);
    if (hair != nullptr) hair->Update(curRootIdx);
}

// Built as RR builds a reloaded shared model, in a fresh heap, with this hud's skin.
static bool LoadPrivate(MenuDriverModelMgr &mgr, u8 hud, u32 character, u8 table) {
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    if (modelMgr == nullptr || scene == nullptr || scnMgr == nullptr) return false;
    const CharacterId id = static_cast<CharacterId>(character);
    ModelDirector *model = nullptr;
    ToadetteHair *hair = nullptr;
    EGG::ExpHeap *heap = nullptr;
    CC::UnlockMenuModelHeaps(*modelMgr);
    menuLoadHud = hud;
    const bool loaded = CC::LoadReloadedMenuDriverModel(*scene, *scnMgr, id, model, hair, heap) ||
                        CC::LoadDefaultReloadedMenuDriverModel(*scene, *scnMgr, id, model, hair, heap);
    menuLoadHud = 0;
    if (!loaded) return false;

    PrivateModel &p = privates[hud];
    DropPrivate(p, true);
    p.storage[0] = *reinterpret_cast<const u32 *>(mgr.models);
    MenuDriverModel *own = Own(p);
    own->model = model;
    CC::ResetReloadedMenuDriverModel(*own, id);
    own->Init();
    p.heap = heap;
    p.hair = hair;
    p.table = table;
    Pose(model, hair);
    Report(hud, character, table, "load");
    return true;
}

static bool SharedTaken(MenuDriverModelMgr &mgr, u8 hud, u32 character) {
    for (u8 i = 0; i < mgr.playerCount && i < kGameLocal; ++i) {
        if (i != hud && (placed & (1 << i)) != 0 && mgr.players[i].playerModel == Shared(mgr, character)) return true;
    }
    return false;
}

// The first player on a driver keeps the shared model; one who joins it, or wants another skin than
// it holds, gets their own, and stays on it while it still holds that driver and skin.
static void Place(MenuDriverModelMgr &mgr, u8 hud, u32 character, MenuDriverModel *prev) {
    PrivateModel &p = privates[hud];
    MenuDriverModel *own = Own(p);
    const u8 table = WantedTable(hud, character);
    const bool holds = own->model != nullptr && ModelCharacter(own) == character && p.table == table;
    if (prev == own && holds) {
        mgr.players[hud].playerModel = own;
        return;
    }
    if (!SharedTaken(mgr, hud, character) && sharedTables[character] == table) {
        Report(hud, character, table, "shared");
        return;
    }
    if (!holds && !LoadPrivate(mgr, hud, character, table)) return;
    mgr.players[hud].playerModel = own;
    Report(hud, character, table, "private");
}

// MenuModelMgr::RequestDriverModel+0x10 replaces its tail "b SetPlayerCharacter" (r3 the driver
// models, r4 hud, r5 character); LR is still the caller's.
static void RequestDriverModelWide(MenuDriverModelMgr *mgr, u32 hudArg, CharacterId character) {
    const u8 hud = hudArg & 0xff;
    MenuDriverModel *prev = hud < kGameLocal ? mgr->players[hud].playerModel : static_cast<MenuDriverModel *>(nullptr);
    mgr->SetPlayerCharacter(hud, character);
    if (hud >= kGameLocal) return;
    Sync(mgr);
    if (owner != mgr) return;
    placed |= 1 << hud;
    if (static_cast<u32>(character) >= kDrivers) return;
    Place(*mgr, hud, character, prev);
}
kmBranch(0x8059e578, RequestDriverModelWide);

bool ReinitPlayerMenuModel(u8 hud, CharacterId character) {
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (modelMgr == nullptr || !modelMgr->isActive || modelMgr->driverModels == nullptr) return false;
    MenuDriverModelMgr &mgr = *modelMgr->driverModels;
    if (hud >= kGameLocal || hud >= mgr.playerCount || static_cast<u32>(character) >= kDrivers) return false;
    Sync(&mgr);
    if (owner != &mgr) return false;

    const u8 table = WantedTable(hud, character);
    if (mgr.players[hud].playerModel == Shared(mgr, character)) {
        CC::UnlockMenuModelHeaps(*modelMgr);
        menuLoadHud = hud;
        const bool reloaded = CC::ReloadMenuDriverModel(mgr, character);
        menuLoadHud = 0;
        if (reloaded) {
            sharedTables[character] = table;
            CC::PoseReloadedMenuDriverModel(character);
            Report(hud, character, table, "reload");
        }
    } else {
        LoadPrivate(mgr, hud, character, table);
    }

    SectionMgr *sectionMgr = SectionMgr::sInstance;
    if (sectionMgr == nullptr || sectionMgr->curSection == nullptr) return true;
    Pages::CharacterSelect *page = sectionMgr->curSection->Get<Pages::CharacterSelect>();
    if (page != nullptr && page->models != nullptr) page->models[hud].RequestModel(character);
    return true;
}

typedef void (*DrawFn)(MenuDriverModelMgr *, u8);
typedef void (*TransparentFn)(ModelDirector *, bool);
typedef void (*LockFn)(ToadetteHair *, bool);
static const DrawFn driverModelsDraw = reinterpret_cast<DrawFn>(0x80830a80);
static const TransparentFn toggleTransparent = reinterpret_cast<TransparentFn>(0x8055f34c);
static const LockFn toggleLock = reinterpret_cast<LockFn>(0x807db028);

// A private Toadette's hair, as MenuDriverModelMgr::Update drives bangs for models[13]'s player
// (0x8083095C-0x808309F4): shown, and locked once the confirmed pose's animation has run out.
static void UpdateHair(MenuDriverModel *model, ToadetteHair *hair) {
    hair->ToggleVisible(true);
    bool lock = false;
    if (ModelState(model) == 1) {
        const AnmHolder *holder = model->model->modelTransformator->GetAnmHolderByType(ANMTYPE_CHR);
        lock = holder->GetFrame() >= holder->GetFrameCount();
    }
    toggleLock(hair, lock);
}

// MenuModelMgr::Update+0x24 replaces "bl MenuDriverModelMgr::Update" (r3 the driver models); Update
// saved LR and keeps only r31 across it. The privates update and hide first, as vanilla does its own
// models, so vanilla's player loop shows the one each player uses.
static void UpdateDriverModelsWide(MenuDriverModelMgr *mgr) {
    Sync(mgr);
    const bool mine = owner == mgr;
    for (u32 i = 0; mine && i < kGameLocal; ++i) {
        MenuDriverModel *own = Own(privates[i]);
        if (own->model == nullptr) continue;
        own->Update();
        own->ToggleVisible(false);
        if (privates[i].hair != nullptr) privates[i].hair->ToggleVisible(false);
    }
    mgr->Update();
    for (u8 i = 0; mine && i < mgr->playerCount && i < kGameLocal; ++i) {
        MenuDriverModel *own = Own(privates[i]);
        if (mgr->players[i].playerModel == own && privates[i].hair != nullptr) UpdateHair(own, privates[i].hair);
    }
}
kmCall(0x8059e4cc, UpdateDriverModelsWide);

// MenuModelMgr::Draw+0x2C replaces "bl MenuDriverModelMgr::Draw" (r3 the driver models, r4 hud);
// Draw saved LR and keeps only r30 and r31 across it. Vanilla Draw hides only models[], so every
// private but this hud's is hidden here before the pass renders.
static void DrawDriverModelsWide(MenuDriverModelMgr *mgr, u32 hudArg) {
    const u8 hud = hudArg & 0xff;
    driverModelsDraw(mgr, hud);
    if (owner != mgr) return;
    for (u8 i = 0; i < kGameLocal; ++i) {
        PrivateModel &p = privates[i];
        MenuDriverModel *own = Own(p);
        if (own->model == nullptr) continue;
        const bool shown = i == hud && mgr->players[hud].playerModel == own;
        if (!shown) own->ToggleTransparent(false);
        if (p.hair != nullptr) toggleTransparent(p.hair, shown && mgr->players[hud].isVisible);
    }
}
kmCall(0x8059e528, DrawDriverModelsWide);

}  // namespace SplitScreen8
