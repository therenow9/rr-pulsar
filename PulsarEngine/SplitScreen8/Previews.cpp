#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/egg/3D/Scn.hpp>
#include <core/egg/DVD/DvdRipper.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/rvl/dvd/dvd.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuModelMgr.hpp>
#include <MarioKartWii/3D/Scn/ScnMgr.hpp>
#include <MarioKartWii/3D/TextureHolder.hpp>
#include <MarioKartWii/Archive/ArchiveMgr.hpp>
#include <MarioKartWii/Driver/Toadette.hpp>
#include <MarioKartWii/GlobalFunctions.hpp>
#include <MarioKartWii/Mii/MiiHeadsModel.hpp>
#include <MarioKartWii/Scene/GameScene.hpp>
#include <MarioKartWii/UI/Section/SectionMgr.hpp>
#include <MarioKartWii/UI/Section/SectionParams.hpp>
#include <Driver/CustomCharacters.hpp>
#include <IO/LooseArchiveOverrides.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// P5-8's character previews on the 8-player page (D67). Every per-player structure of the game's preview
// path is 4 wide (ModelRenderer::params, MenuModelMgr's heaps and textures, MenuDriverModelMgr::players),
// so P5-8 have their own models and textures here and RendererMenu::DrawModels' per-player pass runs four
// more times for them. docs/plans/m4-menu-flow.md, phase C, C4b and C4c.

namespace Pulsar {
// RR's menu Mii body loader (Extra/MiiOutfitC.cpp): outfit C's BRRES from the SD card or the disc onto
// ScnMgr's current heap, any other character through BindDriverBRRES.
u32 LoadMiiOutfitCBRRES(void *holder, CharacterId character);
}  // namespace Pulsar

