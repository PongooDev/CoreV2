#include "pch.h"
#include "Player/FortCheatManager.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIEncounterInfo.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Building/BuildingTrap.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Finder.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionInfo.h"
#include "Missions/FortMissionLibrary.h"
#include "Missions/FortMissionManager.h"
#include "Missions/FortObjectiveBase.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <tuple>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortCheats, Log, All);

namespace
{
    // An asset the asset manager lists for a primary asset type, not loaded.
    struct FCheatAsset
    {
        std::wstring Path;
        std::wstring Name;
    };

    std::wstring Lower(std::wstring_view Text)
    {
        std::wstring Result(Text);
        std::transform(Result.begin(), Result.end(), Result.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
        return Result;
    }

    // Lower case without spaces, underscores or quotes, so "Proximity Mine" finds G_Generic_ProximityMines.
    std::wstring Simplified(std::wstring_view Text)
    {
        std::wstring Result;
        for (const wchar_t Char : Text)
        {
            if (Char != L' ' && Char != L'_' && Char != L'"' && Char != L'\'')
            {
                Result += static_cast<wchar_t>(std::towlower(Char));
            }
        }
        return Result;
    }

    bool IsAthenaAsset(const FCheatAsset& Asset)
    {
        return Lower(Asset.Path).find(L"/athena/") != std::wstring::npos;
    }

    // Every asset of the primary asset types (DefaultGame.ini's PrimaryAssetTypesToScan: "Weapon", "Ammo", "Trap",
    // "Gadget", "Ingredient", "Food", "WorldItem"...).
    std::vector<FCheatAsset> ListAssets(std::initializer_list<const wchar_t*> PrimaryAssetTypes)
    {
        std::vector<FCheatAsset> Assets;
        UAssetManager* Manager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
        for (const wchar_t* Type : PrimaryAssetTypes)
        {
            TArray<FAssetData> AssetDataList;
            if (!Manager || !Manager->GetPrimaryAssetDataList(FName(Type), AssetDataList))
            {
                continue;
            }
            for (int32_t Index = 0; Index < AssetDataList.Num(); ++Index)
            {
                const FAssetData& AssetData = AssetDataList[Index];
                Assets.push_back({ AssetData.ObjectPath.ToString().ToStdString(), AssetData.AssetName.ToString().ToStdString() });
            }
        }
        return Assets;
    }

    // The asset a cheat's item name means: its exact name or path, the name without its prefix (WID_, TID_, G_...), or a
    // part of it, this game's items (Battle Royale's /Athena/ ones or Save the World's) and the shortest name first.
    const FCheatAsset* FindAsset(const std::vector<FCheatAsset>& Assets, std::wstring_view Query, bool bBattleRoyale)
    {
        std::wstring_view Trimmed = Query;
        while (!Trimmed.empty() && (Trimmed.front() == L' ' || Trimmed.front() == L'"'))
        {
            Trimmed.remove_prefix(1);
        }
        while (!Trimmed.empty() && (Trimmed.back() == L' ' || Trimmed.back() == L'"'))
        {
            Trimmed.remove_suffix(1);
        }
        const std::wstring Wanted = Lower(Trimmed);
        const std::wstring WantedSimplified = Simplified(Trimmed);
        if (WantedSimplified.empty())
        {
            return nullptr;
        }

        const FCheatAsset* Best = nullptr;
        int32_t BestScore = 0;
        for (const FCheatAsset& Asset : Assets)
        {
            const std::wstring Name = Lower(Asset.Name);
            int32_t Score = 0;
            if (Name == Wanted || Lower(Asset.Path) == Wanted)
            {
                Score = 6;
            }
            else if (Name.size() > Wanted.size() && Name.ends_with(Wanted) && Name[Name.size() - Wanted.size() - 1] == L'_')
            {
                Score = 4;
            }
            else if (Simplified(Asset.Name).find(WantedSimplified) != std::wstring::npos)
            {
                Score = 2;
            }
            if (Score == 0)
            {
                continue;
            }
            Score += IsAthenaAsset(Asset) == bBattleRoyale ? 1 : 0;
            if (Score > BestScore || (Score == BestScore && Asset.Name.size() < Best->Name.size()))
            {
                Best = &Asset;
                BestScore = Score;
            }
        }
        return Best;
    }

    template <typename T>
    T* LoadAsset(std::wstring Path)
    {
        // A package path names the asset of the same name: /Game/Items/Ingredients/Ingredient_Planks.Ingredient_Planks.
        const size_t Slash = Path.rfind(L'/');
        if (Slash != std::wstring::npos && Path.find(L'.', Slash) == std::wstring::npos)
        {
            Path += L"." + Path.substr(Slash + 1);
        }
        return LoadObject<T>(nullptr, Path.c_str());
    }

    UFortWorldItemDefinition* LoadItem(std::wstring Path)
    {
        return LoadAsset<UFortWorldItemDefinition>(std::move(Path));
    }

    // Base destruction PvP's cornerstone (no playlist of these builds plays the mode).
    class AFortPvPBaseCornerstone : public AActor
    {
        DefineUnrealClass(AFortPvPBaseCornerstone, FortniteGame)
        DefineUFunction(OnRep_bInvincible)

    public:
        DefineBitfieldUProperty(bInvincible)

        void SetInvincible(bool bNewInvincible)
        {
            bInvincible = bNewInvincible;
            if (UFunction* Function = FindStaticFunction_OnRep_bInvincible())
            {
                Reflection::FParams Params(Function);
                Params.Call(this);
            }
            ForceNetUpdate();
        }
    };

    // A building that players capture by standing near it (CapturePercentage, 0 to 1).
    class ABuildingCapturePointActor : public AActor
    {
        DefineUnrealClass(ABuildingCapturePointActor, FortniteGame)
        DefineUFunction(OnRep_ReplicatedCapturePercentage)

    public:
        DefineUProperty(float, CapturePercentage)
        DefineUProperty(float, ReplicatedCapturePercentage)

        void SetCapturePercentage(float NewCapturePercentage)
        {
            CapturePercentage = NewCapturePercentage;
            ReplicatedCapturePercentage = NewCapturePercentage;
            if (UFunction* Function = FindStaticFunction_OnRep_ReplicatedCapturePercentage())
            {
                Reflection::FParams Params(Function);
                Params.Call(this);
            }
            ForceNetUpdate();
        }
    };

    // The player-built pieces CountPlayerBuiltBuildingActors sorts by (the native cheat's four, 1.8 0x7D26D0).
    class ABuildingWall : public ABuildingSMActor
    {
        DefineUnrealClass(ABuildingWall, FortniteGame)
    };

    class ABuildingFloor : public ABuildingSMActor
    {
        DefineUnrealClass(ABuildingFloor, FortniteGame)
    };

    class ABuildingRoof : public ABuildingSMActor
    {
        DefineUnrealClass(ABuildingRoof, FortniteGame)
    };

    class ABuildingStairs : public ABuildingSMActor
    {
        DefineUnrealClass(ABuildingStairs, FortniteGame)
    };

    // Objects of the class in the world, not class defaults or archetypes.
    template <typename T>
    std::vector<T*> WorldObjects(const UWorld* World)
    {
        std::vector<T*> Objects;
        if (!T::StaticClass())
        {
            return Objects;
        }
        for (TObjectIterator<T> It(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)); It; ++It)
        {
            if (It->GetWorld() == World)
            {
                Objects.push_back(*It);
            }
        }
        return Objects;
    }

    std::wstring Trim(std::wstring_view Text)
    {
        while (!Text.empty() && std::iswspace(Text.front()))
        {
            Text.remove_prefix(1);
        }
        while (!Text.empty() && std::iswspace(Text.back()))
        {
            Text.remove_suffix(1);
        }
        return std::wstring(Text);
    }

    FString PlayerNameOf(const AController* Controller)
    {
        const APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(Controller->PlayerState) : nullptr;
        return PlayerState ? static_cast<const FString&>(PlayerState->PlayerName) : GetNameSafe(Controller);
    }

    // The missions of the world's mission manager (Save the World), the primary ones first.
    std::vector<AFortMission*> CurrentMissions(const UObject* WorldContextObject)
    {
        std::vector<AFortMission*> Missions;
        UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
        AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        AFortMissionManager* MissionManager = GameState ? static_cast<AFortMissionManager*>(GameState->MissionManager) : nullptr;
        if (!MissionManager)
        {
            return Missions;
        }
        for (AFortMission* Mission : MissionManager->Missions)
        {
            if (Mission)
            {
                Missions.push_back(Mission);
            }
        }
        std::stable_partition(Missions.begin(), Missions.end(), [](const AFortMission* Mission) { return Mission->IsPrimaryMission(); });
        return Missions;
    }

    // Distance in front of the actor along the ground, the way it faces.
    FVector InFrontOf(const AActor* Actor, float Distance)
    {
        const FVector Location = Actor->GetTransform().GetLocation();
        const float Yaw = Actor->GetActorRotation().Yaw * 3.14159265f / 180.0f;
        return FVector(Location.X + std::cos(Yaw) * Distance, Location.Y + std::sin(Yaw) * Distance, Location.Z);
    }

