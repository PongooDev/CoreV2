#pragma once

#include "Containers/UnrealString.h"
#include "GameFramework/CheatManager.h"
#include "UObject/NameTypes.h"
#include "UObject/Stack.h"

#include <cstdint>
#include <initializer_list>
#include <string>

class ACharacter;
class AFortPlayerController;
class UFortItemDefinition;
class UFortWorldItemDefinition;
struct FGuid;
enum class ECheatMovement : uint8_t;

// Fortnite's cheats ("cheat <command>" runs them on the server, AFortPlayerController::ServerCheat_Implementation).
// Shipping builds keep every exec function but compile many bodies out, so those cheats answered nothing and did
// nothing (#101). Core gives the stripped ones the server's body back; Core-Docs Cheats.md lists every command.
class UFortCheatManager : public UCheatManager
{
    DefineUnrealClass(UFortCheatManager, FortniteGame)
    DefineUFunction(GiveWeapon)
    DefineUFunction(GiveAmmo)
    DefineUFunction(GiveTrap)
    DefineUFunction(GiveGadget)
    DefineUFunction(GiveGadgets)
    DefineUFunction(GiveSpecificItem)
    DefineUFunction(GiveResources)
    DefineUFunction(GiveWood)
    DefineUFunction(GiveStone)
    DefineUFunction(GiveMetal)
    DefineUFunction(GiveWeapons)
    DefineUFunction(GiveAllMelee)
    DefineUFunction(GiveAllPistols)
    DefineUFunction(GiveAllRifles)
    DefineUFunction(GiveAllShotguns)
    DefineUFunction(GiveAllWeapons)
    DefineUFunction(GiveAllTraps)
    DefineUFunction(GiveAllConsumables)
    DefineUFunction(GiveAllIngredients)
    DefineUFunction(GiveUsefulThings)
    DefineUFunction(GiveCheatInventory)
    DefineUFunction(WannaGun)
    DefineUFunction(TeleportToLocation)
    DefineUFunction(SetSafeZoneLocationToPlayer)
    DefineUFunction(SetSafeZoneRadius)
    DefineUFunction(RespawnPlayerServer)
    DefineUFunction(TestInfJumps)
    DefineUFunction(GiveBluGlo)
    DefineUFunction(SpawnAIGroup)
    DefineUFunction(SpawnAIGroupFromExternalSpawner)
    DefineUFunction(SendMissionEvent)
    DefineUFunction(LoadMission)
    DefineUFunction(TeleportToMissionActor)
    DefineUFunction(DumpAllActiveMissions)
    DefineUFunction(SimulateLoot)
    DefineUFunction(ToggleInvincibleCornerstones)
    DefineUFunction(SetCapturePercentage)
    DefineUFunction(CountPlayerBuiltBuildingActors)

public:
    // ToggleUnlimitedHealth's flag: the player's pawn can't be damaged while its controller's cheat manager has it (1.9
    // 0x7AE300, the pawn's damage check, reads it).
    DefineBitfieldUProperty(bUnlimitedHealth)
    static bool HasUnlimitedHealthFlag() { return Member_bUnlimitedHealth.IsAvailable(); }

    AFortPlayerController* GetPlayerController() const;