namespace SplitScreen8 {

const u32 kExtPlayers = kMaxLocal - kGameLocal;
const u16 kTextureSize = 256;  // MenuModelMgr::Init's driver textures (0x8059E460)
const u32 kTextureFormat = 5;

typedef void (*TextureCtorFn)(TextureHolder *, u16, u16, u32, EGG::Heap *);
typedef void (*ModelMgrDrawFn)(MenuModelMgr *, u32);
typedef TextureHolder *(*DriverTextureFn)(MenuModelMgr *, u32);
typedef void (*KartModelsFn)(MenuKartModelMgr *, u32);
typedef void (*KartModelsUpdateFn)(MenuKartModelMgr *);
typedef void (*SwitchStateFn)(MenuDriverModel *, s32, u32);
typedef void (*ToggleTransparentFn)(ModelDirector *, bool);
typedef void (*ToggleHairLockFn)(ToadetteHair *, bool);
static const TextureCtorFn textureCtor = reinterpret_cast<TextureCtorFn>(0x80563ce4);
static const ModelMgrDrawFn modelMgrDraw = reinterpret_cast<ModelMgrDrawFn>(0x8059e4fc);
static const DriverTextureFn driverTexture = reinterpret_cast<DriverTextureFn>(0x8059e634);
static const KartModelsFn kartModelsDraw = reinterpret_cast<KartModelsFn>(0x808331c4);
static const KartModelsUpdateFn kartModelsUpdate = reinterpret_cast<KartModelsUpdateFn>(0x8083300c);
// MenuDriverModel::SwitchState passes its player to MenuKartModelMgr::ToggleVisible only when it is not
// negative (cmpwi r28, 0 at 0x8082FBEC); that writes a 4-wide array, so P5-8 pass -1, not the header's u8.
static const SwitchStateFn switchState = reinterpret_cast<SwitchStateFn>(0x8082fb78);
static const ToggleTransparentFn toggleTransparent = reinterpret_cast<ToggleTransparentFn>(0x8055f34c);
static const ToggleHairLockFn toggleHairLock = reinterpret_cast<ToggleHairLockFn>(0x807db028);
// A menu Mii head as MenuDriverModelMgr keeps one per player for the section: built once (0x80830474-0x80830540:
// layer 5, on a body, the body's id less 0x18, the Mii, 0), then on every character change put on the new body
// (0x807DC2D8), given the Mii again (UpdateParams) and its expression, as SetPlayerCharacter does
// (0x80830FA0-0x80830FDC); neither of those builds a model. Building one allocates outside its own heap and only
// a section's end frees it, so a head is never rebuilt. None of these is in Kamek's externals.
typedef MiiHeadsModel *(*HeadCtorFn)(void *, u32, ModelDirector *, u32, Mii *, u32);
typedef void (*HeadParamsFn)(MiiHeadsModel *, u32, ModelDirector *, u32, Mii *);
typedef void (*HeadExpressionFn)(MiiHeadsModel *, u32);
static const HeadCtorFn headCtor = reinterpret_cast<HeadCtorFn>(0x807dbd80);
static const HeadParamsFn headAttach = reinterpret_cast<HeadParamsFn>(0x807dc2d8);
static const HeadParamsFn headParams = reinterpret_cast<HeadParamsFn>(0x807dc2c0);
static const HeadExpressionFn headExpression = reinterpret_cast<HeadExpressionFn>(0x807dc3e8);
const u32 kHeadSize = 0x70;
const u32 kHeadLayer = 5;
const float kMiiBodyScale = 1.6f;  // the Mii body loader's SetScale (0x8083129C, 0x808AC980)
static u8 *const *const miiManager = reinterpret_cast<u8 *const *>(0x809bd710);  // MiiManager::sInstance

// As RR's PreviewModel (Driver/LocalPlayerSkins.cpp): MenuDriverModel's constructor is not linkable, so the
// object is raw storage given the shared models' vtable word.
struct ExtPreview {
    u32 storage[sizeof(MenuDriverModel) / sizeof(u32)];
    EGG::ExpHeap *heap;
    ToadetteHair *hair;
    MiiHeadsModel *head;  // the player's head while it sits on this model, a Mii body; not owned
    u8 slot;
};
static ExtPreview previews[kExtPlayers];
// Each P5-8 player's model is built on a heap of its own inside a fixed arena made at the page's build, and the
// old model goes before the new one is built, so roll after roll a load always finds its block (D118). The size
// covers the heaviest of RR's 124 skins, Rosalina's 6th (479,828 bytes with its model), and a margin.
const u32 kArenaSize = 0x78000;
// A model that runs out of room while it is built writes through null inside the game's code, so a skin whose file
// and this much model will not fit the arena gets the stock model before anything is built (D123). It covers every
// RR model's need beyond its file but Rosalina's 6th (51,628 at most) and leaves Daisy's 6th, the largest file, room.
const u32 kModelAllowance = 0x26000;
static EGG::ExpHeap *arenas[kExtPlayers];
static MiiHeadsModel *heads[kExtPlayers];
static EGG::ExpHeap *headHeaps[kExtPlayers];
// A head keeps about 0x200 bytes on its own heap; its models' 132,672-byte block comes straight from the scene's
// MEM2 structs heap, which is why heads are built as the page opens (D119).
const u32 kHeadHeapSize = 0x1000;
static u32 textureStorage[kExtPlayers][sizeof(TextureHolder) / sizeof(u32)];
static TextureHolder *textures[kExtPlayers];
static const GameScene *previewScene;
// The pass count DrawModels' loop takes while the page shows P5-8 (DrawPassCount), else 0.
u8 extPassCount;

static MenuDriverModel *Own(ExtPreview &preview) {
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

// A head on the body leaves the scene before the body's heap goes, and waits for the next Mii body.
static void DropPreview(ExtPreview &preview, bool destroy) {
    if (destroy && Own(preview)->model != nullptr) {
        if (preview.head != nullptr)
            preview.head->ToggleVisible(false);
        RemoveModel(preview.hair);
        RemoveModel(Own(preview)->model);
        preview.heap->destroy();
    }
    memset(&preview, 0, sizeof(preview));
}

// GameScene::Reset (from reinit and exit) resets the ScnMgr, then reinit frees all three structsHeaps
// (0x8051B7D4), which hold the models and the textures: the models leave the ScnMgr here and the
// textures are only forgotten.
void ResetExtPreviews() {
    const bool destroy = previewScene == GameScene::GetCurrent();
    for (u32 i = 0; i < kExtPlayers; ++i) {
        DropPreview(previews[i], destroy);
        if (destroy && heads[i] != nullptr) {
            RemoveModel(heads[i]);
            headHeaps[i]->destroy();
        }
        if (destroy && arenas[i] != nullptr)
            arenas[i]->destroy();
        arenas[i] = nullptr;
        heads[i] = nullptr;
        headHeaps[i] = nullptr;
        textures[i] = nullptr;
    }
    previewScene = nullptr;
    extPassCount = 0;
}

// Built as the page first shows, for the section, on the scene's MEM2 structs heap (MEM1, which TextureHolder
// takes, has about 1.8 MB free at a 5-8 player section 0x54): the textures as MenuModelMgr::Init builds its own
// (0x8059E460, 512 KB for four), then each player's arena, then each player's Mii head on a Mii body, before
// any load has fragmented the heap (D118, D119).
bool ExtPreviewsShow(u32 count) {
    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    if (scene == nullptr)
        return false;
    if (previewScene != scene)
        ResetExtPreviews();
    previewScene = scene;
    EGG::Heap *parentHeap = scene->structsHeaps.heaps[1];
    const u16 heapFlags = parentHeap->dameFlag;
    parentHeap->dameFlag &= ~1;
    bool built = true;
    for (u32 i = 0; i < kExtPlayers && built; ++i) {
        if (textures[i] != nullptr)
            continue;
        TextureHolder *texture = reinterpret_cast<TextureHolder *>(textureStorage[i]);
        textureCtor(texture, kTextureSize, kTextureSize, kTextureFormat, parentHeap);
        built = texture->texture != nullptr;
        if (built)
            textures[i] = texture;
    }
    for (u32 i = 0; i + kGameLocal < count && built; ++i) {
        if (arenas[i] == nullptr)
            arenas[i] = EGG::ExpHeap::Create(kArenaSize, parentHeap, 0);
        built = arenas[i] != nullptr;
    }
    parentHeap->dameFlag = heapFlags;
    if (!built)
        return false;
    MiiGroup &miis = SectionMgr::sInstance->sectionParams->playerMiis;
    for (u32 player = kGameLocal; player < count && player < miis.miiCount; ++player) {
        const Mii *mii = miis.GetMii(player);
        const s32 miiCharacter = mii != nullptr ? static_cast<s32>(GetMiiCharacterId(*mii)) : -1;
        if (heads[player - kGameLocal] == nullptr && miiCharacter >= 0)
            ExtPreviewLoad(player, static_cast<CharacterId>(miiCharacter), 0);
    }
    extPassCount = count;
    return true;
}

void ExtPreviewsHide() {
    extPassCount = 0;
}

GX::TexObj *ExtPreviewTexObj(u32 player) {
    const u32 i = player - kGameLocal;
    return i < kExtPlayers && textures[i] != nullptr ? &textures[i]->texObj : static_cast<GX::TexObj *>(nullptr);
}

// Both scn roots take the model's animations, so the first frame it is drawn is not its bind pose (RR's Pose).
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

static u32 TotalFree(EGG::Heap *heap) {
    return static_cast<EGG::ExpHeap *>(heap)->getTotalFreeSize();
}

// A head's models take one block of about 132,672 bytes from the scene's MEM2 structs heap, outside the heap it
// is built on (D119). Without it InitModel writes through a null ScnRfl (first in ScnRfl::SetFogIdx, 0x802443DC), a
// fault on a console, so a head is built only when the block is there. HeadBuilt is a backstop for Dolphin only: a
// refused head has already written through null, and any ScnRfl it did build stays allocated for the section.
const u32 kHeadModelsBlock = 0x21000;
const u32 kScnRflType = 4;  // ScnMdlEx::type InitModel tests (0x8055C648)

static bool HeadBuilt(const MiiHeadsModel *head) {
    for (u32 s = 0; s < 2; ++s) {
        const EGG::ScnMdlEx *ex = head->scnMdlEx[s];
        if (ex == nullptr || ex->type != kScnRflType || ex->scnObj == nullptr)
            return false;
    }
    return true;
}

// The player's head on a Mii body: built once on a small heap of its own (as the scene's current one, so the
// head's own objects land there), then only moved. SetPlayerCharacter raises MiiManager's byte at +0x265 around
// its head work (0x80830E24, 0x80831028).
static MiiHeadsModel *PutHead(u32 i, ModelDirector *body, u32 miiId, Mii *mii, EGG::Heap *parentHeap, ScnMgr *scnMgr) {
    u8 &miiBusy = (*miiManager)[0x265];
    const u8 oldBusy = miiBusy;
    miiBusy = 1;
    MiiHeadsModel *head = heads[i];
    if (head != nullptr) {
        // The attach reads the head's Mii (+0x4C) for the body's colours (0x807DC1F8), and a pick replaces the
        // player's Mii object (CopyMiiToOtherGroup), so the head takes the current one first.
        head->mii = mii;
        headAttach(head, kHeadLayer, body, miiId, mii);
    } else if (parentHeap->getAllocatableSize(4) < kHeadHeapSize + kHeadModelsBlock) {
        OS::Report("ss8 preview: player %u head not built, largest %u\n", kGameLocal + i + 1, parentHeap->getAllocatableSize(4));
    } else {
        const u16 heapFlags = parentHeap->dameFlag;
        parentHeap->dameFlag &= ~1;
        EGG::ExpHeap *heap = EGG::ExpHeap::Create(kHeadHeapSize, parentHeap, 0);
        parentHeap->dameFlag = heapFlags;
        if (heap != nullptr) {
            EGG::Allocator *allocator = new (heap) EGG::Allocator(heap, 0x20);
            EGG::Heap *oldHeap = scnMgr->curHeap;
            EGG::Allocator *oldAllocator = scnMgr->curAllocator;
            EGG::Allocator *oldMenuAllocator = menuAllocator;
            scnMgr->curHeap = heap;
            scnMgr->curAllocator = allocator;
            menuAllocator = allocator;
            EGG::Heap *oldCurrentHeap = heap->BecomeCurrentHeap();
            void *storage = EGG::Heap::alloc(kHeadSize, 4, heap);
            if (storage != nullptr)
                head = headCtor(storage, kHeadLayer, body, miiId, mii, 0);
            oldCurrentHeap->BecomeCurrentHeap();
            scnMgr->curHeap = oldHeap;
            scnMgr->curAllocator = oldAllocator;
            menuAllocator = oldMenuAllocator;
            if (head != nullptr && !HeadBuilt(head)) {
                OS::Report("ss8 preview: player %u head refused\n", kGameLocal + i + 1);
                RemoveModel(head);
                head = nullptr;
            }
            if (head == nullptr) {
                heap->destroy();
            } else {
                const u32 kept = heap->adjust();
                heads[i] = head;
                headHeaps[i] = heap;
                OS::Report("ss8 preview: player %u head kept %u\n", kGameLocal + i + 1, kept);
            }
        }
    }
    if (head != nullptr) {
        headParams(head, kHeadLayer, body, miiId, mii);
        headExpression(head, 0);
    }
    miiBusy = oldBusy;
    return head;
}

// As RR's LoadPreview builds a player's own model, from the skin's BRRES or, for slot 0, the menu archive's stock
// one, but on a heap filling the player's arena, after the old model has left it (D118). A Mii is its outfit's body
// as the Mii body loader builds it (0x80831100, RR's outfit C loader in place of BindDriverBRRES) with the player's
// Mii head on it. A skin that fails leaves the stock model and returns false, and the caller puts the slot back to 0.
bool ExtPreviewLoad(u32 player, CharacterId characterId, u32 slotArg) {
    const u32 i = player - kGameLocal;
    const u32 character = static_cast<u32>(characterId);
    const bool isMii = character >= MII_S_A_MALE && character <= MII_L_C_FEMALE;
    const u32 slot = isMii ? 0 : slotArg;
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (i >= kExtPlayers || (character >= Pulsar::Driver::CHARACTER_COUNT && !isMii) || modelMgr == nullptr || modelMgr->driverModels == nullptr)
        return false;
    MiiGroup &miis = SectionMgr::sInstance->sectionParams->playerMiis;
    Mii *mii = isMii && player < miis.miiCount ? miis.GetMii(player) : static_cast<Mii *>(nullptr);
    if (isMii && mii == nullptr)
        return false;
    ExtPreview &preview = previews[i];
    if (Own(preview)->model != nullptr && Own(preview)->id == character && preview.slot == slot)
        return true;

    char path[0x80];
    u32 fileSize = 0;
    if (slot != 0) {
        snprintf(path, sizeof(path), "/Scene/Model/Driver/%s-%u.brres", ArchiveMgr::GetKartArchivePostfix(characterId), slot);
        const s32 entryNum = Pulsar::IOOverrides::ConvertPathToEntryNumWithLooseOverride(path);
        DVD::FileInfo fileInfo = {};
        if (entryNum < 0 || !DVD::FastOpen(entryNum, &fileInfo)) {
            ExtPreviewLoad(player, characterId, 0);
            return false;
        }
        fileSize = fileInfo.length;
        DVD::Close(&fileInfo);
    }

    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    EGG::ExpHeap *arena = arenas[i];
    if (scene == nullptr || scnMgr == nullptr || scene != previewScene || arena == nullptr)
        return false;
    DropPreview(preview, true);
    if (slot != 0 && fileSize + kModelAllowance > arena->getAllocatableSize(4)) {
        OS::Report("ss8 preview: player %u %#x slot %u file %u too heavy, arena %u\n", player + 1, character, slot, fileSize, arena->getAllocatableSize(4));
        ExtPreviewLoad(player, characterId, 0);
        return false;
    }
    EGG::ExpHeap *heap = EGG::ExpHeap::Create(arena->getAllocatableSize(4), arena, 0);
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
    if (isMii) {
        bound = Pulsar::LoadMiiOutfitCBRRES(&brresHandle, characterId) != 0;
    } else if (slot != 0) {
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
    if (loaded && isMii)
        model->SetScale(kMiiBodyScale);

    scnMgr->curHeap = oldHeap;
    scnMgr->curAllocator = oldAllocator;
    menuAllocator = oldMenuAllocator;

    MiiHeadsModel *head = loaded && isMii ? PutHead(i, model, character - MII_S_A_MALE, mii, scene->structsHeaps.heaps[1], scnMgr) : static_cast<MiiHeadsModel *>(nullptr);
    if (!loaded || (isMii && head == nullptr)) {
        OS::Report("ss8 preview: player %u %#x slot %u file %u failed, arena %u\n", player + 1, character, slot, fileSize, heap->getAllocatableSize(4));
        RemoveModel(hair);
        RemoveModel(model);
        heap->destroy();
        if (slot != 0)
            ExtPreviewLoad(player, characterId, 0);
        return false;
    }
    OS::Report("ss8 preview: player %u %#x slot %u file %u arena free %u, free MEM1 %u MEM2 %u\n", player + 1, character, slot, fileSize, TotalFree(heap), TotalFree(scene->structsHeaps.heaps[0]),
      TotalFree(scene->structsHeaps.heaps[1]));

    preview.storage[0] = *reinterpret_cast<const u32 *>(modelMgr->driverModels->models);
    MenuDriverModel *own = Own(preview);
    own->model = model;
    own->charSelTransformator = model->modelTransformator;
    own->onKartTransformator = nullptr;
    own->id = character;
    own->Init();
    preview.heap = heap;
    preview.hair = hair;
    preview.head = head;
    preview.slot = slot;
    Pose(model, hair);
    return true;
}

// A new Mii in the player's playerMiis entry keeps its object and its character id, so the next load rebuilds.
void ExtPreviewDrop(u32 player) {
    const u32 i = player - kGameLocal;
    if (i < kExtPlayers)
        DropPreview(previews[i], previewScene == GameScene::GetCurrent());
}

void ExtPreviewPose(u32 player, bool confirmed) {
    const u32 i = player - kGameLocal;
    if (i < kExtPlayers && Own(previews[i])->model != nullptr)
        switchState(Own(previews[i]), -1, confirmed ? MenuDriverModel::MENUDRIVERMODEL_STATE_ONCHARSELECT : MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
}

bool ExtPreviewIdle(u32 player) {
    const u32 i = player - kGameLocal;
    return i < kExtPlayers && Own(previews[i])->model != nullptr && Own(previews[i])->state == MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE;
}

bool ExtPreviewStock(u32 player, CharacterId character) {
    const u32 i = player - kGameLocal;
    return i < kExtPlayers && Own(previews[i])->model != nullptr && Own(previews[i])->id == character && previews[i].slot == 0;
}

// DrawModels' pass loop bound, "lbz r0, 0x21(r3)" at 0x8059F220 (MenuModelMgr's player count, 4 in a 5-8
// player game): the page's count while it shows P5-8. Only r0 is live there, then cmpw sets CR0 again.
asmFunc DrawPassCount() {
    ASM(
        nofralloc;
        lbz r0, 0x21(r3);
        lis r12, extPassCount @ha;
        lbz r12, extPassCount @l(r12);
        cmpwi r12, 0;
        beqlr;
        mr r0, r12;
        blr;)
}
kmCall(0x8059f220, DrawPassCount);

static void SetPreviewShown(ExtPreview &preview, bool shown) {
    MenuDriverModel *own = Own(preview);
    if (own->model == nullptr)
        return;
    own->ToggleTransparent(shown);
    if (preview.hair != nullptr)
        toggleTransparent(preview.hair, shown);
    if (preview.head != nullptr)
        toggleTransparent(preview.head, shown);  // as MenuDriverModelMgr::Draw shows a player's Mii head (0x80830BA8)
}

// DrawModels' "bl MenuModelMgr::Draw" (0x8059F1A0, r4 = the pass): P1-4's passes draw as before with P5-8
// hidden. P5-8's pass hides every game model by drawing player 1's with its visible byte cleared (the Mii
// head, Toadette's bangs and RR's preview hair follow that byte), every kart by asking for a player no kart
// has (MenuKartModelMgr::Draw compares, never indexes), then shows only that player's own model.
static void DrawPass(MenuModelMgr *modelMgr, u32 passArg) {
    const u32 pass = passArg & 0xff;
    if (extPassCount == 0 || pass < kGameLocal || modelMgr->driverModels == nullptr) {
        modelMgrDraw(modelMgr, pass);
        for (u32 i = 0; extPassCount != 0 && i < kExtPlayers; ++i) SetPreviewShown(previews[i], false);
        return;
    }
    MenuDriverModelMgr::Player &first = modelMgr->driverModels->players[0];
    const bool firstVisible = first.isVisible;
    first.isVisible = false;
    modelMgrDraw(modelMgr, 0);
    first.isVisible = firstVisible;
    kartModelsDraw(modelMgr->kartModels, kGameLocal);
    for (u32 i = 0; i < kExtPlayers; ++i) SetPreviewShown(previews[i], i == pass - kGameLocal);
}
kmCall(0x8059f1a0, DrawPass);

// DrawModels' "bl MenuModelMgr::GetDriverTexture" (0x8059F1D8): P5-8's passes copy into their own texture.
// DrawPassCount only extends the loop once every texture exists, and CopyFromEFB follows unconditionally.
static TextureHolder *DrawPassTexture(MenuModelMgr *modelMgr, u32 passArg) {
    const u32 pass = passArg & 0xff;
    if (pass >= kGameLocal && pass < kMaxLocal && textures[pass - kGameLocal] != nullptr)
        return textures[pass - kGameLocal];
    return driverTexture(modelMgr, pass);
}
kmCall(0x8059f1d8, DrawPassTexture);

// A preview Toadette's hair, as MenuDriverModelMgr::Update drives bangs (0x8083095C-0x808309F4).
static void UpdateHair(MenuDriverModel *model, ToadetteHair *hair) {
    hair->ToggleVisible(true);
    bool lock = false;
    if (model->state == MenuDriverModel::MENUDRIVERMODEL_STATE_ONCHARSELECT) {
        const AnmHolder *holder = model->model->modelTransformator->GetAnmHolderByType(ANMTYPE_CHR);
        lock = holder->GetFrame() >= holder->GetFrameCount();
    }
    toggleHairLock(hair, lock);
}

// MenuModelMgr::Update's "bl MenuKartModelMgr::Update" (0x8059E4D4, 8 bytes after RR's driver-model hook;
// only r31 is live after). Nothing in the game puts P5-8's models among those drawn, so each is shown here
// while the page shows P5-8 and hidden otherwise. GlobeScene::OnCalc reaches this too.
static void UpdateModels(MenuKartModelMgr *kartModels) {
    kartModelsUpdate(kartModels);
    for (u32 i = 0; i < kExtPlayers; ++i) {
        ExtPreview &preview = previews[i];
        MenuDriverModel *own = Own(preview);
        if (own->model == nullptr)
            continue;
        const bool shown = extPassCount > kGameLocal + i;
        own->Update();
        own->ToggleVisible(shown);
        if (preview.head != nullptr)
            preview.head->ToggleVisible(shown);  // MenuDriverModelMgr::Update toggles each head (0x808307DC, 0x80830934)
        if (preview.hair == nullptr)
            continue;
        if (shown)
            UpdateHair(own, preview.hair);
        else
            preview.hair->ToggleVisible(false);
    }
}
kmCall(0x8059e4d4, UpdateModels);

}  // namespace SplitScreen8
#endif
