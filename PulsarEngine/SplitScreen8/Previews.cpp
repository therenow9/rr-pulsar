#ifdef SS8_PAGES
#include <kamek.hpp>
#include <core/egg/3D/Scn.hpp>
#include <core/egg/DVD/DvdRipper.hpp>
#include <core/egg/Thread.hpp>
#include <core/egg/mem/ExpHeap.hpp>
#include <core/rvl/OS/OS.hpp>
#include <core/rvl/dvd/dvd.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuDriverModel.hpp>
#include <MarioKartWii/3D/Model/Menu/MenuKartModel.hpp>
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

// The game's preview path is 4 wide (MenuModelMgr's heaps and textures, MenuDriverModelMgr::players,
// ModelRenderer::params), so P5-8 have their own models and textures and DrawModels runs four more passes.

namespace Pulsar {
// RR's Mii body loader (Extra/MiiOutfitC.cpp): loads onto ScnMgr's current heap, which callers swap first.
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
typedef void (*SwitchStateFn)(MenuDriverModel *, s32, u32);
static const TextureCtorFn textureCtor = reinterpret_cast<TextureCtorFn>(0x80563ce4);
static const ModelMgrDrawFn modelMgrDraw = reinterpret_cast<ModelMgrDrawFn>(0x8059e4fc);
static const DriverTextureFn driverTexture = reinterpret_cast<DriverTextureFn>(0x8059e634);
static const KartModelsFn kartModelsDraw = reinterpret_cast<KartModelsFn>(0x808331c4);
// SwitchState passes its player to MenuKartModelMgr::ToggleVisible, which writes a 4-wide array, only when it is
// not negative (cmpwi r28, 0 at 0x8082FBEC), so P5-8 pass -1.
static const SwitchStateFn switchState = reinterpret_cast<SwitchStateFn>(0x8082fb78);
// A menu Mii head is built once a section (0x80830474) and then only re-attached (0x807DC2D8) and given its Mii:
// building one allocates outside its own heap and only the section's end frees that, so a head is never rebuilt.
typedef MiiHeadsModel *(*HeadCtorFn)(void *, u32, ModelDirector *, u32, Mii *, u32);
typedef void (*HeadParamsFn)(MiiHeadsModel *, u32, ModelDirector *, u32, Mii *);
static const HeadCtorFn headCtor = reinterpret_cast<HeadCtorFn>(0x807dbd80);
static const HeadParamsFn headAttach = reinterpret_cast<HeadParamsFn>(0x807dc2d8);
static const HeadParamsFn headParams = reinterpret_cast<HeadParamsFn>(0x807dc2c0);
const u32 kHeadSize = 0x70;
const u32 kHeadLayer = 5;
const float kMiiBodyScale = 1.6f;  // the Mii body loader's SetScale (0x8083129C, 0x808AC980)

// Raw storage given the shared models' vtable word and set up field by field, not MenuDriverModel's constructor.
struct ExtPreview {
    u32 storage[sizeof(MenuDriverModel) / sizeof(u32)];
    EGG::ExpHeap *heap;
    ToadetteHair *hair;
    MiiHeadsModel *head;  // the player's head while it sits on this model, a Mii body; not owned
    u8 slot;
};
static ExtPreview previews[kExtPlayers];
// Each player's model gets a heap of its own in a fixed arena, emptied before the next model is built so a load
// never meets a fragmented heap. Sized for RR's heaviest skin, Rosalina's 6th (479,828 bytes with its model).
const u32 kArenaSize = 0x78000;
// A model that runs out of room while it is built writes through null inside the game's code, so a skin whose
// file plus this allowance will not fit the arena gets the stock model before anything is built.
const u32 kModelAllowance = 0x26000;
static EGG::ExpHeap *arenas[kExtPlayers];
static MiiHeadsModel *heads[kExtPlayers];
static EGG::ExpHeap *headHeaps[kExtPlayers];
// A head keeps about 0x200 bytes on its own heap, but its models' 132,672-byte block comes from the scene's MEM2
// structs heap, so heads are built as the page opens, before any load fragments it.
const u32 kHeadHeapSize = 0x1000;
static u32 textureStorage[kExtPlayers][sizeof(TextureHolder) / sizeof(u32)];
static TextureHolder *textures[kExtPlayers];
static const GameScene *previewScene;
// The pass count DrawModels' loop takes while the page shows P5-8 (DrawPassCount), else 0.
u8 extPassCount;

static MenuDriverModel *Own(ExtPreview &preview) {
    return reinterpret_cast<MenuDriverModel *>(preview.storage);
}

static void ResetExtKarts(bool destroy);
static void ForgetKartDriver(u32 i);

static void RemoveModel(ModelDirector *model) {
    if (model == nullptr)
        return;
    ScnMgr *scnMgr = ScnMgr::sInstance[model->scnMgrIdx];
    if ((model->bitfield & 0x100000) && scnMgr != nullptr) {
        model->ToggleVisible(false);
        scnMgr->RemoveModelDirector(model);
    }
}

// The head leaves the scene before the body's heap goes. The kart forgets its driver: a rebuilt one lands at the
// same address in the emptied arena, without on-kart animations.
static void DropPreview(ExtPreview &preview, bool destroy) {
    if (destroy && Own(preview)->model != nullptr) {
        if (preview.head != nullptr)
            preview.head->ToggleVisible(false);
        RemoveModel(preview.hair);
        RemoveModel(Own(preview)->model);
        preview.heap->destroy();
    }
    ForgetKartDriver(static_cast<u32>(&preview - previews));
    memset(&preview, 0, sizeof(preview));
}

// GameScene::Reset resets the ScnMgr, then reinit frees the structsHeaps holding the models and textures
// (0x8051B7D4): models leave the ScnMgr here only while their scene is current, and textures are only forgotten.
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
    ResetExtKarts(destroy);
    previewScene = nullptr;
    extPassCount = 0;
}