    // WeaponName: the item's name with or without its prefix ("Assault_Auto_Athena_R_Ore_T03" or "WID_..."), or part
    // of it. Count weapons at RequestedLevel, a ranged weapon with a full magazine.
    void GiveWeapon(const FString& WeaponName, int32_t RequestedLevel, int32_t Count);
    // "AthenaAmmoDataBulletsHeavy 100"; without an amount, a full stack. Without a name (#155: "giveammo", "giveammo
    // 100"), the ammo the weapon the player holds fires.
    void GiveAmmo(const FString& AmmoNameAndAmount);
    void GiveTrap(const FString& TrapName, int32_t RequestedLevel, int32_t NumTrapsToGive);
    void GiveGadgets(const FString& GadgetName, int32_t Quantity, int32_t ItemLevel);
    // ItemPath: an item's object or package path ("/Game/Items/Ingredients/Ingredient_Planks"), or its name.
    void GiveSpecificItem(const FString& ItemPath, int32_t ItemAmount, int32_t ItemLevel);
    // Wood, stone and metal; without an amount, a full stack of each.
    void GiveResources(int32_t Amount);
    void GiveResource(UFortWorldItemDefinition* Resource, int32_t Amount);
    // One of every weapon with the gameplay tag or one under it (None: every weapon), the best rarity and tier of each.
    void GiveWeapons(FName Tag);
    void GiveAllTraps(int32_t NumItems);
    void GiveAllConsumables(int32_t NumItems);
    void GiveAllIngredients(int32_t NumIngredients);
    void GiveUsefulThings(int32_t NumOfUsefulThings);
    // The game data's CheatInventoryItems (1.7.2 and 1.8: the TestGod rifle).
    void GiveCheatInventory();
    // The WannaGun test gun (/Game/Items/Weapons/Ranged/WIP/WannaGun).
    void WannaGun();
    void TeleportToLocation(float X, float Y, float Z);
    // Battle Royale: the safe zones after the current one close in on the player.
    void SetSafeZoneLocationToPlayer();
    // Battle Royale: the storm circle has this radius until the next phase.
    void SetSafeZoneRadius(float NewRadius);
    void RespawnPlayerServer();
    // The pawn may jump again in the air, any number of times (toggles; until it respawns).
    void TestInfJumps();
    // Save the World's BluGlo (UFortGameData::BluGloItemDefinition).
    void GiveBluGlo(int32_t Amount);
    // Save the World: an AI spawn group (ListSpawnGroups names them) in front of the player, through an active encounter.
    void SpawnAIGroup(const FString& SpawnGroupName, bool bFromExternalSpawner);
    // Save the World: the mission event (UFortMissionLibrary::ExecuteMissionEvent) every mission listens for.
    void SendMissionEvent(const FString& MissionEventName);
    // Save the World: loads the mission (a FortMissionInfo by name, ListMissions names them) into the zone.
    void LoadMission(const FString& MissionName);
    // Save the World: teleports to the first active objective of the current missions, the primary mission's first.
    void TeleportToMissionActor();
    void DumpAllActiveMissions();
    // Rolls the loot tier group once (at LootTier when it is 0 or more, at LootLevel as the world level) and says what dropped.
    void SimulateLoot(FName TierGroup, int32_t LootTier, int32_t LootLevel);
    // Base destruction PvP: every team's cornerstone can't be damaged (toggles).
    void ToggleInvincibleCornerstones();
    // Every capture point's capture progress (0 to 1).
    void SetCapturePercentage(float NewCapturePercentage);
    // The zone's structure count and limit, and the pieces in the world it counts (walls, floors, roofs, stairs, traps,
    // others), so a count that doesn't match what is standing shows (#128, #130).
    void CountPlayerBuiltBuildingActors();