    enum class ECheatMode : uint8_t
    {
        Both,
        BattleRoyale,
        SaveTheWorld,
    };

    // "cheat corecheats": the commands Core supports, by category. Name is the exec function's (left out when the build
    // has none, null for Core's own); the usage follows "cheat ".
    struct FCoreCheat
    {
        const wchar_t* Category;
        const wchar_t* Name;
        const wchar_t* Usage;
        const wchar_t* Description;
        ECheatMode Mode;
    };

    constexpr FCoreCheat CoreCheats[] = {
        { L"Items", L"GiveWeapon", L"GiveWeapon <name> <level> <count>", L"a weapon (or BR consumable) by its name or part of it", ECheatMode::Both },
        { L"Items", L"GiveAmmo", L"GiveAmmo <name> [amount]", L"ammo; a full stack without an amount", ECheatMode::Both },
        { L"Items", L"GiveTrap", L"GiveTrap <name> <level> <count>", L"a trap", ECheatMode::Both },
        { L"Items", L"GiveGadget", L"GiveGadget <name> <level>", L"a gadget (GiveGadgets <name> <count> <level>)", ECheatMode::Both },
        { L"Items", L"GiveSpecificItem", L"GiveSpecificItem <path or name> <count> <level>", L"any item", ECheatMode::Both },
        { L"Items", L"GiveResources", L"GiveResources [amount]", L"wood, stone and metal (GiveWood, GiveStone, GiveMetal [amount])", ECheatMode::Both },
        { L"Items", L"GiveAllWeapons", L"GiveAllWeapons", L"one of every weapon (GiveAllMelee, GiveAllPistols, GiveAllRifles, GiveAllShotguns, GiveWeapons <tag>)", ECheatMode::Both },
        { L"Items", L"GiveAllTraps", L"GiveAllTraps <count>", L"every trap", ECheatMode::Both },
        { L"Items", L"GiveAllConsumables", L"GiveAllConsumables <count>", L"every consumable", ECheatMode::BattleRoyale },
        { L"Items", L"GiveAllIngredients", L"GiveAllIngredients <count>", L"every crafting ingredient (GiveUsefulThings <count>: the common 15)", ECheatMode::SaveTheWorld },
        { L"Items", L"GiveBluGlo", L"GiveBluGlo [amount]", L"BluGlo", ECheatMode::SaveTheWorld },
        { L"Items", L"GiveCheatInventory", L"GiveCheatInventory", L"the TestGod rifle (WannaGun: the WannaGun test gun)", ECheatMode::Both },
        { L"Items", L"GiveAthenaFullKit", L"GiveAthenaFullKit", L"a BR kit (GiveAthenaWeapons, GiveAthenaAmmo, GiveAthenaConsumables, GiveAthenaTraps)", ECheatMode::BattleRoyale },
        { L"Items", L"Badass", L"Badass", L"STW weapons with god, build free and infinite ammo (GiveGoodWeapons: the weapons)", ECheatMode::SaveTheWorld },
        { L"Items", L"ListWeapons", L"ListWeapons", L"the names to give (ListAmmo, ListTraps, ListGadgets)", ECheatMode::Both },
        { L"Player", L"God", L"God", L"invulnerable (toggles)", ECheatMode::Both },
        { L"Player", L"Fly", L"Fly [speed]", L"fly, at the speed if you give one (\"fly 3000\"; Ghost [speed]: through walls too; Walk: walk again at your own speed)", ECheatMode::Both },
        { L"Player", L"TestInfJumps", L"TestInfJumps", L"jump again in the air, any number of times (toggles)", ECheatMode::Both },
        { L"Player", L"Teleport", L"Teleport", L"to where you look (TeleportToLocation <x> <y> <z>, TeleportToActor <class or name>)", ECheatMode::Both },
        { L"Player", L"ChangeSize", L"ChangeSize <scale>", L"your size", ECheatMode::Both },
        { L"Player", L"BuildFree", L"BuildFree", L"building costs nothing (CraftFree: crafting)", ECheatMode::Both },
        { L"Player", L"ToggleInfiniteAmmo", L"ToggleInfiniteAmmo", L"(ToggleInfiniteDurability, ToggleInfiniteStamina, ToggleNoCoolDown)", ECheatMode::Both },
        { L"Player", L"SetHealthPercent", L"SetHealthPercent <0-1>", L"your health (Pain: hurt yourself)", ECheatMode::Both },
        { L"Player", L"DBNO", L"DBNO", L"knock yourself down (ReviveFromDBNO: get up)", ECheatMode::Both },
        { L"Player", L"RespawnPlayerServer", L"RespawnPlayerServer", L"respawn when dead", ECheatMode::Both },
        { L"Player", L"BackpackSetSize", L"BackpackSetSize <slots>", L"your backpack size", ECheatMode::Both },
        { L"Player", L"BuildWith", L"BuildWith <Wood|Stone|Metal>", L"the building material", ECheatMode::Both },
        { L"World", L"Summon", L"Summon <class or /Game/.../X.X_C>", L"spawns an actor in front of you", ECheatMode::Both },
        { L"World", L"Slomo", L"Slomo <speed>", L"the game's speed, for everyone", ECheatMode::Both },
        { L"World", L"PlayersOnly", L"PlayersOnly", L"freezes everything but the players (toggles)", ECheatMode::Both },
        { L"World", L"DestroyTarget", L"DestroyTarget", L"destroys what you look at (DamageTarget <damage>, DestroyAll <class>)", ECheatMode::Both },
        { L"World", L"KillPawns", L"KillPawns [class]", L"kills every pawn (MakeGods, MakeAIsGod: invulnerable)", ECheatMode::Both },
        { L"World", L"SimulateLoot", L"SimulateLoot <tier group> <tier> <level>", L"rolls a loot tier group and says what dropped (tier -1: any)", ECheatMode::Both },
        { L"Storm", L"SetSafeZoneLocationToPlayer", L"SetSafeZoneLocationToPlayer", L"the next storm circles close in on you", ECheatMode::BattleRoyale },
        { L"Storm", L"SetSafeZoneRadius", L"SetSafeZoneRadius <radius>", L"the storm circle's radius until the next phase", ECheatMode::BattleRoyale },
        { L"Save the World", L"SpawnAIGroup", L"SpawnAIGroup <spawn group>", L"husks in front of you, through a running encounter (ListSpawnGroups names them)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"DisableAI", L"DisableAI", L"(ToggleAISpawning, ToggleAIDespawning)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"MissionForceSuccess", L"MissionForceSuccess", L"(MissionForcePrimarySuccess, MissionForceFailure)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"LoadMission", L"LoadMission <mission info>", L"loads a mission into the zone", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SendMissionEvent", L"SendMissionEvent <event>", L"sends a mission event to the missions", ECheatMode::SaveTheWorld },
        { L"Save the World", L"TeleportToMissionActor", L"TeleportToMissionActor", L"to the current objective", ECheatMode::SaveTheWorld },
        { L"Save the World", L"DumpAllActiveMissions", L"DumpAllActiveMissions", L"the missions and their objectives (ListMissions)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SetWorldLevel", L"SetWorldLevel <level>", L"(SetWorldDaysElapsed <days>, UnlockAllZones)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"ToggleIndestructibleKeep", L"ToggleIndestructibleKeep", L"the objective can't be destroyed", ECheatMode::SaveTheWorld },
        { L"Save the World", L"CountPlayerBuiltBuildingActors", L"CountPlayerBuiltBuildingActors", L"the structure count and limit, and the builds and traps it counts", ECheatMode::SaveTheWorld },
        { L"Other", L"CheatScript", L"CheatScript <section>", L"runs a [CheatScript.X] list of cheats", ECheatMode::Both },
        { L"Other", L"ListCheats", L"ListCheats", L"every exec function, working or not", ECheatMode::Both },
        { L"Other", nullptr, L"corecheats [filter or all]", L"this list (a filter keeps the names or categories with it)", ECheatMode::Both },
    };

    // One asset per item family, the name up to its rarity (WID_Assault_Auto_Athena of WID_Assault_Auto_Athena_R_Ore_T03):
    // the rarest, then the highest tier.
    std::vector<FCheatAsset> BestOfEachFamily(const std::vector<FCheatAsset>& Assets)
    {
        static constexpr const wchar_t* Rarities[] = { L"_C_", L"_UC_", L"_R_", L"_VR_", L"_SR_" };
        std::map<std::wstring, std::tuple<int32_t, std::wstring, const FCheatAsset*>> Families;
        for (const FCheatAsset& Asset : Assets)
        {
            size_t Split = std::wstring::npos;
            int32_t Rarity = -1;
            for (int32_t Index = 0; Index < static_cast<int32_t>(std::size(Rarities)); ++Index)
            {
                const size_t Found = Asset.Name.find(Rarities[Index]);
                if (Found != std::wstring::npos && Found < Split)
                {
                    Split = Found;
                    Rarity = Index;
                }
            }
            const std::wstring Family = Lower(Asset.Name.substr(0, Split));
            const std::wstring Rest = Split == std::wstring::npos ? std::wstring() : Asset.Name.substr(Split);
            auto [It, bAdded] = Families.try_emplace(Family, Rarity, Rest, &Asset);
            if (!bAdded && std::tie(Rarity, Rest) > std::tie(std::get<0>(It->second), std::get<1>(It->second)))
            {
                It->second = { Rarity, Rest, &Asset };
            }
        }
        std::vector<FCheatAsset> Result;
        for (const auto& [Family, Best] : Families)
        {
            Result.push_back(*std::get<2>(Best));
        }
        return Result;
    }

    // The item has the gameplay tag or one under it ("Weapon.Ranged" has Weapon.Ranged.Assault).
    bool HasTagOrChild(const UFortItemDefinition* ItemDefinition, const std::wstring& Tag)
    {
        const FGameplayTagContainer& Tags = ItemDefinition->GameplayTags;
        for (const FGameplayTag& ItemTag : Tags.GameplayTags)
        {
            const std::wstring Name = Lower(ItemTag.TagName.ToString().ToStdString());
            if (Name == Tag || Name.starts_with(Tag + L"."))
            {
                return true;
            }
        }
        return false;
    }

    int32_t FullStack(const UFortItemDefinition* ItemDefinition)
    {
        const int32_t MaxStackSize = ItemDefinition ? static_cast<int32_t>(ItemDefinition->MaxStackSize) : 0;
        return MaxStackSize > 0 ? MaxStackSize : 999;
    }

    // UFortCheatManager::GiveUsefulThings (1.7.2 0x7D0290, 1.8 the same list) built these paths for GiveSpecificItem,
    // which shipping builds compiled out, and then dropped them.
    constexpr const wchar_t* UsefulThings[] = {
        L"/Game/Items/Ingredients/Ingredient_Twine_T01", L"/Game/Items/Ingredients/Ingredient_Ore_Alloy",
        L"/Game/Items/Ingredients/Ingredient_Planks", L"/Game/Items/Ingredients/Ingredient_Ore_Coal",
        L"/Game/Items/Ingredients/Ingredient_Resin", L"/Game/Items/Ingredients/Ingredient_Herbs",
        L"/Game/Items/Ingredients/Ingredient_Nuts_Bolts", L"/Game/Items/Ingredients/Ingredient_Flowers",
        L"/Game/Items/Ingredients/Ingredient_Bacon", L"/Game/Items/Ingredients/Ingredient_Batteries",
        L"/Game/Items/Ingredients/Ingredient_Blastpowder", L"/Game/Items/Ingredients/Ingredient_Mechanical_Parts_T01",
        L"/Game/Items/Ingredients/Ingredient_Duct_Tape", L"/Game/Items/Ingredients/Ingredient_Ore_Copper",
        L"/Game/Items/Ingredients/Ingredient_Powder_T01",
    };
}

AFortPlayerController* UFortCheatManager::GetPlayerController() const
{
    return Cast<AFortPlayerController>(GetOuterAPlayerController());
}

void UFortCheatManager::Reply(const FString& Message) const
{
    AFortPlayerController* PlayerController = GetPlayerController();
    UE_LOG(LogFortCheats, Log, TEXT("%s: %s"), *GetNameSafe(PlayerController), *Message);
    // A cheat run on another player (#122) answers the player who typed it, naming whom it ran on.
    if (ReplyTo && ReplyTo != PlayerController)
    {
        ReplyTo->ClientMessage(FString::Printf(TEXT("[%s] %s"), *PlayerNameOf(PlayerController), *Message));
    }
    else if (PlayerController)
    {
        PlayerController->ClientMessage(Message);
    }
}

AFortPlayerController* UFortCheatManager::GetReceiver() const
{
    AFortPlayerController* PlayerController = GetPlayerController();
    if (PlayerController && !PlayerController->WorldInventory)
    {
        Reply(TEXT("You have no inventory to give items to yet"));
        return nullptr;
    }
    return PlayerController;
}

void UFortCheatManager::GiveNamedItem(std::initializer_list<const wchar_t*> PrimaryAssetTypes, const FString& Name, int32_t Count, int32_t Level)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }

    UFortWorldItemDefinition* ItemDefinition = nullptr;
    const std::wstring Query = Name.ToStdString();
    if (Query.find(L'/') != std::wstring::npos)
    {
        ItemDefinition = LoadItem(Query);
    }
    else
    {
        const std::vector<FCheatAsset> Assets = ListAssets(PrimaryAssetTypes);
        if (const FCheatAsset* Asset = FindAsset(Assets, Query, AFortGameModeAthena::GetCurrent(PlayerController) != nullptr))
        {
            ItemDefinition = LoadItem(Asset->Path);
        }
    }
    if (!ItemDefinition)
    {
        Reply(FString::Printf(TEXT("No item called '%s' (ListWeapons, ListAmmo, ListTraps and ListGadgets list them)"), *Name));
        return;
    }

    if (Count <= 0)
    {
        Count = 1;
    }
    UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, ItemDefinition, Count, true, std::max(Level, 1), true);
    Reply(FString::Printf(TEXT("Gave %d %s"), Count, *ItemDefinition->GetName()));
