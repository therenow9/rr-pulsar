#ifdef SS8_PAGES
#include <kamek.hpp>
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
#include <MarioKartWii/Scene/GameScene.hpp>
#include <Driver/CustomCharacters.hpp>
#include <IO/LooseArchiveOverrides.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// P5-8's character previews on the 8-player page (D67). Every per-player structure of the game's preview
// path is 4 wide (ModelRenderer::params, MenuModelMgr's heaps and textures, MenuDriverModelMgr::players),
// so P5-8 have their own models and textures here and RendererMenu::DrawModels' per-player pass runs four
// more times for them. docs/plans/m4-menu-flow.md, phase C, C4b.

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

// As RR's PreviewModel (Driver/LocalPlayerSkins.cpp): MenuDriverModel's constructor is not linkable, so the
// object is raw storage given the shared models' vtable word.
struct ExtPreview {
    u32 storage[sizeof(MenuDriverModel) / sizeof(u32)];
    EGG::ExpHeap *heap;
    ToadetteHair *hair;
    u8 slot;
};
static ExtPreview previews[kExtPlayers];
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

static void DropPreview(ExtPreview &preview, bool destroy) {
    if (destroy && Own(preview)->model != nullptr) {
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
        textures[i] = nullptr;
    }
    previewScene = nullptr;
    extPassCount = 0;
}

// Built as the page shows: the textures as MenuModelMgr::Init builds its own (0x8059E460), but on the
// scene's MEM2 structs heap, where SkinModelHeap puts the models (D99): MEM1, which TextureHolder takes,
// has about 1.9 MB free at a 5-8 player section 0x54, and four textures cost 512 KB.
bool ExtPreviewsShow(u32 count) {
    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    if (scene == nullptr)
        return false;
    if (previewScene != scene)
        ResetExtPreviews();
    previewScene = scene;
    EGG::Heap *parentHeap = scene->structsHeaps.heaps[1];
    for (u32 i = 0; i < kExtPlayers; ++i) {
        if (textures[i] != nullptr)
            continue;
        const u16 heapFlags = parentHeap->dameFlag;
        parentHeap->dameFlag &= ~1;
        TextureHolder *texture = reinterpret_cast<TextureHolder *>(textureStorage[i]);
        textureCtor(texture, kTextureSize, kTextureSize, kTextureFormat, parentHeap);
        parentHeap->dameFlag = heapFlags;
        if (texture->texture == nullptr)
            return false;
        textures[i] = texture;
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

// As RR's LoadPreview builds a player's own model: a heap of its own on SkinModelHeap's, the skin's BRRES or,
// for slot 0, the menu archive's stock one, trimmed after.
bool ExtPreviewLoad(u32 player, CharacterId characterId, u32 slot) {
    const u32 i = player - kGameLocal;
    const u32 character = static_cast<u32>(characterId);
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (i >= kExtPlayers || character >= Pulsar::Driver::CHARACTER_COUNT || modelMgr == nullptr || modelMgr->driverModels == nullptr)
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
        if (entryNum < 0 || !DVD::FastOpen(entryNum, &fileInfo))
            return false;
        fileSize = fileInfo.length;
        DVD::Close(&fileInfo);
    }

    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    if (scene == nullptr || scnMgr == nullptr || scene != previewScene)
        return false;
    u32 reserve = 0xe1000;
    EGG::Heap *parentHeap = SkinModelHeap(*scene, scene->structsHeaps.heaps[0], reserve);
    const u16 heapFlags = parentHeap->dameFlag;
    parentHeap->dameFlag &= ~1;
    const u32 largest = parentHeap->getAllocatableSize(4);
    EGG::ExpHeap *heap = EGG::ExpHeap::Create(fileSize + reserve, parentHeap, 0);
    parentHeap->dameFlag = heapFlags;
    if (heap == nullptr) {
        OS::Report("ss8 preview: player %u %#x slot %u file %u no heap, largest %u\n", player + 1, character, slot, fileSize, largest);
        return false;
    }

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
    const u32 kept = heap->adjust();
    OS::Report("ss8 preview: player %u %#x slot %u file %u kept %u, largest before %u\n", player + 1, character, slot, fileSize, kept, largest);

    DropPreview(preview, true);
    preview.storage[0] = *reinterpret_cast<const u32 *>(modelMgr->driverModels->models);
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

void ExtPreviewPose(u32 player, bool confirmed) {
    const u32 i = player - kGameLocal;
    if (i < kExtPlayers && Own(previews[i])->model != nullptr)
        switchState(Own(previews[i]), -1, confirmed ? MenuDriverModel::MENUDRIVERMODEL_STATE_ONCHARSELECT : MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
}

bool ExtPreviewIdle(u32 player) {
    const u32 i = player - kGameLocal;
    return i < kExtPlayers && Own(previews[i])->model != nullptr && Own(previews[i])->state == MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE;
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