    // Core's own command, "cheat corecheats [filter]": the cheats Core supports, with usage, as chat lines.
    void ListCoreCheats(const FString& Filter);
    // Core's own command, "cheat spawnactor <class or path> [scale | x y z]" (#132): an actor of the class in front of the
    // player, at the scale (uniform, or per axis), replicated to every client.
    void SpawnActor(const FString& Arguments);
    // The actor class a name or path means (SpawnActor's lookup, loading the class when a path names one not loaded).
    static UClass* FindActorClass(const std::wstring& Query);
    // Core's own command, "cheat tpactor <class, name or path> [n]" (#146): the player's pawn goes on top of the nearest
    // (or nth nearest) actor that matches, traced down to what is there so it doesn't land inside it or under the map.
    void TpActor(const FString& Arguments);
    // Core's own command, "cheat nocooldown [on | off]" (#148; "cheat ToggleNoCoolDown" too): the player's hero abilities
    // and gadgets have no cooldown until it is turned off, respawns included. The game's ToggleNoCoolDown only sets a flag
    // nothing in a shipping build reads; Core takes each cooldown effect off as soon as it is applied.
    void NoCooldown(const FString& Arguments);
    // Core's own command, "cheat infiniteammo [on | off]" ("cheat ToggleInfiniteAmmo" too; #155): the game's
    // ToggleInfiniteAmmo flag (bInfiniteAmmo, replicated): reloads never use up the reserve, the magazine still empties.
    // Says whether it is on, where the game's toggle answered nothing the player saw.
    void InfiniteAmmo(const FString& Arguments);
    // Core's own command, "cheat infiniteconsumables [on | off]" ("cheat toggleinfiniteconsumables" too; #155): until it
    // is turned off, respawns included, using a consumable (shield potions, medkits, bandages, grenades, smoke
    // grenades...) doesn't use it up. Says whether it is on.
    void InfiniteConsumables(const FString& Arguments);
    // Server: true when the game's removal of this item (by definition, or by the player's item with that guid) is a
    // consumable's use that infiniteconsumables keeps: the player has it on and the item spends itself when used.
    static bool KeepsConsumable(AFortPlayerController* PlayerController, const UFortItemDefinition* ItemDefinition);
    static bool KeepsConsumable(AFortPlayerController* PlayerController, const FGuid& ItemGuid);
    // Core's own commands from PongooDev/Core (#155): "cheat sethealth <value>", "cheat setshield <value>" (the shield,
    // up to its maximum) and "cheat setmaxshield <value | reset>" (the maximum shield, kept through respawns like
    // maxhealth), Save the World's and Battle Royale's alike.
    void SetHealth(const FString& Arguments);
    void SetShield(const FString& Arguments);
    void SetMaxShield(const FString& Arguments);
    // Core's own command, "cheat clearequippeditem" (#155): the item the player holds leaves their inventory.
    void ClearEquippedItem(const FString& Arguments);
    // Core's own command, "cheat setscale <x> [y z] | reset" (#155): the player's pawn at that scale, collision included,
    // replicated to every client, until it respawns. FortCheatManagerDisguise.cpp.
    void SetScale(const FString& Arguments);
    // Core's own command, "cheat progressquest [quest] [amount | all]" (#155): an open objective of the player's quest
    // (Battle Royale's challenges, Save the World's quests) moves on by the amount, saved to their profile through the
    // quest manager's UpdateQuests; without a quest, the open quests are listed.
    void ProgressQuest(const FString& Arguments);
    // Core's own command, "cheat infinitemag [on | off]" (#149): the player never reloads.
    // The game's own ammo-saving effect (GE_Alteration_Attribute_AmmoSave) at a chance over 1 goes on their ability system,
    // so a shot takes no round on the server or on their client (#155); the server also keeps the magazine full through
    // the weapon's own ModAmmo and sets the game's replicated bInfiniteAmmo (the reserve never runs out), which tells
    // Core's client to keep its own count of the magazine full.
    void InfiniteMag(const FString& Arguments);
    // Core's own command, "cheat unlimitedhealth [on | off]" (#155; "cheat ToggleUnlimitedHealth" too): sets the game's
    // bUnlimitedHealth, as ToggleUnlimitedHealth does, and says whether it is on.
    void UnlimitedHealth(const FString& Arguments);
    // Core's own command, "cheat repair [all]" (#150): the equipped weapon (or every item that wears out) back to its full
    // durability, through the same durability update the game's wear goes through.
    void Repair(const FString& Arguments);
    // Core's own command, "cheat firerate <multiplier | reset>" (#150): the player's guns fire that many times as fast as
    // they do now, through the game's own fire rate alteration effect (GE_Alteration_Attribute_FireRate, WeaponRateOfFire)
    // on their ability system, which replicates; reset takes Core's effects off again.
    void FireRate(const FString& Arguments);
    // Core's own command, "cheat weapondamage <multiplier | reset>" (#152): the player's guns and melee weapons do that
    // many times the damage they do now, through the game's own weapon damage perk effects (GE_Alteration_Attribute_
    // WeaponDamage and its melee one, OutgoingAbilityDamage) on their ability system; reset takes Core's effects off.
    void WeaponDamage(const FString& Arguments);
    // Core's own command, "cheat dance [EID or path]" (#152): the player plays the emote (a random one without a name)
    // through the emote wheel's own ServerPlayEmoteItem, run on the server.
    void Dance(const FString& Arguments);
    // Core's own command, "cheat maxhealth <value | reset>" ("setmaxhealth" too; #155): the pawn's maximum health becomes
    // the value, through its own SetMaxHealth on the health set (replicated: the HUD shows it), and it is healed to it;
    // kept through respawns until reset, which gives back the maximum it had.
    void MaxHealth(const FString& Arguments);
    // Core's own command, "cheat sethealthpercent <percent>" (#155; the game's takes 0-1 and answers nothing): the pawn's
    // health becomes that share of its maximum, Core's maximum health included; 0-1 is a fraction, above 1 a percent.
    void HealthPercent(const FString& Arguments);
    // Core's own command, "cheat toggleinfinitedurability [on | off]" (#155): the game's flag, whose own cheat answers
    // only the server's console.
    void InfiniteDurability(const FString& Arguments);
    // Core's own command, "cheat speed <multiplier | reset>" (#155): the pawn walks, runs and sprints that many times as
    // fast. Save the World's pawn moves at its movement set's speeds, which the server scales and replicates to its
    // client. Battle Royale's moves at its stats row's, which each machine reads on its own: the server applies the
    // game's own move speed alteration (GE_Alteration_Attribute_MoveSpeed, RunSpeed), which reaches the owning client
    // with the ability system, and the server and that client (StartSpeedView) both scale the row by the RunSpeed
    // multiplier it gives.
    void Speed(const FString& Arguments);
    // Client (#155): every so often, the local Battle Royale pawn moves at the RunSpeed multiplier its ability system has.
    static void StartSpeedView();
    // Core's own commands, "cheat teleportto <player>" and "cheat bring <player | all>" (#155): the player's pawn goes
    // next to the named player's, or theirs (everyone's) in front of the player's, on the floor there.
    void TeleportTo(const FString& Arguments);
    void Bring(const FString& Arguments);
    // Core's own commands, "cheat possessplayer <player>" and "cheat unpossess" (#155): the player takes control of the
    // named player's pawn while that player spectates it, and gives it back. The pawn keeps its owner's look (character
    // parts and Battle Royale cosmetics); its items are the possessor's. FortCheatManagerDisguise.cpp.
    void PossessPlayer(const FString& Arguments);
    void Unpossess(const FString& Arguments);
    // A player another player's possessplayer controls, or who controls another's: their own pawn isn't theirs to play.
    static bool IsInPossession(const AFortPlayerController* PlayerController);