#endif
}

void UFortCheatManager::GiveWeapon(const FString& WeaponName, int32_t RequestedLevel, int32_t Count)
{
    // Battle Royale's consumables are weapons too (Weapon's directories include /Game/Athena/Items/Consumables).
    GiveNamedItem({ L"Weapon" }, WeaponName, Count, RequestedLevel);
}

void UFortCheatManager::GiveAmmo(const FString& AmmoNameAndAmount)
{
#if WITH_SERVER_CODE
    // "AmmoDataBulletsHeavy 1000", the CheatScript sections' form.
    const std::wstring Line = AmmoNameAndAmount.ToStdString();
    const size_t Space = Line.find_last_of(L' ');
    std::wstring Name = Line;
    int32_t Amount = 0;
    if (Space != std::wstring::npos)
    {
        wchar_t* End = nullptr;
        const long Parsed = std::wcstol(Line.c_str() + Space + 1, &End, 10);
        if (End && *End == L'\0' && Space + 1 < Line.size())
        {
            Name = Line.substr(0, Space);
            Amount = static_cast<int32_t>(Parsed);
        }
    }

    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }
    const std::vector<FCheatAsset> Assets = ListAssets({ L"Ammo" });
    const FCheatAsset* Asset = FindAsset(Assets, Name, AFortGameModeAthena::GetCurrent(PlayerController) != nullptr);
    UFortWorldItemDefinition* Ammo = Asset ? LoadItem(Asset->Path) : nullptr;
    if (!Ammo)
    {
        Reply(FString::Printf(TEXT("No ammo called '%s' (ListAmmo lists them)"), Name.c_str()));
        return;
    }
    const int32_t Count = Amount > 0 ? Amount : FullStack(Ammo);
    UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Ammo, Count, true);
    Reply(FString::Printf(TEXT("Gave %d %s"), Count, *Ammo->GetName()));
#endif
}

void UFortCheatManager::GiveTrap(const FString& TrapName, int32_t RequestedLevel, int32_t NumTrapsToGive)
{
    GiveNamedItem({ L"Trap" }, TrapName, NumTrapsToGive, RequestedLevel);
}

void UFortCheatManager::GiveGadgets(const FString& GadgetName, int32_t Quantity, int32_t ItemLevel)
{
    GiveNamedItem({ L"Gadget" }, GadgetName, Quantity, ItemLevel);
}

void UFortCheatManager::GiveSpecificItem(const FString& ItemPath, int32_t ItemAmount, int32_t ItemLevel)
{
    GiveNamedItem({ L"Weapon", L"Ammo", L"Trap", L"Gadget", L"Ingredient", L"Food", L"WorldItem" }, ItemPath, ItemAmount, ItemLevel);
}

void UFortCheatManager::GiveResource(UFortWorldItemDefinition* Resource, int32_t Amount)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController || !Resource)
    {
        return;
    }
    const int32_t Count = Amount > 0 ? Amount : FullStack(Resource);
    UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Resource, Count, true);
    Reply(FString::Printf(TEXT("Gave %d %s"), Count, *Resource->GetName()));
#endif
}

void UFortCheatManager::GiveResources(int32_t Amount)
{
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (GameData)
    {
        GiveResource(GameData->WoodItemDefinition, Amount);
        GiveResource(GameData->StoneItemDefinition, Amount);
        GiveResource(GameData->MetalItemDefinition, Amount);
    }
}