// Built once a section on a scene structs heap, before any load fragments it: the textures as
// MenuModelMgr::Init builds its own (0x8059E460), then each player's arena, then each Mii player's head.
bool ExtPreviewsShow(u32 count) {
    GameScene *scene = const_cast<GameScene *>(GameScene::GetCurrent());
    if (scene == nullptr)
        return false;
    if (previewScene != scene)
        ResetExtPreviews();
    previewScene = scene;
    EGG::Heap *parentHeap = scene->structsHeaps.heaps[1];
    u32 needed = 0x1000;
    for (u32 i = 0; i < kExtPlayers; ++i)
        if (textures[i] == nullptr)
            needed += kTextureSize * kTextureSize * 2;  // format 5, RGB5A3: 16 bits a texel
    for (u32 i = 0; i + kGameLocal < count; ++i)
        if (arenas[i] == nullptr)
            needed += kArenaSize;
    // The next-course section (0x4B) leaves MEM2 too little for eight players' set; its MEM1 holds it there.
    if (parentHeap->getAllocatableSize(4) < needed)
        parentHeap = scene->structsHeaps.heaps[0];
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

// Without this block free, a head's InitModel writes through a null ScnRfl (0x802443DC), a fault on a console.
// HeadBuilt is a Dolphin-only backstop: a refused head has already written through null and leaks its ScnRfl.
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

// Built once on its own heap, made current so the head's objects land there, then only moved. MiiManager's byte
// at +0x265 is raised around head work, as SetPlayerCharacter does (0x80830E24, 0x80831028).
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
        head->SetExpression(0);
    }
    miiBusy = oldBusy;
    return head;
}

// Built as RR's LoadDriverBRRES builds a player's model, on a heap filling the emptied arena; a Mii is its outfit's
// body (0x80831100) with the player's head. A failed skin leaves the stock model and returns false.
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