    // Client (#152): "cheat togglebloom [on | off]", "cheat togglerecoil [on | off]" (this player's guns' spread and
    // recoil, which only their game simulates) and "cheat copytargetpath [class | mesh | name]" (what they look at, to
    // their clipboard) run here, not on the server; true when Msg was one of them.
    static bool RunClientCheatCommand(class APlayerController* PlayerController, const FString& Msg);
    // Client (#149): every frame, while the server has bInfiniteAmmo on for this player, the equipped weapon's magazine as
    // this client counts it stays full, so its own shots never empty it and start a reload. Nothing is hooked.
    static void StartInfiniteMagazineView();

    // Core's own command, "cheat skin <HID or CID | reset>" ("cheat hero" too; #134, #136): the player wears that hero's
    // look (a Save the World HID_*, a Battle Royale HID_*_Athena_*, or a CID_* outfit's hero), replicated to everyone.
    // Cosmetic only: no perks, abilities or stats. FortCheatManagerDisguise.cpp.
    void Skin(const FString& Arguments);
    // Core's own commands, "cheat disguise <class, path or static mesh> [scale | HID]" and "cheat undisguise" (#134,
    // #136): a player pawn class (PlayerPawn_Ninja_C, ...) is possessed in the player's place with its hero's abilities,
    // another pawn (a husk) is a puppet that walks with the player's hidden pawn, anything else is a prop that hides the
    // player's pawn and follows it. FortCheatManagerDisguise.cpp.
    void Disguise(const FString& Arguments);
    void Undisguise(const FString& Arguments);

    // Set while a disguise possesses a pawn or gives the player's own back: AFortPlayerController::Possess then only does
    // what the new pawn needs to be played (no hero, attribute reset, starting items or gadgets).
    static inline bool bDisguisePossess = false;
    // From Possess once the controller's hero is set again (a respawn): the skin "cheat skin" chose.
    static void ReapplySkin(AFortPlayerController* PlayerController);
    // Client (#137): every frame, the player's own puppet sits on their pawn (the server sends this client none of its
    // movement), every disguise loses its collision here, and the camera pulls back for a disguise bigger than the
    // player. Nothing is hooked: Core's game thread tick, reflected properties and UFunctions.
    static void StartDisguiseView();
    // Client: "cheat disguiseview <back | hide | normal>" (how the player sees their own disguise) runs here, not on the
    // server; true when Msg was it.
    static bool RunDisguiseViewCommand(class APlayerController* PlayerController, const FString& Msg);