void UFortCheatManager::GiveWeapons(FName Tag)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }

    // Weapons proper (WID_, not consumables, building pieces, test guns, pickaxes or hero ability weapons), this game's
    // only: Battle Royale's about 20 families, Save the World's about 70, each in every rarity, material and tier.
    const bool bBattleRoyale = AFortGameModeAthena::GetCurrent(PlayerController) != nullptr;
    std::vector<FCheatAsset> Weapons;
    for (FCheatAsset& Asset : ListAssets({ L"Weapon" }))
    {
        if (IsAthenaAsset(Asset) == bBattleRoyale && Lower(Asset.Name).starts_with(L"wid_") && Lower(Asset.Path).find(L"/items/weapons/") != std::wstring::npos)
        {
            Weapons.push_back(std::move(Asset));
        }
    }

    const std::wstring WantedTag = Tag.IsNone() ? std::wstring() : Lower(Tag.ToString().ToStdString());
    int32_t Given = 0;
    for (const FCheatAsset& Asset : BestOfEachFamily(Weapons))
    {
        UFortWorldItemDefinition* Weapon = LoadItem(Asset.Path);
        if (Weapon && Weapon->ItemType != EFortItemType::GetWeaponHarvest() && (WantedTag.empty() || HasTagOrChild(Weapon, WantedTag)))
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Weapon, 1, true, 1, true);
            ++Given;
        }
    }
    Reply(FString::Printf(TEXT("Gave %d weapons%s%s"), Given, WantedTag.empty() ? TEXT("") : TEXT(" tagged "), WantedTag.c_str()));
#endif
}

void UFortCheatManager::GiveAllTraps(int32_t NumItems)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }

    const bool bBattleRoyale = AFortGameModeAthena::GetCurrent(PlayerController) != nullptr;
    std::vector<FCheatAsset> Traps;
    for (FCheatAsset& Asset : ListAssets({ L"Trap" }))
    {
        if (IsAthenaAsset(Asset) == bBattleRoyale)
        {
            Traps.push_back(std::move(Asset));
        }
    }
    int32_t Given = 0;
    for (const FCheatAsset& Asset : BestOfEachFamily(Traps))
    {
        if (UFortWorldItemDefinition* Trap = LoadItem(Asset.Path))
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Trap, std::max(NumItems, 1), true);
            ++Given;
        }
    }
    Reply(FString::Printf(TEXT("Gave %d of %d traps"), std::max(NumItems, 1), Given));
#endif
}

void UFortCheatManager::GiveAllConsumables(int32_t NumItems)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }

    // Battle Royale's consumables are weapons (Athena_Bandage, Athena_Medkit...); Save the World's are Food.
    const bool bBattleRoyale = AFortGameModeAthena::GetCurrent(PlayerController) != nullptr;
    int32_t Given = 0;
    for (const FCheatAsset& Asset : ListAssets({ bBattleRoyale ? L"Weapon" : L"Food" }))
    {
        if (IsAthenaAsset(Asset) != bBattleRoyale || (bBattleRoyale && Lower(Asset.Path).find(L"/items/consumables/") == std::wstring::npos))
        {
            continue;
        }
        if (UFortWorldItemDefinition* Consumable = LoadItem(Asset.Path))
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Consumable, std::max(NumItems, 1), true);
            ++Given;
        }
    }
    Reply(FString::Printf(TEXT("Gave %d of %d consumables"), std::max(NumItems, 1), Given));
#endif
}

void UFortCheatManager::GiveAllIngredients(int32_t NumIngredients)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetReceiver();
    if (!PlayerController)
    {
        return;
    }

    int32_t Given = 0;
    for (const FCheatAsset& Asset : ListAssets({ L"Ingredient" }))
    {
        if (UFortWorldItemDefinition* Ingredient = LoadItem(Asset.Path))
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Ingredient, std::max(NumIngredients, 1), true);
            ++Given;
        }
    }
    Reply(FString::Printf(TEXT("Gave %d of %d ingredients"), std::max(NumIngredients, 1), Given));
#endif
}

void UFortCheatManager::GiveUsefulThings(int32_t NumOfUsefulThings)
{
    for (const wchar_t* Path : UsefulThings)
    {
        GiveSpecificItem(FString(Path), NumOfUsefulThings, 1);
    }
}

void UFortCheatManager::WannaGun()
{
    // Empty in shipping builds (it shares its stripped exec with AthenaEndlessGame); the build still cooks the test gun of
    // the same name.
    GiveNamedItem({ L"Weapon" }, TEXT("/Game/Items/Weapons/Ranged/WIP/WannaGun"), 1, 1);
}

void UFortCheatManager::GiveCheatInventory()
{
#if WITH_SERVER_CODE
    // The native one loads the game data's CheatInventoryItems and gives them from the load's callback, a method of the
    // cheat manager that shipping builds compiled out (1.7.2 0x7CFD30 binds the shared "ret 0").
    AFortPlayerController* PlayerController = GetReceiver();
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!PlayerController || !GameData)
    {
        return;
    }
    const TArray<TAssetPtr<UFortWorldItemDefinition>>& Items = GameData->CheatInventoryItems;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        if (UFortWorldItemDefinition* Item = Items[Index].LoadSynchronous())
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Item, 1, true, 1, true);
            Reply(FString::Printf(TEXT("Gave 1 %s"), *Item->GetName()));
        }
    }
#endif
}

void UFortCheatManager::TeleportToLocation(float X, float Y, float Z)
{
#if WITH_SERVER_CODE
    // The native one stores the location in the controller's LocationUnderReticle and calls a virtual that only Save
    // the World's controller implements (1.7.2 slot 0x2D9, AFortPlayerControllerZone 0x82EA80; 1.8 slot 0x2DD; Athena's
    // is the shared "ret 0" on both), so it did nothing in Battle Royale. It teleports the pawn here, facing the same way.
    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    if (!Pawn)
    {
        Reply(TEXT("No pawn to teleport"));
        return;
    }
    const FVector Location(X, Y, Z);
    if (!Pawn->TeleportTo(Location, Pawn->GetActorRotation()))
    {
        Reply(FString::Printf(TEXT("Can't teleport to X=%.1f Y=%.1f Z=%.1f (blocked)"), X, Y, Z));
    }
#endif
}

void UFortCheatManager::SetSafeZoneLocationToPlayer()
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(PlayerController);
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    if (!GameMode || !Pawn)
    {
        Reply(TEXT("SetSafeZoneLocationToPlayer needs Battle Royale and a pawn"));
        return;
    }

    // The game mode picks every phase's center when the storm starts (SafeZoneLocations, index: the phase) and moves the
    // circle to the next one as each phase begins, so the ones after the current phase close in on the player.
    const FVector Location = Pawn->GetTransform().GetLocation();
    TArray<FVector>& Locations = GameMode->SafeZoneLocations;
    const int32_t FirstPhase = std::max(static_cast<int32_t>(GameMode->SafeZonePhase) + 1, 0);
    if (FirstPhase >= Locations.Num())
    {
        Reply(TEXT("No safe zone left to move (the storm picks them when it starts)"));
        return;
    }
    for (int32_t Phase = FirstPhase; Phase < Locations.Num(); ++Phase)
    {
        Locations[Phase].X = Location.X;
        Locations[Phase].Y = Location.Y;
    }
    Reply(FString::Printf(TEXT("Center of zone successfully moved to X=%.1f Y=%.1f (from phase %d of %d)"), Location.X, Location.Y, FirstPhase, Locations.Num()));
#endif
}

void UFortCheatManager::SetSafeZoneRadius(float NewRadius)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(GetPlayerController());
    AFortSafeZoneIndicator* Indicator = GameMode ? static_cast<AFortSafeZoneIndicator*>(GameMode->SafeZoneIndicator) : nullptr;
    if (!Indicator || NewRadius <= 0.0f)
    {
        Reply(TEXT("SetSafeZoneRadius needs Battle Royale's storm and a radius"));
        return;
    }

    // The circle shrinks from LastRadius to NextRadius during a phase; all three hold the new radius until the next.
    Indicator->Radius = NewRadius;
    Indicator->LastRadius = NewRadius;
    Indicator->NextRadius = NewRadius;
    Indicator->ForceNetUpdate();
    Reply(FString::Printf(TEXT("Safe zone radius set to %.1f"), NewRadius));
#endif
}

void UFortCheatManager::RespawnPlayerServer()
{
#if WITH_SERVER_CODE
    // UFortCheatManager::RespawnPlayer (client) sends "RespawnPlayerServer" with Cheat. A dead player gets a new pawn the
    // way the game mode starts every player.
    AFortPlayerController* PlayerController = GetPlayerController();
    UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
    AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
    AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (!GameMode)
    {
        return;
    }
    if (Pawn && !Pawn->IsDead())
    {
        Reply(TEXT("RespawnPlayerServer respawns a dead player; you are alive"));
        return;
    }
    GameMode->RestartPlayer(PlayerController);
    Reply(TEXT("Respawned"));
#endif
}