// Four more whole kart archives do not fit, so P5-8 keep only the shown kart: archives unpack one at a time into
// the course prefetch heap, idle in section 0x54, and the kart's BRRES and driver_anim.brres are copied out.
const u32 kPrefetchHeap = 0x59c;  // AsyncCourseArchiveLoader::heap in ArchiveMgr (0x80541A70)
const u32 kUnpackSize = 0xe1000;  // a game player's kart heap (0x80542324); RR's largest archive unpacks to 891,680
// The largest VS kart BRRES (114,944), driver_anim.brres (21,376 at most), and the built models and animations.
const u32 kKartHeapSize = 0x30000;
const u32 kUnpackHolder = kGameLocal;  // ArchiveMgr keeps a race's 12 kart archives; the menus load only 0-3
// LinkDriverAnim (0x8081EAC4) takes driver_anim.brres from this holder's archive, and when it has none (in the
// menus) links the BRRES already in its handle (0x8081EB0C skips the store), which is the player's copy.
const u32 kNoArchiveHolder = kGameLocal + 1;
// MenuKartModel as MenuKartModelMgr::Load allocates it (12 * 0x2C, 0x80832E08); its constructor is not linkable,
// so the object is raw storage with its vtable word. Offsets as MenuKartModel::Load writes them (0x80831FF0-0x80832084).
const u32 kKartModelSize = 0x2c;
const u32 kKartModelVtable = 0x808d8c8c;
const u32 kKartId = 0x8, kKartCharacter = 0xc, kKartAllocator = 0x10, kKartDriver = 0x14, kKartDisplay = 0x1c;
const u32 kDriverKart = 0x14;  // MenuDriverModel's kart, as VehicleModelControl::OnUpdate sets it (0x805F3F30)

typedef void (*SleepTicksFn)(s64);
static const SleepTicksFn sleepTicks = reinterpret_cast<SleepTicksFn>(0x801aaca8);

struct ExtKart {
    u32 storage[kKartModelSize / sizeof(u32)];
    EGG::ExpHeap *heap;
    ModelDirector *driverModel;  // the driver model whose on-kart animations are on heap
    CharacterId character;  // what is built, while the kart model is
    u32 slot;
    KartId kart;
    CharacterId wantCharacter;
    u32 wantSlot;
    KartId wantKart;
    bool wanted;
    bool failed;  // the wanted kart could not be built; tried again once the want changes
    bool shown;
    bool chosen;
};
static ExtKart karts[kExtPlayers];
static EGG::ExpHeap *unpackHeap;
static EGG::ExpHeap *orphanHeap;  // an area whose unpack outlived its section, freed at the next reset
enum UnpackState {
    UNPACK_IDLE,
    UNPACK_BUSY,  // the load thread owns the area
    UNPACK_DONE
};
static volatile u32 unpackState;
static CharacterId unpackCharacter;
static u32 unpackSlot;
static char unpackPath[0x80];

static MenuModel *KartModel(ExtKart &kart) {
    return reinterpret_cast<MenuModel *>(kart.storage);
}

template <typename T>
static T &KartField(ExtKart &kart, u32 offset) {
    return *reinterpret_cast<T *>(reinterpret_cast<u8 *>(kart.storage) + offset);
}

static EGG::ExpHeap *PrefetchHeap() {
    const u8 *archives = reinterpret_cast<const u8 *>(ArchiveMgr::sInstance);
    return archives != nullptr ? *reinterpret_cast<EGG::ExpHeap *const *>(archives + kPrefetchHeap) : static_cast<EGG::ExpHeap *>(nullptr);
}

static bool KartCurrent(ExtKart &kart, u32 i) {
    return KartModel(kart)->model != nullptr && kart.driverModel == Own(previews[i])->model && kart.character == kart.wantCharacter && kart.slot == kart.wantSlot && kart.kart == kart.wantKart;
}

// Both kart poses use the on-kart transformator (SwitchState, 0x8082FC60).
static bool OnKart(const MenuDriverModel *driver) {
    return driver->state == MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT || driver->state == kStateKartChosen;
}