    // A final "/Name/" picks the player a cheat runs on (#122): Msg without it in OutCommand, the name in OutTarget.
    static bool SplitCheatTarget(const FString& Msg, FString& OutCommand, FString& OutTarget);
    // "healthpercent" for "sethealthpercent", "bloom" for "togglebloom" (#155): the command name Name means. A name that
    // is a command already (IsKnown, or an exec function of the player's controller, cheat manager or pawn) stays as it
    // is; else "set" or "toggle" in front of it, when that makes one. Lower case.
    static std::wstring ResolveCommandName(APlayerController* PlayerController, const std::wstring& Name, bool (*IsKnown)(const std::wstring&));
    // The player whose display name is Name (case-insensitive) or uniquely starts with it; else null and OutError says why.
    static AFortPlayerController* FindCheatTarget(const UObject* WorldContextObject, const FString& Name, FString& OutError);
    // While a cheat runs on another player, Core's replies go to the player who typed it.
    static inline AFortPlayerController* ReplyTo = nullptr;

    // "fly [speed]", "ghost [speed]" and "walk" (#129): true for those, with OutCommand the command without the speed
    // and OutSpeed the speed (0: the pawn's own), or OutError saying why the speed isn't one.
    static bool ParseCheatMovement(const FString& Command, FString& OutCommand, float& OutSpeed, FString& OutError);
    // The speed the next Fly or Ghost moves the pawn at (0: its own). The server sets it while the command runs; the
    // client from the command its player sends, until the server's ClientCheatFly or ClientCheatGhost comes back.
    static inline float CheatMoveSpeed = 0.0f;
    // ACharacter::OnCheatMovement, on the server and the client: Fly and Ghost take CheatMoveSpeed, Walk the pawn's own.
    static void OnCheatMovement(ACharacter* Character, ECheatMovement Movement);

    // Server: the stripped cheats run Core's bodies.
    static void HookNatives();

    DECLARE_FUNCTION(execGiveWeapon);
    DECLARE_FUNCTION(execGiveAmmo);
    DECLARE_FUNCTION(execGiveTrap);
    DECLARE_FUNCTION(execGiveGadget);
    DECLARE_FUNCTION(execGiveGadgets);
    DECLARE_FUNCTION(execGiveSpecificItem);
    DECLARE_FUNCTION(execGiveResources);
    DECLARE_FUNCTION(execGiveWood);
    DECLARE_FUNCTION(execGiveStone);
    DECLARE_FUNCTION(execGiveMetal);
    DECLARE_FUNCTION(execGiveWeapons);
    DECLARE_FUNCTION(execGiveAllMelee);
    DECLARE_FUNCTION(execGiveAllPistols);
    DECLARE_FUNCTION(execGiveAllRifles);
    DECLARE_FUNCTION(execGiveAllShotguns);
    DECLARE_FUNCTION(execGiveAllWeapons);
    DECLARE_FUNCTION(execGiveAllTraps);
    DECLARE_FUNCTION(execGiveAllConsumables);
    DECLARE_FUNCTION(execGiveAllIngredients);
    DECLARE_FUNCTION(execGiveUsefulThings);
    DECLARE_FUNCTION(execGiveCheatInventory);
    DECLARE_FUNCTION(execWannaGun);
    DECLARE_FUNCTION(execTeleportToLocation);
    DECLARE_FUNCTION(execSetSafeZoneLocationToPlayer);
    DECLARE_FUNCTION(execSetSafeZoneRadius);
    DECLARE_FUNCTION(execRespawnPlayerServer);
    DECLARE_FUNCTION(execTestInfJumps);
    DECLARE_FUNCTION(execGiveBluGlo);
    DECLARE_FUNCTION(execSpawnAIGroup);
    DECLARE_FUNCTION(execSpawnAIGroupFromExternalSpawner);
    DECLARE_FUNCTION(execSendMissionEvent);
    DECLARE_FUNCTION(execLoadMission);
    DECLARE_FUNCTION(execTeleportToMissionActor);
    DECLARE_FUNCTION(execDumpAllActiveMissions);
    DECLARE_FUNCTION(execSimulateLoot);
    DECLARE_FUNCTION(execToggleInvincibleCornerstones);
    DECLARE_FUNCTION(execSetCapturePercentage);
    DECLARE_FUNCTION(execCountPlayerBuiltBuildingActors);

private:
    void Reply(const FString& Message) const;
    // The player, when they have an inventory to give items to.
    AFortPlayerController* GetReceiver() const;
    // Gives the first item of the asset manager's types that the name finds, and says which.
    void GiveNamedItem(std::initializer_list<const wchar_t*> PrimaryAssetTypes, const FString& Name, int32_t Count, int32_t Level);
};