void UFortCheatManager::TestInfJumps()
{
#if WITH_SERVER_CODE
    // Empty in shipping builds (it shares a "do nothing" exec with other compiled-out cheats). A character may jump while
    // in the air as long as JumpCurrentCount < JumpMaxCount (ACharacter::CanJumpInternal, 1.7.2 0x1FCE070, under the Athena
    // pawn's 0x7C3460); JumpMaxCount replicates, so the owning client predicts the extra jumps too.
    AFortPlayerController* PlayerController = GetPlayerController();
    ACharacter* Character = PlayerController ? Cast<ACharacter>(reinterpret_cast<UObject*>(PlayerController->GetPawn())) : nullptr;
    if (!Character)
    {
        Reply(TEXT("No pawn to jump with"));
        return;
    }
    const ACharacter* Default = static_cast<const ACharacter*>(Character->GetClass()->GetDefaultObject(false));
    const int32_t DefaultJumps = Default ? std::max<int32_t>(Default->JumpMaxCount, 1) : 1;
    const bool bInfinite = Character->JumpMaxCount <= DefaultJumps;
    Character->JumpMaxCount = bInfinite ? 100000 : DefaultJumps;
    Character->ForceNetUpdate();
    Reply(bInfinite ? TEXT("Infinite jumps on (until you respawn)") : TEXT("Infinite jumps off"));
#endif
}

void UFortCheatManager::GiveBluGlo(int32_t Amount)
{
    // Empty in shipping builds. The game data names the BluGlo item (BluGloWorldItemData) on every build.
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UFortWorldItemDefinition* BluGlo = GameData ? static_cast<UFortWorldItemDefinition*>(GameData->BluGloItemDefinition) : nullptr;
    if (!BluGlo)
    {
        Reply(TEXT("This build's game data has no BluGlo item"));
        return;
    }
    GiveResource(BluGlo, Amount);
}

void UFortCheatManager::SpawnAIGroup(const FString& SpawnGroupName, bool bFromExternalSpawner)
{
#if WITH_SERVER_CODE
    // The cheats only free their string in shipping builds. A spawn group spawns through an encounter: the encounter's
    // native SpawnAIGroup (1.7.2 0x2D74B0) queues its enemies with the director. UFortMissionLibrary::StartAIEncounter is
    // compiled out (1.7.2 0x6B3D20 returns null), so the group joins an encounter that is already running: the director's
    // debug one, its default one, or the newest of its active ones (a mission's).
    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    AFortAIDirector* Director = AFortAIDirector::GetCurrent(PlayerController);
    if (!Pawn || !Director)
    {
        Reply(TEXT("SpawnAIGroup needs Save the World's AI director and a pawn"));
        return;
    }

    const std::vector<FCheatAsset> Assets = ListAssets({ L"FortAISpawnGroup" });
    const FCheatAsset* Asset = FindAsset(Assets, SpawnGroupName.ToStdString(), false);
    UFortAISpawnGroup* SpawnGroup = Asset ? LoadAsset<UFortAISpawnGroup>(Asset->Path) : nullptr;
    if (!SpawnGroup)
    {
        Reply(FString::Printf(TEXT("No spawn group called '%s' (ListSpawnGroups lists them)"), *SpawnGroupName));
        return;
    }

    UFortAIEncounterInfo* Encounter = Director->ActiveDummyDebugEncounter;
    if (!Encounter)
    {
        Encounter = Director->ActiveDefaultEncounter;
    }
    const TArray<UFortAIEncounterInfo*>& ActiveEncounters = Director->ActiveEncounters;
    for (int32_t Index = ActiveEncounters.Num() - 1; !Encounter && Index >= 0; --Index)
    {
        Encounter = ActiveEncounters[Index];
    }
    if (!Encounter)
    {
        Reply(TEXT("No AI encounter is running to spawn the group through (start a mission's waves first)"));
        return;
    }

    const FVector Location = InFrontOf(Pawn, 600.0f);
    const FRotator Rotation(0.0f, Pawn->GetActorRotation().Yaw + 180.0f, 0.0f);
    if (!Encounter->SpawnAIGroup(SpawnGroup, Location, Rotation, bFromExternalSpawner))
    {
        Reply(TEXT("This build's encounters can't spawn a group"));
        return;
    }
    Reply(FString::Printf(TEXT("Spawning %s through %s"), *SpawnGroup->GetName(), *Encounter->GetName()));
#endif
}

void UFortCheatManager::SendMissionEvent(const FString& MissionEventName)
{
#if WITH_SERVER_CODE
    // Only frees its string in shipping builds. Mission events (the mission manager's MissionEventNames: "ItemCollected",
    // "MissionParTimeHit", the missions' own) go to every mission through the library, as Blueprints send them.
    const std::wstring EventName = Trim(MissionEventName.ToStdString());
    if (EventName.empty() || !UFortMissionLibrary::FindStaticFunction_ExecuteMissionEvent() || CurrentMissions(GetPlayerController()).empty())
    {
        Reply(TEXT("SendMissionEvent needs Save the World's missions and an event name"));
        return;
    }
    UFortMissionLibrary::ExecuteMissionEvent(GetPlayerController(), FName(EventName.c_str()), nullptr);
    Reply(FString::Printf(TEXT("Sent mission event %s"), EventName.c_str()));
#endif
}

void UFortCheatManager::LoadMission(const FString& MissionName)
{
#if WITH_SERVER_CODE
    // Only frees its string in shipping builds. The zone loads its missions through UFortMissionLibrary::LoadMission
    // (AFortWorldManager does the same with its mission records).
    if (CurrentMissions(GetPlayerController()).empty() || !UFortMissionLibrary::FindStaticFunction_LoadMission())
    {
        Reply(TEXT("LoadMission needs Save the World's mission manager"));
        return;
    }
    const std::vector<FCheatAsset> Assets = ListAssets({ L"FortMissionInfo" });
    const FCheatAsset* Asset = FindAsset(Assets, MissionName.ToStdString(), false);
    UFortMissionInfo* MissionInfo = Asset ? LoadAsset<UFortMissionInfo>(Asset->Path) : nullptr;
    if (!MissionInfo)
    {
        Reply(FString::Printf(TEXT("No mission called '%s'"), *MissionName));
        return;
    }
    UFortMissionLibrary::LoadMission(GetPlayerController(), MissionInfo);
    Reply(FString::Printf(TEXT("Loading mission %s"), *MissionInfo->GetName()));
#endif
}

void UFortCheatManager::TeleportToMissionActor()
{
#if WITH_SERVER_CODE
    // Empty in shipping builds. The mission actors players go to are its active objectives (the primary mission's first).
    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    if (!Pawn)
    {
        Reply(TEXT("No pawn to teleport"));
        return;
    }
    for (AFortMission* Mission : CurrentMissions(PlayerController))
    {
        for (AFortObjectiveBase* Objective : Mission->ActiveObjectives)
        {
            const FVector Location = Objective ? Objective->GetTransform().GetLocation() : FVector(0.0f, 0.0f, 0.0f);
            if (!Objective || (Location.X == 0.0f && Location.Y == 0.0f && Location.Z == 0.0f))
            {
                continue;
            }
            // Next to it rather than inside it; TeleportTo moves the pawn to a free spot.
            const FVector Destination(Location.X + 150.0f, Location.Y, Location.Z + 100.0f);
            if (Pawn->TeleportTo(Destination, Pawn->GetActorRotation()))
            {
                Reply(FString::Printf(TEXT("Teleported to %s (%s)"), *Objective->GetName(), *Mission->GetName()));
                return;
            }
        }
    }
    Reply(TEXT("No mission has an objective to teleport to"));
#endif
}

void UFortCheatManager::DumpAllActiveMissions()
{
#if WITH_SERVER_CODE
    const std::vector<AFortMission*> Missions = CurrentMissions(GetPlayerController());
    if (Missions.empty())
    {
        Reply(TEXT("No missions (Save the World only)"));
        return;
    }
    for (AFortMission* Mission : Missions)
    {
        const UFortMissionInfo* MissionInfo = Mission->MissionInfo;
        const EFortMissionStatus Status = Mission->MissionStatus;
        Reply(FString::Printf(TEXT("%s %s (%s): %s"), Mission->IsPrimaryMission() ? TEXT("Primary") : TEXT("Secondary"), *GetNameSafe(MissionInfo), *Mission->GetName(), Status.ToString().c_str()));
        for (AFortObjectiveBase* Objective : Mission->ActiveObjectives)
        {
            if (Objective)
            {
                const EFortObjectiveStatus ObjectiveStatus = Objective->ObjectiveStatus;
                Reply(FString::Printf(TEXT("  objective %s: %s"), *Objective->GetName(), ObjectiveStatus.ToString().c_str()));
            }
        }
    }
#endif
}

