#include <kamek.hpp>
#include <CustomCharacters/CustomCharacters.hpp>
#include <SplitScreen8/SplitScreen8.hpp>

// D77: two locals on one voice group with different voice sets each hear their own. The game loads a
// group once, so a second set is put in the PC voice group of a character no local uses whose voices
// have the same layout, and RR's voice borrowing plays it. docs/plans/m4-menu-flow.md, C2b.

// Retro Rewind's CustomCharacters function this file calls. It is not in its header; a changed
// signature fails the link.
namespace Pulsar {
namespace CustomCharacters {
bool VoiceBaseGroupForTable(CharacterId character, u8 table, u32 &groupId);
}  // namespace CustomCharacters
}  // namespace Pulsar

namespace SplitScreen8 {

namespace CC = Pulsar::CustomCharacters;

const u32 kNoGroup = 0xffffffff;  // RR's SILENT_VOICE_GROUP, and no group

// PC voice groups whose sounds use the same wave entries at the same indices in revo_kart.brsar
// (78 and 75 entries), as do their cannon and goal groups, so one character's voice files play right
// from another's groups. Every other character's layout is its own: two locals sharing one of those
// keep RR's first-player voices.
static const u32 kFamilyA[] = {
    BRSAR_GROUP_MARIO,
    BRSAR_GROUP_LUIGI,
    BRSAR_GROUP_DAISY,
    BRSAR_GROUP_BOWSER_JR,
    BRSAR_GROUP_DIDDY_KONG,
    BRSAR_GROUP_BABY_LUIGI,
    BRSAR_GROUP_BABY_DAISY,
    BRSAR_GROUP_TOADETTE,
    BRSAR_GROUP_WARIO,
    BRSAR_GROUP_WALUIGI,
    BRSAR_GROUP_BOWSER,
    BRSAR_GROUP_DONKEY_KONG,
};
static const u32 kFamilyB[] = {BRSAR_GROUP_PEACH, BRSAR_GROUP_BABY_PEACH};

static bool InFamily(const u32 *family, u32 count, u32 group) {
    for (u32 i = 0; i < count; ++i) {
        if (family[i] == group) return true;
    }
    return false;
}

static bool Holds(const u32 *groups, u32 count, u32 group) {
    for (u32 i = 0; i < count; ++i) {
        if (groups[i] == group) return true;
    }
    return false;
}

// The first group of base's family that no local has as its base and no earlier local was lent;
// base itself when there is none.
static u32 Lend(u32 base, const u32 *bases, u32 localCount, const u32 *groups, u32 earlier) {
    const u32 *family = kFamilyA;
    u32 size = sizeof(kFamilyA) / sizeof(kFamilyA[0]);
    if (!InFamily(family, size, base)) {
        family = kFamilyB;
        size = sizeof(kFamilyB) / sizeof(kFamilyB[0]);
        if (!InFamily(family, size, base)) return base;
    }
    for (u32 i = 0; i < size; ++i) {
        if (!Holds(bases, localCount, family[i]) && !Holds(groups, earlier, family[i])) return family[i];
    }
    return base;
}

u32 LocalVoiceGroup(u8 playerId, u32 baseGroup) {
    const Racedata *racedata = Racedata::sInstance;
    if (racedata == nullptr || CC::IsOnlineRoom(RKNet::Controller::sInstance) || !CC::IsLocalMultiplayer()) return baseGroup;
    const RacedataScenario &scenario = racedata->racesScenario;
    u32 count = scenario.localPlayerCount;
    if (count > kGameLocal) count = kGameLocal;
    if (count < 2) return baseGroup;

    // Each local's base group (RR's choice, after its own voice borrowing) and voice set: its skin's
    // loose files, or none for the base character's own voices.
    u32 bases[kGameLocal];
    const char *sets[kGameLocal];
    u8 ids[kGameLocal];
    for (u32 h = 0; h < count; ++h) {
        ids[h] = racedata->GetPlayerIdOfLocalPlayer(h);
        bases[h] = kNoGroup;
        sets[h] = nullptr;
        if (ids[h] >= scenario.playerCount) continue;
        const CharacterId character = scenario.players[ids[h]].characterId;
        if (!CC::IsCharacter(character) || CC::IsMiiCharacter(character)) continue;
        const u8 table = CC::RaceSkinTable(ids[h], character);
        u32 base = kNoGroup;
        if (!CC::VoiceBaseGroupForTable(character, table, base)) continue;
        bases[h] = base;
        if (CC::GetLooseVoiceInfo(character, table).hasFiles) sets[h] = CC::GeneratedCustomPostfix(character, table);
    }

    // A group stays with its base character's own voices when a local uses them, else with the first
    // local's set; each other set takes a lent group, shared by the locals with that set.
    u32 groups[kGameLocal];
    for (u32 h = 0; h < count; ++h) {
        groups[h] = bases[h];
        if (bases[h] == kNoGroup) continue;
        const char *keeper = nullptr;
        bool ownVoices = false;
        for (u32 k = count; k-- > 0;) {
            if (bases[k] != bases[h]) continue;
            ownVoices |= sets[k] == nullptr;
            keeper = sets[k];
        }
        if (ownVoices) keeper = nullptr;
        if (sets[h] == keeper) continue;
        bool shared = false;
        for (u32 k = 0; k < h && !shared; ++k) {
            if (bases[k] == bases[h] && sets[k] == sets[h]) {
                groups[h] = groups[k];
                shared = true;
            }
        }
        if (!shared) groups[h] = Lend(bases[h], bases, count, groups, h);
    }

    for (u32 h = 0; h < count; ++h) {
        if (ids[h] == playerId && bases[h] == baseGroup) return groups[h];
    }
    return baseGroup;
}

}  // namespace SplitScreen8
