#include <Driver/CustomCharacters.hpp>
#include <IO/LooseArchiveOverrides.hpp>
#include <MarioKartWii/Archive/ArchiveMgr.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/3D/Scn/ScnMgr.hpp>
#include <MarioKartWii/Driver/Toadette.hpp>
#include <MarioKartWii/RKNet/RKNetController.hpp>
#include <MarioKartWii/Scene/GameScene.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <core/egg/DVD/DvdRipper.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <core/rvl/dvd/dvd.hpp>

namespace Pulsar {
namespace Driver {

// In offline local multiplayer each player keeps their own skin, so two players on one character can
// show different skins. Player 1 keeps selectedSlots, which 1P, ghosts and online also read, so a skin
// picked alone carries into multiplayer and back. Players 2-4 each have their own row here.
static u8 localPlayerSlots[LOCAL_PLAYER_SKIN_COUNT - 1][CHARACTER_COUNT];

static bool IsOnline() {
    const RKNet::Controller *controller = RKNet::Controller::sInstance;
    return controller != nullptr && controller->roomType != RKNet::ROOMTYPE_NONE;
}

static bool IsValidSlot(u32 character, u32 slot) {
    return character < CHARACTER_COUNT && slot <= MAX_CUSTOM_CHARACTER_SLOTS && characterTables[character][slot];
}

u32 GetLocalPlayerSlot(u32 hud, CharacterId characterId) {
    const u32 character = static_cast<u32>(characterId);
    if (character >= CHARACTER_COUNT)
        return 0;
    // Online rooms send one slot per character (PulSELECT), so every local there shares selectedSlots.
    if (hud == 0 || IsOnline())
        return selectedSlots[character];
    if (hud >= LOCAL_PLAYER_SKIN_COUNT)
        return 0;
    const u32 slot = localPlayerSlots[hud - 1][character];
    return IsValidSlot(character, slot) ? slot : 0;
}

void SetLocalPlayerSlot(u32 hud, CharacterId characterId, u32 slot) {
    const u32 character = static_cast<u32>(characterId);
    if (!IsValidSlot(character, slot))
        return;
    if (hud == 0 || IsOnline())
        selectedSlots[character] = slot;
    else if (hud < LOCAL_PLAYER_SKIN_COUNT)
        localPlayerSlots[hud - 1][character] = slot;
}

// A player's own preview model, used when the character's shared model (models[character]) already
// stands for another player. MenuDriverModel's constructor is not linkable, so the object is raw
// storage given the shared models' vtable word and set up the way LoadDriverBRRES sets up its swap.
struct PreviewModel {
    u32 storage[sizeof(MenuDriverModel) / sizeof(u32)];
    EGG::ExpHeap *heap;
    ToadetteHair *hair;
    u8 slot;
};
static PreviewModel previews[LOCAL_PLAYER_SKIN_COUNT];
static MenuDriverModelMgr *previewOwner;
static const GameScene *previewScene;
// The manager's constructor parks every player on Mario through SetPlayerCharacter directly, so a
// player only counts as standing on a model once RequestDriverModel has placed them.
static u8 placedPlayers;

static MenuDriverModel *Own(PreviewModel &preview) {
    return reinterpret_cast<MenuDriverModel *>(preview.storage);
}

static void RemoveModel(ModelDirector *model) {
    if (model == nullptr)
        return;
    ScnMgr *scnMgr = ScnMgr::sInstance[model->scnMgrIdx];
    if ((model->bitfield & 0x100000) && scnMgr != nullptr) {
        model->ToggleVisible(false);
        scnMgr->RemoveModelDirector(model);
    }
}

static void DropPreview(PreviewModel &preview, bool destroy) {
    if (destroy && Own(preview)->model != nullptr) {
        RemoveModel(preview.hair);
        RemoveModel(Own(preview)->model);
        preview.heap->destroy();
    }
    memset(&preview, 0, sizeof(preview));
}

void DestroyLocalPlayerPreviews() {
    // A preview built in an earlier scene died with that scene's heaps; only forget it.
    const bool destroy = previewOwner != nullptr && previewScene == GameScene::GetCurrent();
    for (u32 i = 0; i < LOCAL_PLAYER_SKIN_COUNT; ++i) DropPreview(previews[i], destroy);
    previewOwner = nullptr;
    previewScene = nullptr;
}

static bool UsesLocalPlayerPreviews(const MenuDriverModelMgr *manager) {
    const SectionMgr *sectionMgr = SectionMgr::sInstance;
    return manager != nullptr && manager->playerCount > 1 && sectionMgr != nullptr && sectionMgr->curSection != nullptr && sectionMgr->curSection->sectionId == SECTION_LOCAL_MULTIPLAYER;
}

// A manager built since the last call means a new menu scene: the old previews go with it.
static bool Sync(MenuDriverModelMgr *manager) {
    if (previewOwner == manager)
        return manager != nullptr;
    if (previewOwner != nullptr)
        DestroyLocalPlayerPreviews();
    if (!UsesLocalPlayerPreviews(manager))
        return false;
    previewOwner = manager;
    previewScene = GameScene::GetCurrent();
    placedPlayers = 0;
    return true;
}

static bool SharedTakenByOther(const MenuDriverModelMgr &manager, u32 hud, u32 character) {
    for (u32 i = 0; i < manager.playerCount && i < LOCAL_PLAYER_SKIN_COUNT; ++i) {
        if (i != hud && (placedPlayers & (1 << i)) != 0 && manager.players[i].playerModel == &manager.models[character])
            return true;
    }
    return false;
}

// The slot the character's shared model should hold, and the player standing on it (0 when it is
// player 1's or nobody's), so a failed load clears that player's slot rather than player 1's.
u32 GetSharedModelSlot(CharacterId characterId, bool focused, u32 &standingHud) {
    const u32 character = static_cast<u32>(characterId);
    standingHud = 0;
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    MenuDriverModelMgr *manager = modelMgr != nullptr ? modelMgr->driverModels : static_cast<MenuDriverModelMgr *>(nullptr);
    if (character >= CHARACTER_COUNT || !Sync(manager))
        return focused ? selectedSlots[character] : 0;
    for (u32 i = 0; i < manager->playerCount && i < LOCAL_PLAYER_SKIN_COUNT; ++i) {
        if ((placedPlayers & (1 << i)) != 0 && manager->players[i].playerModel == &manager->models[character]) {
            standingHud = i;
            return GetLocalPlayerSlot(i, characterId);
        }
    }
    return 0;
}

u32 GetRequestedModelSlot(u32 hud, CharacterId characterId) {
    const u32 character = static_cast<u32>(characterId);
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    MenuDriverModelMgr *manager = modelMgr != nullptr ? modelMgr->driverModels : static_cast<MenuDriverModelMgr *>(nullptr);
    if (character >= CHARACTER_COUNT || !Sync(manager))
        return character < CHARACTER_COUNT ? selectedSlots[character] : 0;
    // Another player keeps the shared model as it is; this one gets a preview of their own instead.
    u32 standingHud;
    if (SharedTakenByOther(*manager, hud, character))
        return GetSharedModelSlot(characterId, true, standingHud);
    return GetLocalPlayerSlot(hud, characterId);
}

// Both scn roots take the model's animations, so the first frame it is drawn is not its bind pose.
static void Pose(ModelDirector *model, ToadetteHair *hair) {
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    if (model == nullptr || scnMgr == nullptr)
        return;
    const u32 curRootIdx = scnMgr->curScnRootIdx;
    const u32 drawnRootIdx = curRootIdx ^ 1;
    model->Update(drawnRootIdx);
    if (hair != nullptr)
        hair->Update(drawnRootIdx);
    scnMgr->curScnRootIdx = drawnRootIdx;
    scnMgr->curScnRoot = scnMgr->scnRoots[drawnRootIdx];
    scnMgr->UpdateScnRoot(0);
    scnMgr->curScnRootIdx = curRootIdx;
    scnMgr->curScnRoot = scnMgr->scnRoots[curRootIdx];
    model->Update(curRootIdx);
    if (hair != nullptr)
        hair->Update(curRootIdx);
}

// Built as LoadDriverBRRES builds a skin, in a heap of its own, from the skin's BRRES or, for slot 0,
// the menu archive's stock one. The heap is trimmed after: a stock preview keeps only its model.
static bool LoadPreview(MenuDriverModelMgr &manager, u32 hud, u32 character, u32 slot) {
    const CharacterId characterId = static_cast<CharacterId>(character);
    char path[0x80];
    u32 fileSize = 0;
    if (slot != 0) {
        snprintf(path, sizeof(path), "/Scene/Model/Driver/%s-%u.brres", ArchiveMgr::GetKartArchivePostfix(characterId), slot);
        const s32 entryNum = IOOverrides::ConvertPathToEntryNumWithLooseOverride(path);
        DVD::FileInfo fileInfo = {};
        if (entryNum < 0 || !DVD::FastOpen(entryNum, &fileInfo))
            return false;
        fileSize = fileInfo.length;
        DVD::Close(&fileInfo);
    }

    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    if (scene == nullptr || scnMgr == nullptr)
        return false;
    EGG::Heap *parentHeap = scene->structsHeaps.heaps[0];
    const u16 heapFlags = parentHeap->dameFlag;
    parentHeap->dameFlag &= ~1;
    EGG::ExpHeap *heap = EGG::ExpHeap::Create(fileSize + 0xe1000, parentHeap, 0);
    parentHeap->dameFlag = heapFlags;
    if (heap == nullptr)
        return false;

    EGG::Allocator *allocator = new (heap) EGG::Allocator(heap, 0x20);
    EGG::Heap *oldHeap = scnMgr->curHeap;
    EGG::Allocator *oldAllocator = scnMgr->curAllocator;
    EGG::Allocator *oldMenuAllocator = menuAllocator;
    scnMgr->curHeap = heap;
    scnMgr->curAllocator = allocator;
    menuAllocator = allocator;

    ModelDirector *model = new (heap) ModelDirector(2, 0);
    MenuModelBRRESHandle brresHandle;
    bool bound;
    if (slot != 0) {
        g3d::ResFile brres;
        brres.data = static_cast<g3d::ResFileData *>(EGG::DvdRipper::LoadToMainRAM(path, nullptr, heap, EGG::DvdRipper::ALLOC_FROM_HEAD, 0, nullptr, nullptr));
        if (brres.data != nullptr)
            ModelDirector::BindBRRESImpl(brres, path, nullptr, 0);
        brresHandle.menuModelBRRES = brres;
        bound = brres.data != nullptr;
    } else {
        bound = brresHandle.BindDriverBRRES(characterId);
    }
    const bool loaded = bound && brresHandle.LoadDriverModel(*model, characterId);
    ToadetteHair *hair = nullptr;
    if (loaded && character == TOADETTE && ModelDirector::MdlExists("hair", brresHandle.menuModelBRRES)) {
        EGG::Heap *oldCurrentHeap = heap->BecomeCurrentHeap();
        hair = new (heap) ToadetteHair(brresHandle.menuModelBRRES, model, 1);
        oldCurrentHeap->BecomeCurrentHeap();
    }

    scnMgr->curHeap = oldHeap;
    scnMgr->curAllocator = oldAllocator;
    menuAllocator = oldMenuAllocator;

    if (!loaded) {
        RemoveModel(model);
        heap->destroy();
        return false;
    }
    heap->adjust();

    PreviewModel &preview = previews[hud];
    DropPreview(preview, true);
    preview.storage[0] = *reinterpret_cast<const u32 *>(manager.models);
    MenuDriverModel *own = Own(preview);
    own->model = model;
    own->charSelTransformator = model->modelTransformator;
    own->onKartTransformator = nullptr;
    own->id = character;
    own->Init();
    preview.heap = heap;
    preview.hair = hair;
    preview.slot = slot;
    Pose(model, hair);
    return true;
}

// The first player on a character keeps its shared model; a player who joins it gets their own
// preview, and keeps it while it still holds that character and skin.
static void Place(MenuDriverModelMgr &manager, u32 hud, u32 character, MenuDriverModel *previous) {
    PreviewModel &preview = previews[hud];
    MenuDriverModel *own = Own(preview);
    const u32 slot = GetLocalPlayerSlot(hud, static_cast<CharacterId>(character));
    const bool holds = own->model != nullptr && own->id == character && preview.slot == slot;
    if (previous == own && holds) {
        manager.players[hud].playerModel = own;
        return;
    }
    if (!SharedTakenByOther(manager, hud, character))
        return;
    if (!holds && !LoadPreview(manager, hud, character, slot))
        return;
    manager.players[hud].playerModel = own;
}

// MenuModelMgr::RequestDriverModel+0x10 replaces its tail "b SetPlayerCharacter" (r3 the driver models,
// r4 the player, r5 the character); LR is still the caller's.
static void RequestDriverModelForPlayer(MenuDriverModelMgr *manager, u32 playerArg, CharacterId characterId) {
    const u32 hud = playerArg & 0xff;
    MenuDriverModel *previous = hud < LOCAL_PLAYER_SKIN_COUNT ? manager->players[hud].playerModel : static_cast<MenuDriverModel *>(nullptr);
    manager->SetPlayerCharacter(hud, characterId);
    if (hud >= LOCAL_PLAYER_SKIN_COUNT || !Sync(manager))
        return;
    placedPlayers |= 1 << hud;
    if (static_cast<u32>(characterId) < CHARACTER_COUNT)
        Place(*manager, hud, characterId, previous);
}
kmBranch(0x8059e578, RequestDriverModelForPlayer);

// A player on their own preview reloads it; anyone else reloads the character's shared model.
bool LoadLocalPlayerModel(u32 hud, CharacterId characterId, u32 slot) {
    const u32 character = static_cast<u32>(characterId);
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    MenuDriverModelMgr *manager = modelMgr != nullptr ? modelMgr->driverModels : static_cast<MenuDriverModelMgr *>(nullptr);
    if (hud < LOCAL_PLAYER_SKIN_COUNT && character < CHARACTER_COUNT && Sync(manager) && hud < manager->playerCount && manager->players[hud].playerModel == Own(previews[hud]))
        return LoadPreview(*manager, hud, character, slot);
    return LoadDriverBRRES(characterId, slot);
}

bool HasOwnLocalPlayerSlots() {
    return !IsOnline();
}

typedef void (*DriverModelsDraw)(MenuDriverModelMgr *, u8);
typedef void (*ToggleTransparent)(ModelDirector *, bool);
typedef void (*ToggleHairLock)(ToadetteHair *, bool);
static const DriverModelsDraw driverModelsDraw = reinterpret_cast<DriverModelsDraw>(0x80830a80);
static const ToggleTransparent toggleTransparent = reinterpret_cast<ToggleTransparent>(0x8055f34c);
static const ToggleHairLock toggleHairLock = reinterpret_cast<ToggleHairLock>(0x807db028);

// A preview Toadette's hair, as MenuDriverModelMgr::Update drives bangs for the shared Toadette
// (0x8083095C-0x808309F4): shown, and locked once the confirmed pose's animation has run out.
static void UpdateHair(MenuDriverModel *model, ToadetteHair *hair) {
    hair->ToggleVisible(true);
    bool lock = false;
    if (model->state == MenuDriverModel::MENUDRIVERMODEL_STATE_ONCHARSELECT) {
        const AnmHolder *holder = model->model->modelTransformator->GetAnmHolderByType(ANMTYPE_CHR);
        lock = holder->GetFrame() >= holder->GetFrameCount();
    }
    toggleHairLock(hair, lock);
}

// MenuModelMgr::Update+0x24 replaces "bl MenuDriverModelMgr::Update" (r3 the driver models); Update
// saved LR and keeps only r31 across it. The previews update and hide first, as the manager does its
// own models, so its player loop then shows the one each player uses.
static void UpdateDriverModels(MenuDriverModelMgr *manager) {
    const bool mine = Sync(manager);
    for (u32 i = 0; mine && i < LOCAL_PLAYER_SKIN_COUNT; ++i) {
        MenuDriverModel *own = Own(previews[i]);
        if (own->model == nullptr)
            continue;
        own->Update();
        own->ToggleVisible(false);
        if (previews[i].hair != nullptr)
            previews[i].hair->ToggleVisible(false);
    }
    manager->Update();
    for (u32 i = 0; mine && i < manager->playerCount && i < LOCAL_PLAYER_SKIN_COUNT; ++i) {
        MenuDriverModel *own = Own(previews[i]);
        if (manager->players[i].playerModel == own && previews[i].hair != nullptr)
            UpdateHair(own, previews[i].hair);
    }
}
kmCall(0x8059e4cc, UpdateDriverModels);

// MenuModelMgr::Draw+0x2C replaces "bl MenuDriverModelMgr::Draw" (r3 the driver models, r4 the
// player); Draw saved LR and keeps only r30 and r31 across it. The manager's Draw hides only its own
// models, so every preview but this player's is hidden here before the pass renders.
static void DrawDriverModels(MenuDriverModelMgr *manager, u32 playerArg) {
    const u32 hud = playerArg & 0xff;
    driverModelsDraw(manager, hud);
    if (previewOwner != manager)
        return;
    for (u32 i = 0; i < LOCAL_PLAYER_SKIN_COUNT; ++i) {
        PreviewModel &preview = previews[i];
        MenuDriverModel *own = Own(preview);
        if (own->model == nullptr)
            continue;
        const bool shown = i == hud && manager->players[hud].playerModel == own;
        if (!shown)
            own->ToggleTransparent(false);
        if (preview.hair != nullptr)
            toggleTransparent(preview.hair, shown && manager->players[hud].isVisible);
    }
}
kmCall(0x8059e528, DrawDriverModels);

}  // namespace Driver
}  // namespace Pulsar