void UFortCheatManager::SimulateLoot(FName TierGroup, int32_t LootTier, int32_t LootLevel)
{
#if WITH_SERVER_CODE
    // Empty in shipping builds. Rolls the tier group the way a container does (Core's loot roll; Battle Royale's redirect
    // first, as its containers roll Loot_Athena* groups) and lists the drops without spawning them.
    AFortPlayerController* PlayerController = GetPlayerController();
    FName Group = TierGroup;
    if (AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(PlayerController))
    {
        const FName Redirected = GameMode->RedirectLootTierGroup(TierGroup);
        Group = Redirected.IsNone() ? TierGroup : Redirected;
    }
    if (LootLevel <= 0)
    {
        UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
        AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        LootLevel = GameState ? static_cast<int32_t>(GameState->WorldLevel) : 0;
    }

    TStructArray<FFortItemEntry> LootDrops{};
    if (Group.IsNone() || !UFortKismetLibrary::PickLootDrops(LootDrops, Group, LootLevel, LootTier))
    {
        Reply(FString::Printf(TEXT("Tier group '%s' dropped nothing (Loot_AthenaTreasure, Loot_AthenaFloorLoot, Loot_Treasure...)"), *TierGroup.ToString()));
        LootDrops.Empty();
        return;
    }
    Reply(FString::Printf(TEXT("%s (tier %d, level %d) dropped %d:"), *Group.ToString(), LootTier, LootLevel, LootDrops.Num()));
    for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
    {
        const FFortItemEntry& Entry = LootDrops[Index];
        Reply(FString::Printf(TEXT("  %d %s"), static_cast<int32_t>(Entry.Count), *GetNameSafe(static_cast<UFortItemDefinition*>(Entry.ItemDefinition))));
    }
    LootDrops.Empty();
#endif
}

void UFortCheatManager::ToggleInvincibleCornerstones()
{
#if WITH_SERVER_CODE
    // Empty in shipping builds. Each team's cornerstone (base destruction PvP) has bInvincible, replicated.
    AFortPlayerController* PlayerController = GetPlayerController();
    const std::vector<AFortPvPBaseCornerstone*> Cornerstones = WorldObjects<AFortPvPBaseCornerstone>(PlayerController ? PlayerController->GetWorld() : nullptr);
    if (Cornerstones.empty())
    {
        Reply(TEXT("No cornerstones in this match (base destruction PvP only)"));
        return;
    }
    const bool bInvincible = !Cornerstones.front()->bInvincible;
    for (AFortPvPBaseCornerstone* Cornerstone : Cornerstones)
    {
        Cornerstone->SetInvincible(bInvincible);
    }
    Reply(FString::Printf(TEXT("%d cornerstones %s"), static_cast<int32_t>(Cornerstones.size()), bInvincible ? TEXT("invincible") : TEXT("vulnerable")));
#endif
}

void UFortCheatManager::SetCapturePercentage(float NewCapturePercentage)
{
#if WITH_SERVER_CODE
    // Only reads its argument in shipping builds. Sets every capture point's progress, which replicates.
    AFortPlayerController* PlayerController = GetPlayerController();
    const std::vector<ABuildingCapturePointActor*> CapturePoints = WorldObjects<ABuildingCapturePointActor>(PlayerController ? PlayerController->GetWorld() : nullptr);
    if (CapturePoints.empty())
    {
        Reply(TEXT("No capture points in this match"));
        return;
    }
    const float Percentage = std::clamp(NewCapturePercentage > 1.0f ? NewCapturePercentage / 100.0f : NewCapturePercentage, 0.0f, 1.0f);
    for (ABuildingCapturePointActor* CapturePoint : CapturePoints)
    {
        CapturePoint->SetCapturePercentage(Percentage);
    }
    Reply(FString::Printf(TEXT("%d capture points at %.0f%%"), static_cast<int32_t>(CapturePoints.size()), Percentage * 100.0f));
#endif
}

void UFortCheatManager::CountPlayerBuiltBuildingActors()
{
#if WITH_SERVER_CODE
    // The native body (1.8 0x7D26D0) counts the player-built walls, floors, roofs and stairs, but only writes them to the
    // server's log and a local console, so the player saw nothing. This answers them, with the count the limit uses.
    AFortPlayerController* PlayerController = GetPlayerController();
    UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    int32_t Total = 0;
    int32_t Max = -1;
    if (!GameState || !GameState->GetPlayerStructures(Total, Max))
    {
        Reply(TEXT("No structure count here (Save the World zones only)"));
        return;
    }

    int32_t Walls = 0;
    int32_t Floors = 0;
    int32_t Roofs = 0;
    int32_t Stairs = 0;
    int32_t Traps = 0;
    int32_t Others = 0;
    for (ABuildingActor* Building : WorldObjects<ABuildingActor>(World))
    {
        if (!Building->CountsTowardStructureLimit())
        {
            continue;
        }
        UObject* Object = reinterpret_cast<UObject*>(Building);
        int32_t& Kind = Object->IsA(ABuildingTrap::StaticClass()) ? Traps
            : ABuildingWall::StaticClass() && Object->IsA(ABuildingWall::StaticClass()) ? Walls
            : ABuildingFloor::StaticClass() && Object->IsA(ABuildingFloor::StaticClass()) ? Floors
            : ABuildingRoof::StaticClass() && Object->IsA(ABuildingRoof::StaticClass()) ? Roofs
            : ABuildingStairs::StaticClass() && Object->IsA(ABuildingStairs::StaticClass()) ? Stairs
            : Others;
        ++Kind;
    }
    const int32_t Counted = Walls + Floors + Roofs + Stairs + Traps + Others;

    Reply(Max < 0 ? FString::Printf(TEXT("Structures: %d (no limit here)"), Total) : FString::Printf(TEXT("Structures: %d of %d%s"), Total, Max, Total >= Max ? TEXT(", building is blocked") : TEXT("")));
    Reply(FString::Printf(TEXT("Standing and counted: %d walls, %d floors, %d roofs, %d stairs, %d traps, %d others (%d)"), Walls, Floors, Roofs, Stairs, Traps, Others, Counted));
    if (Counted != Total)
    {
        Reply(FString::Printf(TEXT("The count is %d %s what is standing"), Total > Counted ? Total - Counted : Counted - Total, Total > Counted ? TEXT("more than") : TEXT("less than")));
    }
#endif
}

void UFortCheatManager::ListCoreCheats(const FString& Filter)
{
    // Core's own: what works, for this mode unless the filter is "all".
    const std::wstring Wanted = Simplified(Trim(Filter.ToStdString()));
    const bool bAll = Wanted == L"all";
    AFortPlayerController* PlayerController = GetPlayerController();
    const bool bBattleRoyale = AFortGameModeAthena::GetCurrent(PlayerController) != nullptr;
    UClass* Class = GetClass();

    std::vector<const FCoreCheat*> Shown;
    for (const FCoreCheat& Cheat : CoreCheats)
    {
        if (!bAll && Cheat.Mode != ECheatMode::Both && (Cheat.Mode == ECheatMode::BattleRoyale) != bBattleRoyale)
        {
            continue;
        }
        if (Cheat.Name && (!Class || !Class->FindFunctionByName(FName(Cheat.Name, FNAME_Find))))
        {
            continue;
        }
        if (!bAll && !Wanted.empty() && Simplified(Cheat.Usage).find(Wanted) == std::wstring::npos && Simplified(Cheat.Category).find(Wanted) == std::wstring::npos &&
            Simplified(Cheat.Description).find(Wanted) == std::wstring::npos)
        {
            continue;
        }
        Shown.push_back(&Cheat);
    }

    Reply(FString::Printf(TEXT("Core cheats%s (%d): type \"cheat <command>\". End any command with /Player/ to run it on that player instead."),
        bAll ? TEXT("") : bBattleRoyale ? TEXT(" for Battle Royale") : TEXT(" for Save the World"), static_cast<int32_t>(Shown.size())));
    const wchar_t* Category = nullptr;
    for (const FCoreCheat* Cheat : Shown)
    {
        if (!Category || std::wstring_view(Category) != Cheat->Category)
        {
            Category = Cheat->Category;
            Reply(FString::Printf(TEXT("-- %s --"), Category));
        }
        Reply(FString::Printf(TEXT("%s: %s"), Cheat->Usage, Cheat->Description));
    }
    if (Shown.empty())
    {
        Reply(TEXT("Nothing matches; \"cheat corecheats all\" lists every one"));
    }
}

bool UFortCheatManager::SplitCheatTarget(const FString& Msg, FString& OutCommand, FString& OutTarget)
{
    // "giveweapon Scar 0 1 /diddy/": a last "/Name/" (it may hold spaces, "/John Doe/") that starts the line or follows a
    // space, so item paths (/Game/Items/...) aren't taken for one.
    const std::wstring Line = Trim(Msg.ToStdString());
    if (Line.size() < 3 || Line.back() != L'/')
    {
        return false;
    }
    const size_t Close = Line.size() - 1;
    const size_t Open = Line.rfind(L'/', Close - 1);
    if (Open == std::wstring::npos || Open + 1 >= Close || (Open > 0 && !std::iswspace(Line[Open - 1])))
    {
        return false;
    }
    const std::wstring Target = Trim(std::wstring_view(Line).substr(Open + 1, Close - Open - 1));
    if (Target.empty())
    {
        return false;
    }
    OutCommand = FString(Trim(std::wstring_view(Line).substr(0, Open)).c_str());
    OutTarget = FString(Target.c_str());
    return true;
}