// The driver leaves its on-kart animations, which are on the kart's heap, before that heap is freed.
static void DropKart(u32 i) {
    ExtKart &kart = karts[i];
    MenuDriverModel *driver = Own(previews[i]);
    if (driver->model != nullptr && driver->model == kart.driverModel && driver->onKartTransformator != nullptr) {
        if (OnKart(driver))
            switchState(driver, -1, MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
        driver->onKartTransformator = nullptr;
    }
    RemoveModel(KartModel(kart)->model);
    memset(kart.storage, 0, sizeof(kart.storage));
    kart.driverModel = nullptr;
    if (kart.heap != nullptr)
        kart.heap->freeAll();
}

static void ForgetKartDriver(u32 i) {
    karts[i].driverModel = nullptr;
}

bool ExtKartsShow(u32 count) {
    EGG::ExpHeap *prefetch = PrefetchHeap();
    if (prefetch == nullptr)
        return false;
    const u16 heapFlags = prefetch->dameFlag;
    prefetch->dameFlag &= ~1;
    if (unpackHeap == nullptr)
        unpackHeap = EGG::ExpHeap::Create(kUnpackSize, prefetch, 0);
    bool built = unpackHeap != nullptr;
    for (u32 i = 0; i + kGameLocal < count && built; ++i) {
        if (karts[i].heap == nullptr)
            karts[i].heap = EGG::ExpHeap::Create(kKartHeapSize, prefetch, 0);
        built = karts[i].heap != nullptr;
        karts[i].shown = true;
        karts[i].chosen = false;
    }
    prefetch->dameFlag = heapFlags;
    if (!built)
        OS::Report("ss8 preview: kart heaps failed, prefetch largest %u\n", prefetch->getAllocatableSize(4));
    return built;
}

void ExtKartsHide() {
    for (u32 i = 0; i < kExtPlayers; ++i) {
        karts[i].shown = false;
        MenuDriverModel *driver = Own(previews[i]);
        if (driver->model != nullptr && OnKart(driver))
            switchState(driver, -1, MenuDriverModel::MENUDRIVERMODEL_STATE_IDLE);
    }
}

void ExtKartRequest(u32 player, CharacterId character, u32 slot, KartId kartId) {
    const u32 i = player - kGameLocal;
    if (i >= kExtPlayers)
        return;
    ExtKart &kart = karts[i];
    if (kart.wanted && kart.wantCharacter == character && kart.wantSlot == slot && kart.wantKart == kartId)
        return;
    kart.wantCharacter = character;
    kart.wantSlot = slot;
    kart.wantKart = kartId;
    kart.wanted = true;
    kart.failed = false;
}

void ExtKartPose(u32 player, bool confirmed) {
    const u32 i = player - kGameLocal;
    if (i < kExtPlayers)
        karts[i].chosen = confirmed;
}

// On the game's load thread, as LoadKartArchiveAsync (0x80541E44) loads a game player's: the old archive out, then
// the new one through the game's kart dump heap, which that thread's loads use one at a time.
static void UnpackKartArchive(void *area) {
    EGG::ExpHeap *heap = static_cast<EGG::ExpHeap *>(area);
    ArchiveMgr *archives = ArchiveMgr::sInstance;
    ArchivesHolder *holder = &archives->kartModelsHolders[kUnpackHolder];
    if (holder->HasArchives())
        holder->UnmountArchives();
    heap->freeAll();
    holder->LoadArchives(unpackPath, heap, archives->allkartModelDumpHeap, nullptr);
    unpackState = UNPACK_DONE;
}

// Queued behind an unpack its section did not outlive, so the race's LoadKartArchive, which skips a holder that
// has archives (0x80540E88), loads player 5's own.
static void ReleaseUnpackHolder(void *) {
    ArchivesHolder *holder = &ArchiveMgr::sInstance->kartModelsHolders[kUnpackHolder];
    if (holder->HasArchives())
        holder->UnmountArchives();
}

// The path as RR's LoadKartArchive makes it (Driver/CustomCharacters.cpp), always VS's: 5-8 players have no Battle.
static bool RequestUnpack(CharacterId character, u32 slot) {
    ArchiveMgr *archives = ArchiveMgr::sInstance;
    if (archives == nullptr || archives->allkartModelDumpHeap == nullptr)
        return false;
    const char *name = ArchiveMgr::GetKartArchivePostfix(character);
    char skinPath[0x80];
    snprintf(skinPath, sizeof(skinPath), "/Scene/Model/Kart/%s-%u-allkart.szs", name, slot);
    if (slot != 0 && Pulsar::IOOverrides::ConvertPathToEntryNumWithLooseOverride(skinPath) >= 0)
        snprintf(unpackPath, sizeof(unpackPath), "Scene/Model/Kart/%s-%u-allkart", name, slot);
    else
        snprintf(unpackPath, sizeof(unpackPath), "Scene/Model/Kart/%s-allkart", name);
    unpackCharacter = character;
    unpackSlot = slot;
    unpackState = UNPACK_BUSY;
    // Refused while the manager shuts down, else queued on the load thread, which runs tasks in order;
    // ProcessRequestsAndShutdown (GameScene::Reset+0x30) waits until it has none left.
    if (!archives->RequestTask(UnpackKartArchive, unpackHeap, 0x10000003)) {  // RequestLoadKartArchives' value (0x805422B0)
        unpackState = UNPACK_IDLE;
        return false;
    }
    OS::Report("ss8 preview: kart archive %s\n", unpackPath);
    return true;
}

// Built as MenuKartModel::Load builds a multiplayer kart (0x80831FC4: two ModelDirectors, the display one never
// loaded), with the driver's on-kart animations linked as PrepareDriverOnKartAnms does (0x80830C64).
static bool BuildKart(u32 i) {
    ExtKart &kart = karts[i];
    MenuDriverModel *driver = Own(previews[i]);
    ArchiveMgr *archives = ArchiveMgr::sInstance;
    ScnMgr *scnMgr = ScnMgr::sInstance[0];
    MenuModelMgr *modelMgr = MenuModelMgr::sInstance;
    if (driver->model == nullptr || scnMgr == nullptr || modelMgr == nullptr || modelMgr->kartModels == nullptr)
        return false;
    DropKart(i);
    char name[0x20];
    snprintf(name, sizeof(name), "%s.brres", ArchiveMgr::GetKartArchivePrefix(kart.wantKart));
    const ArchivesHolder &holder = archives->kartModelsHolders[kUnpackHolder];
    u32 kartSize = 0;
    u32 animSize = 0;
    const void *kartFile = holder.GetFile(name, &kartSize);
    const void *animFile = holder.GetFile("driver_anim.brres", &animSize);
    EGG::ExpHeap *heap = kart.heap;
    void *kartCopy = kartFile != nullptr ? EGG::Heap::alloc(kartSize, 0x20, heap) : nullptr;
    void *animCopy = animFile != nullptr ? EGG::Heap::alloc(animSize, 0x20, heap) : nullptr;
    if (kartCopy == nullptr || animCopy == nullptr || archives->kartModelsHolders[kNoArchiveHolder].HasArchives()) {
        OS::Report("ss8 preview: player %u kart %#x %s failed, file %u anim %u\n", kGameLocal + i + 1, kart.wantKart, name, kartSize, animSize);
        heap->freeAll();
        return false;
    }
    memcpy(kartCopy, kartFile, kartSize);
    memcpy(animCopy, animFile, animSize);

    EGG::Allocator *allocator = new (heap) EGG::Allocator(heap, 0x20);
    EGG::Heap *oldHeap = scnMgr->curHeap;
    EGG::Allocator *oldAllocator = scnMgr->curAllocator;
    EGG::Allocator *oldMenuAllocator = menuAllocator;
    scnMgr->curHeap = heap;
    scnMgr->curAllocator = allocator;
    menuAllocator = allocator;
    EGG::Heap *oldCurrentHeap = heap->BecomeCurrentHeap();

    kart.storage[0] = kKartModelVtable;
    MenuModel *model = KartModel(kart);
    KartField<KartId>(kart, kKartId) = kart.wantKart;
    KartField<CharacterId>(kart, kKartCharacter) = kart.wantCharacter;
    KartField<EGG::Allocator *>(kart, kKartAllocator) = allocator;
    KartField<MenuDriverModel *>(kart, kKartDriver) = driver;
    model->model = new (heap) ModelDirector(2, 0);
    KartField<ModelDirector *>(kart, kKartDisplay) = new (heap) ModelDirector(4, 0);
    MenuModelBRRESHandle kartHandle;
    kartHandle.menuModelBRRES.data = static_cast<g3d::ResFileData *>(kartCopy);
    ModelDirector::BindBRRESImpl(kartHandle.menuModelBRRES, name, nullptr, 0);
    const bool loaded = kartHandle.LoadKartModel(*model->model, modelMgr->kartModels->players[0].team);

    ModelTransformator *onKart = loaded ? driver->model->CreateAndBindTransformator() : static_cast<ModelTransformator *>(nullptr);
    if (onKart != nullptr) {
        driver->model->ChangeTransformator(onKart, false);
        MenuModelBRRESHandle animHandle;
        animHandle.menuModelBRRES.data = static_cast<g3d::ResFileData *>(animCopy);
        animHandle.menuModelBRRES.Init();
        animHandle.LinkDriverAnim(kNoArchiveHolder, *driver->model, static_cast<CharacterId>(driver->id));
        driver->model->ChangeTransformator(driver->charSelTransformator, false);
        driver->onKartTransformator = onKart;
    }

    oldCurrentHeap->BecomeCurrentHeap();
    scnMgr->curHeap = oldHeap;
    scnMgr->curAllocator = oldAllocator;
    menuAllocator = oldMenuAllocator;
    if (onKart == nullptr) {
        OS::Report("ss8 preview: player %u kart %#x %s not built\n", kGameLocal + i + 1, kart.wantKart, name);
        RemoveModel(model->model);
        memset(kart.storage, 0, sizeof(kart.storage));
        heap->freeAll();
        return false;
    }
    model->Init();
    kart.driverModel = driver->model;
    kart.character = kart.wantCharacter;
    kart.slot = kart.wantSlot;
    kart.kart = kart.wantKart;
    OS::Report("ss8 preview: player %u kart %#x %s file %u anim %u heap free %u\n", kGameLocal + i + 1, kart.kart, name, kartSize, animSize, TotalFree(heap));
    return true;
}

// Builds every wanted kart whose archive is unpacked, else queues the next unpack. A built kart puts its driver in
// the kart pose as VehicleModelControl::OnUpdate does (0x805F3F3C), with -1 for SwitchState's player.
static void ServiceKarts() {
    if (unpackHeap == nullptr || unpackState == UNPACK_BUSY)
        return;
    for (u32 i = 0; i < kExtPlayers; ++i) {
        ExtKart &kart = karts[i];
        if (!kart.shown || !kart.wanted || kart.failed || kart.heap == nullptr || Own(previews[i])->model == nullptr || KartCurrent(kart, i))
            continue;
        if (unpackState == UNPACK_DONE && unpackCharacter == kart.wantCharacter && unpackSlot == kart.wantSlot)
            kart.failed = !BuildKart(i);
        else if (RequestUnpack(kart.wantCharacter, kart.wantSlot))
            return;
    }
    for (u32 i = 0; i < kExtPlayers; ++i) {
        ExtKart &kart = karts[i];
        MenuDriverModel *driver = Own(previews[i]);
        const u32 pose = kart.chosen ? kStateKartChosen : MenuDriverModel::MENUDRIVERMODEL_STATE_ONKARTSELECT;
        if (kart.shown && KartCurrent(kart, i) && driver->state != pose) {
            *reinterpret_cast<KartId *>(reinterpret_cast<u8 *>(driver) + kDriverKart) = kart.kart;
            switchState(driver, -1, pose);
        }
    }
}

// MenuKartModel::Update turns a kart and seats its driver only for a kart a game player has selected (0x808327A0),
// so it runs with this kart lent player 1's selection, which is put back at once.
static void UpdateKart(ExtKart &kart, MenuKartModelMgr *kartModels) {
    MenuKartModelMgr::Player &first = kartModels->players[0];
    MenuKartModel *const oldModels = first.modelsArray;
    const u32 oldIdx = first.idx;
    const bool oldLocked = first.isLocked;
    first.modelsArray = reinterpret_cast<MenuKartModel *>(kart.storage);
    first.idx = 0;
    first.isLocked = true;
    KartModel(kart)->Update();
    first.modelsArray = oldModels;
    first.idx = oldIdx;
    first.isLocked = oldLocked;
}

// The area goes once the load thread's last unpack has ended. One still running after two seconds gets the archive's
// removal queued behind it, and its area is freed at the next reset; models leave the scene only while it is theirs.
static void ResetExtKarts(bool destroy) {
    for (u32 wait = 0; unpackState == UNPACK_BUSY && wait < 2000; ++wait) sleepTicks(static_cast<s64>(*reinterpret_cast<const u32 *>(0x800000f8) / 4 / 1000));  // a millisecond (OSMillisecondsToTicks)
    ArchiveMgr *archives = ArchiveMgr::sInstance;
    if (unpackState == UNPACK_BUSY && unpackHeap != nullptr) {
        const bool queued = archives->RequestTask(ReleaseUnpackHolder, nullptr, 0x10000003);
        OS::Report("ss8 preview: kart archive still loading, release %s\n", queued ? "queued" : "not queued");
        orphanHeap = unpackHeap;
        unpackHeap = nullptr;
    } else if (unpackState != UNPACK_BUSY) {
        if (orphanHeap != nullptr)
            orphanHeap->destroy();
        orphanHeap = nullptr;
        if (unpackHeap != nullptr && archives != nullptr && archives->kartModelsHolders[kUnpackHolder].HasArchives())
            archives->kartModelsHolders[kUnpackHolder].UnmountArchives();
        if (unpackHeap != nullptr)
            unpackHeap->destroy();
        unpackHeap = nullptr;
        unpackState = UNPACK_IDLE;
    }
    for (u32 i = 0; i < kExtPlayers; ++i) {
        ExtKart &kart = karts[i];
        if (destroy)
            RemoveModel(KartModel(kart)->model);
        if (kart.heap != nullptr)
            kart.heap->destroy();
        memset(&kart, 0, sizeof(kart));
    }
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
        preview.hair->ToggleTransparent(shown);
    if (preview.head != nullptr)
        preview.head->ToggleTransparent(shown);  // as MenuDriverModelMgr::Draw shows a player's Mii head (0x80830BA8)
}

// As MenuKartModelMgr::Draw shows a player's selected kart in their pass only (0x808331C4).
static void SetKartShown(u32 i, bool shown) {
    ExtKart &kart = karts[i];
    if (KartModel(kart)->model != nullptr && kart.driverModel == Own(previews[i])->model)
        KartModel(kart)->ToggleTransparent(shown && kart.shown);
}

// DrawModels' "bl MenuModelMgr::Draw" (0x8059F1A0, r4 = the pass). P5-8's pass hides the game's drivers by clearing
// player 1's visible byte (head and hair follow it) and every kart by asking for a player none has (compared, not indexed).
static void DrawPass(MenuModelMgr *modelMgr, u32 passArg) {
    const u32 pass = passArg & 0xff;
    if (extPassCount == 0 || pass < kGameLocal || modelMgr->driverModels == nullptr) {
        modelMgrDraw(modelMgr, pass);
        for (u32 i = 0; extPassCount != 0 && i < kExtPlayers; ++i) SetPreviewShown(previews[i], false);
        for (u32 i = 0; extPassCount != 0 && i < kExtPlayers; ++i) SetKartShown(i, false);
        return;
    }
    MenuDriverModelMgr::Player &first = modelMgr->driverModels->players[0];
    const bool firstVisible = first.isVisible;
    first.isVisible = false;
    modelMgrDraw(modelMgr, 0);
    first.isVisible = firstVisible;
    kartModelsDraw(modelMgr->kartModels, kGameLocal);
    for (u32 i = 0; i < kExtPlayers; ++i) SetPreviewShown(previews[i], i == pass - kGameLocal);
    for (u32 i = 0; i < kExtPlayers; ++i) SetKartShown(i, i == pass - kGameLocal);
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
    hair->ToggleLock(lock);
}

// MenuModelMgr::Update's "bl MenuKartModelMgr::Update" (0x8059E4D4, 8 bytes after RR's hook; only r31 live after;
// GlobeScene::OnCalc reaches it too). P5-8's models show only while the page does, each driver before its kart.
static void UpdateModels(MenuKartModelMgr *kartModels) {
    kartModels->Update();
    ServiceKarts();
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
        ExtKart &kart = karts[i];
        if (KartModel(kart)->model != nullptr && kart.driverModel == own->model) {
            UpdateKart(kart, kartModels);
            KartModel(kart)->ToggleVisible(shown && kart.shown);
        }
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