bool UFortCheatManager::ParseCheatMovement(const FString& Command, FString& OutCommand, float& OutSpeed, FString& OutError)
{
    // The speed is how fast the pawn runs while it flies, in cm/s; it walks and sprints faster by as much.
    constexpr float MaxSpeed = 100000.0f;
    std::vector<std::wstring> Words;
    const std::wstring Line = Command.ToStdString();
    for (size_t Start = 0; Start < Line.size();)
    {
        while (Start < Line.size() && std::iswspace(Line[Start]))
        {
            ++Start;
        }
        size_t End = Start;
        while (End < Line.size() && !std::iswspace(Line[End]))
        {
            ++End;
        }
        if (End > Start)
        {
            Words.emplace_back(Line.substr(Start, End - Start));
        }
        Start = End;
    }
    if (Words.empty())
    {
        return false;
    }
    const std::wstring Name = Lower(Words[0]);
    if (Name != L"fly" && Name != L"ghost" && Name != L"walk")
    {
        return false;
    }

    OutCommand = Command;
    OutSpeed = 0.0f;
    OutError = FString();
    if (Name == L"walk" || Words.size() == 1)
    {
        return true;
    }

    wchar_t* End = nullptr;
    const float Speed = std::wcstof(Words[1].c_str(), &End);
    if (Words.size() > 2 || !End || *End != L'\0' || !std::isfinite(Speed) || Speed < 1.0f || Speed > MaxSpeed)
    {
        OutError = FString::Printf(TEXT("Usage: %s [speed], a speed from 1 to %d (\"cheat %s 3000\")"), Name.c_str(), static_cast<int32_t>(MaxSpeed), Name.c_str());
        return true;
    }
    OutCommand = FString(Words[0].c_str());
    OutSpeed = Speed;
    return true;
}

void UFortCheatManager::OnCheatMovement(ACharacter* Character, ECheatMovement Movement)
{
    const float Speed = Movement == ECheatMovement::Walk ? 0.0f : CheatMoveSpeed;
    CheatMoveSpeed = 0.0f;
    AFortPawn* const Pawn = Cast<AFortPawn>(static_cast<UObject*>(Character));
    if (!Pawn)
    {
        return;
    }
    const float OwnSpeed = Pawn->SetCheatMoveSpeed(Speed);
    if (Speed > 0.0f)
    {
        UE_LOG(LogFortCheats, Log, TEXT("%s runs at %.0f while it flies (its own speed %.0f)"), *GetNameSafe(Pawn), Speed, OwnSpeed);
    }

#if WITH_SERVER_CODE
    AController* const Controller = Pawn->Controller;
    AFortPlayerController* const PlayerController = Cast<AFortPlayerController>(static_cast<UObject*>(Controller));
    if (PlayerController && Speed > 0.0f)
    {
        PlayerController->ClientMessage(OwnSpeed > 0.0f
            ? FString::Printf(TEXT("Flying at speed %d (your own: %d); \"cheat %s\" without a speed for your own"), static_cast<int32_t>(Speed), static_cast<int32_t>(OwnSpeed), Movement == ECheatMovement::Ghost ? TEXT("ghost") : TEXT("fly"))
            : FString(TEXT("This pawn has no speed to change; it moves at its own")));
    }
#endif
}

AFortPlayerController* UFortCheatManager::FindCheatTarget(const UObject* WorldContextObject, const FString& Name, FString& OutError)
{
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
    if (!GameState)
    {
        OutError = TEXT("No players to pick from");
        return nullptr;
    }

    const std::wstring Wanted = Lower(Name.ToStdString());
    std::vector<std::pair<AFortPlayerController*, FString>> Exact;
    std::vector<std::pair<AFortPlayerController*, FString>> Prefixed;
    const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
    for (APlayerState* PlayerState : PlayerArray)
    {
        AFortPlayerController* Controller = PlayerState ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(PlayerState->GetOwner())) : nullptr;
        if (!Controller)
        {
            continue;
        }
        const FString& PlayerName = PlayerState->PlayerName;
        const std::wstring Candidate = Lower(PlayerName.ToStdString());
        if (Candidate == Wanted)
        {
            Exact.emplace_back(Controller, PlayerName);
        }
        else if (Candidate.starts_with(Wanted))
        {
            Prefixed.emplace_back(Controller, PlayerName);
        }
    }

    const std::vector<std::pair<AFortPlayerController*, FString>>& Matches = !Exact.empty() ? Exact : Prefixed;
    if (Matches.size() == 1)
    {
        return Matches.front().first;
    }
    if (Matches.empty())
    {
        OutError = FString::Printf(TEXT("No player called '%s'"), *Name);
        return nullptr;
    }
    std::wstring Names;
    for (const auto& [Controller, PlayerName] : Matches)
    {
        Names += (Names.empty() ? L"" : L", ") + PlayerName.ToStdString();
    }
    OutError = FString::Printf(TEXT("'%s' matches %d players (%s); give more of the name"), *Name, static_cast<int32_t>(Matches.size()), Names.c_str());
    return nullptr;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveWeapon)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_WeaponName);
    P_GET_PROPERTY(UIntProperty, Z_Param_RequestedLevel);
    P_GET_PROPERTY(UIntProperty, Z_Param_Count);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapon(Z_Param_WeaponName, Z_Param_RequestedLevel, Z_Param_Count);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAmmo)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_AmmoNameAndAmmount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveAmmo(Z_Param_AmmoNameAndAmmount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveTrap)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_TrapName);
    P_GET_PROPERTY(UIntProperty, Z_Param_RequestedLevel);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumTrapsToGive);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveTrap(Z_Param_TrapName, Z_Param_RequestedLevel, Z_Param_NumTrapsToGive);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveGadget)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_GadgetName);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveGadgets(Z_Param_GadgetName, 1, Z_Param_ItemLevel);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveGadgets)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_GadgetName);
    P_GET_PROPERTY(UIntProperty, Z_Param_Quantity);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveGadgets(Z_Param_GadgetName, Z_Param_Quantity, Z_Param_ItemLevel);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveSpecificItem)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_ItemPath);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemAmount);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveSpecificItem(Z_Param_ItemPath, Z_Param_ItemAmount, Z_Param_ItemLevel);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveResources)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Amount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveResources(Z_Param_Amount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveWood)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Amount);
    P_FINISH;
    P_NATIVE_BEGIN;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    static_cast<UFortCheatManager*>(Context)->GiveResource(GameData ? static_cast<UFortWorldItemDefinition*>(GameData->WoodItemDefinition) : nullptr, Z_Param_Amount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveStone)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Amount);
    P_FINISH;
    P_NATIVE_BEGIN;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    static_cast<UFortCheatManager*>(Context)->GiveResource(GameData ? static_cast<UFortWorldItemDefinition*>(GameData->StoneItemDefinition) : nullptr, Z_Param_Amount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveMetal)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Amount);
    P_FINISH;
    P_NATIVE_BEGIN;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    static_cast<UFortCheatManager*>(Context)->GiveResource(GameData ? static_cast<UFortWorldItemDefinition*>(GameData->MetalItemDefinition) : nullptr, Z_Param_Amount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveWeapons)
{
    P_GET_PROPERTY(UNameProperty, Z_Param_Tag);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(Z_Param_Tag);
    P_NATIVE_END;
}

// GiveAllMelee, GiveAllPistols, GiveAllRifles and GiveAllShotguns are GiveWeapons with the tag their bodies still name
// (1.7.2 0x7CFB00-0x7CFB60); GiveAllWeapons passes None.
DEFINE_FUNCTION(UFortCheatManager::execGiveAllMelee)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(FName(L"Weapon.Melee"));
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllPistols)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(FName(L"Weapon.Ranged.Pistol"));
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllRifles)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(FName(L"Weapon.Ranged.Assault"));
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllShotguns)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(FName(L"Weapon.Ranged.Shotgun"));
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllWeapons)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveWeapons(FName());
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllTraps)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_NumItems);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveAllTraps(Z_Param_NumItems);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllConsumables)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_NumItems);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveAllConsumables(Z_Param_NumItems);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveAllIngredients)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_NumIngredients);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveAllIngredients(Z_Param_NumIngredients);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveUsefulThings)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_NumOfUsefulThings);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveUsefulThings(Z_Param_NumOfUsefulThings);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execWannaGun)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->WannaGun();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveCheatInventory)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveCheatInventory();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execTeleportToLocation)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_X);
    P_GET_PROPERTY(UFloatProperty, Z_Param_Y);
    P_GET_PROPERTY(UFloatProperty, Z_Param_Z);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->TeleportToLocation(Z_Param_X, Z_Param_Y, Z_Param_Z);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSetSafeZoneLocationToPlayer)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SetSafeZoneLocationToPlayer();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSetSafeZoneRadius)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_NewRadius);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SetSafeZoneRadius(Z_Param_NewRadius);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execRespawnPlayerServer)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->RespawnPlayerServer();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execTestInfJumps)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->TestInfJumps();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execGiveBluGlo)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Amount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->GiveBluGlo(Z_Param_Amount);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSpawnAIGroup)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_SpawnGroupName);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SpawnAIGroup(Z_Param_SpawnGroupName, false);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSpawnAIGroupFromExternalSpawner)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_SpawnGroupName);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SpawnAIGroup(Z_Param_SpawnGroupName, true);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSendMissionEvent)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_MissionEventName);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SendMissionEvent(Z_Param_MissionEventName);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execLoadMission)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_MissionName);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->LoadMission(Z_Param_MissionName);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execTeleportToMissionActor)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->TeleportToMissionActor();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execDumpAllActiveMissions)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->DumpAllActiveMissions();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSimulateLoot)
{
    P_GET_PROPERTY(UNameProperty, Z_Param_TierGroup);
    P_GET_PROPERTY(UIntProperty, Z_Param_LootTier);
    P_GET_PROPERTY(UIntProperty, Z_Param_LootLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SimulateLoot(Z_Param_TierGroup, Z_Param_LootTier, Z_Param_LootLevel);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execToggleInvincibleCornerstones)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->ToggleInvincibleCornerstones();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execSetCapturePercentage)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_NewCapturePercentage);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->SetCapturePercentage(Z_Param_NewCapturePercentage);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortCheatManager::execCountPlayerBuiltBuildingActors)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortCheatManager*>(Context)->CountPlayerBuiltBuildingActors();
    P_NATIVE_END;
}

void UFortCheatManager::HookNatives()
{
    // Each of these is stripped on 1.2, 1.7.2 and 1.8 alike (Cheats.md): GiveWeapon, GiveAmmo, GiveTrap, GiveGadget(s),
    // GiveSpecificItem only free their string (1.7.2 0x7D01A0), GiveResources/Wood/Stone/Metal, GiveWeapons, GiveAllTraps,
    // GiveAllConsumables, GiveAllIngredients, RespawnPlayerServer and WannaGun are the shared "ret 0", and the rest call one of
    // those or something else compiled out (TeleportToLocation a virtual in Battle Royale, SetSafeZoneLocationToPlayer and
    // SetSafeZoneRadius the game mode's setters, GiveCheatInventory its load callback). The second batch (TestInfJumps,
    // GiveBluGlo, SpawnAIGroup(FromExternalSpawner), SendMissionEvent, LoadMission, TeleportToMissionActor,
    // DumpAllActiveMissions, SimulateLoot, ToggleInvincibleCornerstones, SetCapturePercentage) is empty or only frees or
    // reads its argument the same way. A build without one of them (1.1 and 1.2 have no Battle Royale safe zone cheats,
    // CoreV2 #118; 1.1 no TestInfJumps or DumpAllActiveMissions) skips it instead of stopping the server.
    // CountPlayerBuiltBuildingActors keeps its body, but it only writes to the server's log, so Core's answers the player.
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    for (const auto& [Function, Native, Name] : {
             std::tuple{ FindStaticFunction_GiveWeapon(), &execGiveWeapon, L"UFortCheatManager::GiveWeapon" },
             std::tuple{ FindStaticFunction_GiveAmmo(), &execGiveAmmo, L"UFortCheatManager::GiveAmmo" },
             std::tuple{ FindStaticFunction_GiveTrap(), &execGiveTrap, L"UFortCheatManager::GiveTrap" },
             std::tuple{ FindStaticFunction_GiveGadget(), &execGiveGadget, L"UFortCheatManager::GiveGadget" },
             std::tuple{ FindStaticFunction_GiveGadgets(), &execGiveGadgets, L"UFortCheatManager::GiveGadgets" },
             std::tuple{ FindStaticFunction_GiveSpecificItem(), &execGiveSpecificItem, L"UFortCheatManager::GiveSpecificItem" },
             std::tuple{ FindStaticFunction_GiveResources(), &execGiveResources, L"UFortCheatManager::GiveResources" },
             std::tuple{ FindStaticFunction_GiveWood(), &execGiveWood, L"UFortCheatManager::GiveWood" },
             std::tuple{ FindStaticFunction_GiveStone(), &execGiveStone, L"UFortCheatManager::GiveStone" },
             std::tuple{ FindStaticFunction_GiveMetal(), &execGiveMetal, L"UFortCheatManager::GiveMetal" },
             std::tuple{ FindStaticFunction_GiveWeapons(), &execGiveWeapons, L"UFortCheatManager::GiveWeapons" },
             std::tuple{ FindStaticFunction_GiveAllMelee(), &execGiveAllMelee, L"UFortCheatManager::GiveAllMelee" },
             std::tuple{ FindStaticFunction_GiveAllPistols(), &execGiveAllPistols, L"UFortCheatManager::GiveAllPistols" },
             std::tuple{ FindStaticFunction_GiveAllRifles(), &execGiveAllRifles, L"UFortCheatManager::GiveAllRifles" },
             std::tuple{ FindStaticFunction_GiveAllShotguns(), &execGiveAllShotguns, L"UFortCheatManager::GiveAllShotguns" },
             std::tuple{ FindStaticFunction_GiveAllWeapons(), &execGiveAllWeapons, L"UFortCheatManager::GiveAllWeapons" },
             std::tuple{ FindStaticFunction_GiveAllTraps(), &execGiveAllTraps, L"UFortCheatManager::GiveAllTraps" },
             std::tuple{ FindStaticFunction_GiveAllConsumables(), &execGiveAllConsumables, L"UFortCheatManager::GiveAllConsumables" },
             std::tuple{ FindStaticFunction_GiveAllIngredients(), &execGiveAllIngredients, L"UFortCheatManager::GiveAllIngredients" },
             std::tuple{ FindStaticFunction_GiveUsefulThings(), &execGiveUsefulThings, L"UFortCheatManager::GiveUsefulThings" },
             std::tuple{ FindStaticFunction_GiveCheatInventory(), &execGiveCheatInventory, L"UFortCheatManager::GiveCheatInventory" },
             std::tuple{ FindStaticFunction_WannaGun(), &execWannaGun, L"UFortCheatManager::WannaGun" },
             std::tuple{ FindStaticFunction_TeleportToLocation(), &execTeleportToLocation, L"UFortCheatManager::TeleportToLocation" },
             std::tuple{ FindStaticFunction_SetSafeZoneLocationToPlayer(), &execSetSafeZoneLocationToPlayer, L"UFortCheatManager::SetSafeZoneLocationToPlayer" },
             std::tuple{ FindStaticFunction_SetSafeZoneRadius(), &execSetSafeZoneRadius, L"UFortCheatManager::SetSafeZoneRadius" },
             std::tuple{ FindStaticFunction_RespawnPlayerServer(), &execRespawnPlayerServer, L"UFortCheatManager::RespawnPlayerServer" },
             std::tuple{ FindStaticFunction_TestInfJumps(), &execTestInfJumps, L"UFortCheatManager::TestInfJumps" },
             std::tuple{ FindStaticFunction_GiveBluGlo(), &execGiveBluGlo, L"UFortCheatManager::GiveBluGlo" },
             std::tuple{ FindStaticFunction_SpawnAIGroup(), &execSpawnAIGroup, L"UFortCheatManager::SpawnAIGroup" },
             std::tuple{ FindStaticFunction_SpawnAIGroupFromExternalSpawner(), &execSpawnAIGroupFromExternalSpawner, L"UFortCheatManager::SpawnAIGroupFromExternalSpawner" },
             std::tuple{ FindStaticFunction_SendMissionEvent(), &execSendMissionEvent, L"UFortCheatManager::SendMissionEvent" },
             std::tuple{ FindStaticFunction_LoadMission(), &execLoadMission, L"UFortCheatManager::LoadMission" },
             std::tuple{ FindStaticFunction_TeleportToMissionActor(), &execTeleportToMissionActor, L"UFortCheatManager::TeleportToMissionActor" },
             std::tuple{ FindStaticFunction_DumpAllActiveMissions(), &execDumpAllActiveMissions, L"UFortCheatManager::DumpAllActiveMissions" },
             std::tuple{ FindStaticFunction_SimulateLoot(), &execSimulateLoot, L"UFortCheatManager::SimulateLoot" },
             std::tuple{ FindStaticFunction_ToggleInvincibleCornerstones(), &execToggleInvincibleCornerstones, L"UFortCheatManager::ToggleInvincibleCornerstones" },
             std::tuple{ FindStaticFunction_SetCapturePercentage(), &execSetCapturePercentage, L"UFortCheatManager::SetCapturePercentage" },
             std::tuple{ FindStaticFunction_CountPlayerBuiltBuildingActors(), &execCountPlayerBuiltBuildingActors, L"UFortCheatManager::CountPlayerBuiltBuildingActors" },
         })
    {
        if (Function)
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }
    Finder::ResolveAssetManagerLive();
}

