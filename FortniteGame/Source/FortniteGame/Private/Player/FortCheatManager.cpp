#include "pch.h"
#include "Player/FortCheatManager.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIEncounterInfo.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Building/BuildingTrap.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
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
#include "GameFramework/Pawn.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Finder.h"
#include "GameThread.h"
#include "GameplayEffect.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionInfo.h"
#include "Missions/FortMissionLibrary.h"
#include "Missions/FortMissionManager.h"
#include "Missions/FortObjectiveBase.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <variant>
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

    // The line's words, split at spaces and tabs.
    std::vector<std::wstring> SplitWords(std::wstring_view Line)
    {
        std::vector<std::wstring> Words;
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
        return Words;
    }

    // Classes the editor and blueprint compiler leave behind, which nothing spawns.
    bool IsLeftoverClass(const std::wstring& Name)
    {
        return Name.starts_with(L"SKEL_") || Name.starts_with(L"REINST_") || Name.starts_with(L"TRASHCLASS_") || Name.starts_with(L"PLACEHOLDER-CLASS");
    }

    // The actor class SpawnActor names, the way the native Summon finds one (1.7.2 0x1FE9A90, FindObject<UClass>), and
    // loading it when it isn't yet:
    // - a path, "/Script/Engine.StaticMeshActor", "/Game/Building/ActorBlueprints/Containers/Tiered_Chest_6_Parent.Tiered_Chest_6_Parent_C",
    //   or the package alone (".../Tiered_Chest_6_Parent", its _C class), loaded if it isn't;
    // - a loaded class's name, with or without its _C ("StaticMeshActor", "Tiered_Chest_6_Parent");
    // - else part of a loaded actor class's name, the shortest that has it.
    UClass* ResolveActorClass(std::wstring Query)
    {
        while (!Query.empty() && (Query.front() == L'"' || Query.front() == L'\''))
        {
            Query.erase(Query.begin());
        }
        while (!Query.empty() && (Query.back() == L'"' || Query.back() == L'\''))
        {
            Query.pop_back();
        }
        if (Query.empty())
        {
            return nullptr;
        }

        if (Query.front() == L'/')
        {
            std::vector<std::wstring> Paths;
            const size_t Slash = Query.rfind(L'/');
            if (Query.find(L'.', Slash) == std::wstring::npos)
            {
                const std::wstring AssetName = Query.substr(Slash + 1);
                Paths = { Query + L"." + AssetName + L"_C", Query + L"." + AssetName };
            }
            else
            {
                Paths.push_back(Query);
                if (!Lower(Query).ends_with(L"_c"))
                {
                    Paths.push_back(Query + L"_C");
                }
            }
            for (const std::wstring& Path : Paths)
            {
                if (UClass* Class = FindObject<UClass>(nullptr, Path.c_str()))
                {
                    return Class;
                }
            }
            for (const std::wstring& Path : Paths)
            {
                if (UClass* Class = LoadObject<UClass>(nullptr, Path.c_str(), nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet)))
                {
                    return Class;
                }
            }
            return nullptr;
        }

        // A short name has no package: "Engine.StaticMeshActor" is StaticMeshActor.
        const size_t Dot = Query.rfind(L'.');
        const std::wstring Name = Dot == std::wstring::npos ? Query : Query.substr(Dot + 1);
        if (Name.empty())
        {
            return nullptr;
        }
        for (const std::wstring& Candidate : { Name, Name + L"_C" })
        {
            if (UClass* Class = FindObject<UClass>(ANY_PACKAGE, Candidate.c_str()))
            {
                return Class;
            }
        }

        const std::wstring Wanted = Simplified(Name);
        UClass* Best = nullptr;
        size_t BestLength = 0;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (!Class->IsChildOf(AActor::StaticClass()))
            {
                continue;
            }
            const std::wstring ClassName = Class->GetName().ToStdString();
            if (IsLeftoverClass(ClassName) || Simplified(ClassName).find(Wanted) == std::wstring::npos)
            {
                continue;
            }
            if (!Best || ClassName.size() < BestLength)
            {
                Best = Class;
                BestLength = ClassName.size();
            }
        }
        return Best;
    }

    FString PlayerNameOf(const AController* Controller)
    {
        const APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(Controller->PlayerState) : nullptr;
        return PlayerState ? PlayerState->GetPlayerName() : GetNameSafe(Controller);
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

    // "cheat corecheats": the commands Core supports, by category, one line each (#155: short, so the list reads at a
    // glance). Name is the exec function's (left out when the build has none, null for Core's own); Usage is the syntax
    // after "cheat ", Description a few words.
    struct FCoreCheat
    {
        const wchar_t* Category;
        const wchar_t* Name;
        const wchar_t* Usage;
        const wchar_t* Description;
        ECheatMode Mode;
    };

    constexpr FCoreCheat CoreCheats[] = {
        { L"Items", L"GiveWeapon", L"giveweapon <name> [level] [count]", L"a weapon or consumable", ECheatMode::Both },
        { L"Items", L"GiveAmmo", L"giveammo <name> [amount]", L"ammo", ECheatMode::Both },
        { L"Items", L"GiveTrap", L"givetrap <name> [level] [count]", L"a trap", ECheatMode::Both },
        { L"Items", L"GiveGadget", L"givegadget <name> [level]", L"a gadget", ECheatMode::Both },
        { L"Items", L"GiveSpecificItem", L"givespecificitem <path or name> [count] [level]", L"any item", ECheatMode::Both },
        { L"Items", L"GiveResources", L"giveresources [amount]", L"wood, stone, metal (givewood...)", ECheatMode::Both },
        { L"Items", L"GiveAllWeapons", L"giveallweapons", L"every weapon (giveallrifles...)", ECheatMode::Both },
        { L"Items", L"GiveAllTraps", L"givealltraps [count]", L"every trap", ECheatMode::Both },
        { L"Items", L"GiveAllConsumables", L"giveallconsumables [count]", L"every consumable", ECheatMode::BattleRoyale },
        { L"Items", L"GiveAllIngredients", L"giveallingredients [count]", L"every ingredient", ECheatMode::SaveTheWorld },
        { L"Items", L"GiveBluGlo", L"givebluglo [amount]", L"BluGlo", ECheatMode::SaveTheWorld },
        { L"Items", L"GiveCheatInventory", L"givecheatinventory", L"the TestGod rifle", ECheatMode::Both },
        { L"Items", L"GiveAthenaFullKit", L"giveathenafullkit", L"a full BR kit", ECheatMode::BattleRoyale },
        { L"Items", L"Badass", L"badass", L"god, build free, STW weapons", ECheatMode::SaveTheWorld },
        { L"Items", L"ListWeapons", L"listweapons", L"item names (listammo, listtraps)", ECheatMode::Both },
        { L"Player", L"God", L"god", L"invulnerable, toggles", ECheatMode::Both },
        { L"Player", nullptr, L"unlimitedhealth [on | off]", L"take no damage", ECheatMode::Both },
        { L"Player", L"ToggleInfiniteStamina", L"infinitestamina", L"stamina never runs out, toggles", ECheatMode::SaveTheWorld },
        { L"Player", L"Pain", L"pain <amount>", L"hurt yourself", ECheatMode::Both },
        { L"Player", L"Fly", L"fly [speed]", L"fly (ghost: through walls; walk: stop)", ECheatMode::Both },
        { L"Player", L"TestInfJumps", L"testinfjumps", L"jump in the air, toggles", ECheatMode::Both },
        { L"Player", nullptr, L"speed <multiplier | reset>", L"move faster or slower", ECheatMode::Both },
        { L"Player", nullptr, L"maxhealth <value | reset>", L"maximum health, healed to it", ECheatMode::Both },
        { L"Player", nullptr, L"healthpercent <0-100>", L"health as a share of the maximum", ECheatMode::Both },
        { L"Player", L"ChangeSize", L"changesize <scale>", L"your size", ECheatMode::Both },
        { L"Player", L"BuildFree", L"buildfree", L"free building, toggles (craftfree)", ECheatMode::Both },
        { L"Player", nullptr, L"infinitedurability [on | off]", L"items don't wear out", ECheatMode::Both },
        { L"Player", nullptr, L"nocooldown [on | off]", L"no ability or gadget cooldowns, charges stay full", ECheatMode::Both },
        { L"Player", L"DBNO", L"dbno", L"knock yourself down (revivefromdbno)", ECheatMode::Both },
        { L"Player", L"RespawnPlayerServer", L"respawnplayerserver", L"respawn when dead", ECheatMode::Both },
        { L"Player", L"BackpackSetSize", L"backpacksetsize <slots>", L"backpack size", ECheatMode::Both },
        { L"Player", L"BuildWith", L"buildwith <wood | stone | metal>", L"building material", ECheatMode::Both },
        { L"Player", nullptr, L"skin <HID or CID | reset>", L"wear a hero's or outfit's look", ECheatMode::Both },
        { L"Player", nullptr, L"disguise <class, path or mesh> [scale]", L"become a prop or pawn (undisguise)", ECheatMode::Both },
        { L"Player", nullptr, L"disguiseview <back | hide | normal>", L"how you see your disguise", ECheatMode::Both },
        { L"Player", nullptr, L"dance [EID]", L"play an emote", ECheatMode::Both },
        { L"Weapons", nullptr, L"infinitemag [on | off]", L"never reload, never run out", ECheatMode::Both },
        { L"Weapons", nullptr, L"firerate <multiplier | reset>", L"guns fire faster", ECheatMode::Both },
        { L"Weapons", nullptr, L"weapondamage <multiplier | reset>", L"weapons hit harder", ECheatMode::Both },
        { L"Weapons", nullptr, L"bloom [on | off]", L"no spread (your game only)", ECheatMode::Both },
        { L"Weapons", nullptr, L"recoil [on | off]", L"no recoil (your game only)", ECheatMode::Both },
        { L"Weapons", nullptr, L"repair [all]", L"full durability", ECheatMode::Both },
        { L"Movement", L"Teleport", L"teleport", L"to where you look", ECheatMode::Both },
        { L"Movement", L"TeleportToLocation", L"teleporttolocation <x> <y> <z>", L"to a location", ECheatMode::Both },
        { L"Movement", nullptr, L"teleportto <player>", L"next to a player", ECheatMode::Both },
        { L"Movement", nullptr, L"bring <player | all>", L"a player to you", ECheatMode::Both },
        { L"Movement", nullptr, L"tpactor <class or name> [n]", L"onto the nearest such actor", ECheatMode::Both },
        { L"Movement", nullptr, L"possessplayer <player>", L"control their pawn (unpossess)", ECheatMode::Both },
        { L"World", L"Summon", L"summon <class>", L"spawn an actor", ECheatMode::Both },
        { L"World", nullptr, L"spawnactor <class or path> [scale]", L"spawn an actor at a scale", ECheatMode::Both },
        { L"World", nullptr, L"copytargetpath [class | mesh | name]", L"copy what you look at", ECheatMode::Both },
        { L"World", L"Slomo", L"slomo <speed>", L"game speed, for everyone", ECheatMode::Both },
        { L"World", L"DestroyTarget", L"destroytarget", L"destroy what you look at", ECheatMode::Both },
        { L"World", L"DamageTarget", L"damagetarget <amount>", L"damage what you look at", ECheatMode::Both },
        { L"World", L"DestroyAll", L"destroyall <class>", L"destroy every actor of a class", ECheatMode::Both },
        { L"World", L"DestroyPawns", L"destroypawns <class>", L"destroy every pawn of a class", ECheatMode::Both },
        { L"World", L"MakeGods", L"makegods <class>", L"pawns of a class can't be hurt, toggles", ECheatMode::Both },
        { L"World", L"MakeBuildingActorsGod", L"makebuildingactorsgod", L"buildings can't be damaged, toggles", ECheatMode::Both },
        { L"World", L"KillPawns", L"killpawns [class]", L"kill every pawn", ECheatMode::Both },
        { L"World", L"SimulateLoot", L"simulateloot <group> <tier> <level>", L"roll a loot group", ECheatMode::Both },
        { L"Storm", L"SetSafeZoneLocationToPlayer", L"safezonelocationtoplayer", L"storm closes on you", ECheatMode::BattleRoyale },
        { L"Storm", L"SetSafeZoneRadius", L"safezoneradius <radius>", L"storm circle radius", ECheatMode::BattleRoyale },
        { L"Save the World", L"SpawnAIGroup", L"spawnaigroup <group>", L"spawn husks", ECheatMode::SaveTheWorld },
        { L"Save the World", L"DisableAI", L"disableai", L"stop the AI (toggleaispawning)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"MakeAIsGod", L"makeaisgod", L"husks can't be hurt, toggles", ECheatMode::SaveTheWorld },
        { L"Save the World", L"MissionForceSuccess", L"missionforcesuccess", L"win the mission (missionforcefailure)", ECheatMode::SaveTheWorld },
        { L"Save the World", L"MissionForcePrimarySuccess", L"missionforceprimarysuccess 1", L"complete the main objective", ECheatMode::SaveTheWorld },
        { L"Save the World", L"ClearRespawnTimer", L"clearrespawntimer", L"respawn without waiting", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SetWorldDaysElapsed", L"worlddayselapsed <days>", L"time of day", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SetGameDifficultyOverride", L"gamedifficultyoverride <value>", L"enemy difficulty", ECheatMode::SaveTheWorld },
        { L"Save the World", L"LoadMission", L"loadmission <mission>", L"load a mission", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SendMissionEvent", L"sendmissionevent <event>", L"send a mission event", ECheatMode::SaveTheWorld },
        { L"Save the World", L"TeleportToMissionActor", L"teleporttomissionactor", L"to the objective", ECheatMode::SaveTheWorld },
        { L"Save the World", L"DumpAllActiveMissions", L"dumpallactivemissions", L"list the missions", ECheatMode::SaveTheWorld },
        { L"Save the World", L"SetWorldLevel", L"worldlevel <level>", L"the world level", ECheatMode::SaveTheWorld },
        { L"Save the World", L"ToggleIndestructibleKeep", L"indestructiblekeep", L"objective can't break", ECheatMode::SaveTheWorld },
        { L"Save the World", L"CountPlayerBuiltBuildingActors", L"countplayerbuiltbuildingactors", L"structure count", ECheatMode::SaveTheWorld },
        { L"Other", L"CheatScript", L"cheatscript <section>", L"run a list of cheats", ECheatMode::Both },
        { L"Other", L"ListCheats", L"listcheats", L"every exec function", ECheatMode::Both },
        { L"Other", nullptr, L"corecheats [filter | all]", L"this list", ECheatMode::Both },
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

UClass* UFortCheatManager::FindActorClass(const std::wstring& Query)
{
    return ResolveActorClass(Query);
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
        if (UFortWorldItemDefinition* Item = SoftElementAt(Items, Index).LoadSynchronous())
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
        return;
    }
    Reply(FString::Printf(TEXT("Teleported to X=%.1f Y=%.1f Z=%.1f"), X, Y, Z));
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
        Reply(TEXT("RespawnPlayerServer needs the server's game mode"));
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

    if (Shown.empty())
    {
        Reply(TEXT("Nothing matches; \"cheat corecheats all\" lists every one"));
        return;
    }
    Reply(FString::Printf(TEXT("Core cheats (%d): \"cheat <command>\"; \"set\" and \"toggle\" are optional; end with /Player/ to target someone"),
        static_cast<int32_t>(Shown.size())));
    const wchar_t* Category = nullptr;
    for (const FCoreCheat* Cheat : Shown)
    {
        if (!Category || std::wstring_view(Category) != Cheat->Category)
        {
            Category = Cheat->Category;
            Reply(FString::Printf(TEXT("[%s]"), Category));
        }
        Reply(FString::Printf(TEXT("  %s - %s"), Cheat->Usage, Cheat->Description));
    }
}

void UFortCheatManager::SpawnActor(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // PongooDev/Core's SpawnActor, with a scale (#132). The scale goes on the spawn transform, so a blueprint's
    // construction script builds the actor at that size, and on the root component after (SetActorScale3D). The actor's
    // first bunch carries its scale to the clients (UPackageMapClient::SerializeNewActor, 1.7.2 0x223ABE0), so they spawn
    // it at that size too.
    constexpr float MinScale = 0.01f;
    constexpr float MaxScale = 100.0f;
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const FString Usage = TEXT("Usage: spawnactor <class or path> [scale | x y z], a scale from 0.01 to 100 (\"spawnactor Tiered_Chest_6_Parent 3\", \"spawnactor Tiered_Chest_6_Parent 1 1 4\")");
    if (Words.empty() || Words.size() == 3 || Words.size() > 4)
    {
        Reply(Usage);
        return;
    }
    float Scale[3] = { 1.0f, 1.0f, 1.0f };
    for (size_t Index = 1; Index < Words.size(); ++Index)
    {
        wchar_t* End = nullptr;
        const float Value = std::wcstof(Words[Index].c_str(), &End);
        if (!End || *End != L'\0' || !std::isfinite(Value) || Value < MinScale || Value > MaxScale)
        {
            Reply(Usage);
            return;
        }
        Scale[Index - 1] = Value;
    }
    if (Words.size() == 2)
    {
        Scale[1] = Scale[0];
        Scale[2] = Scale[0];
    }
    const FVector Scale3D(Scale[0], Scale[1], Scale[2]);
    const FString ScaleText = Words.size() == 2 ? FString::Printf(TEXT("%g"), Scale[0]) : FString::Printf(TEXT("%g x %g x %g"), Scale[0], Scale[1], Scale[2]);

    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    if (!Pawn)
    {
        Reply(TEXT("SpawnActor needs a pawn to spawn the actor in front of"));
        return;
    }
    UClass* Class = FindActorClass(Words[0]);
    if (!Class)
    {
        Reply(FString::Printf(TEXT("No class called '%s' (give its path, /Game/.../X.X_C, to load one that isn't yet)"), Words[0].c_str()));
        return;
    }
    if (!Class->IsChildOf(AActor::StaticClass()))
    {
        Reply(FString::Printf(TEXT("%s isn't an actor class"), *Class->GetPathName()));
        return;
    }

    // In front of the player, a pawn standing as they do and anything else on the floor there (#147: at the player's
    // feet it hung in the air past a ramp or a cliff edge), facing the way they face. A pawn moves up out of what it
    // would stand in.
    const bool bPawn = Class->IsChildOf(APawn::StaticClass());
    FVector PawnOrigin;
    FVector PawnExtent;
    Pawn->GetActorBounds(true, PawnOrigin, PawnExtent);
    // The floor under a point, traced down from the player's head height; their feet's height if there is none.
    const auto OnFloor = [&](FVector At)
    {
        if (PawnExtent.Z > 0.0f)
        {
            At.Z = PawnOrigin.Z - PawnExtent.Z;
        }
        FVector Floor;
        if (UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(At.X, At.Y, PawnOrigin.Z + PawnExtent.Z), FVector(At.X, At.Y, At.Z - 100000.0f), 0, false, true, Floor))
        {
            At.Z = Floor.Z;
        }
        return At;
    };
    FVector Location = InFrontOf(Pawn, 300.0f);
    if (!bPawn)
    {
        Location = OnFloor(Location);
    }
    const FQuat Rotation = FRotator(0.0f, Pawn->GetActorRotation().Yaw, 0.0f).Quaternion();
    const ESpawnActorCollisionHandlingMethod Collision = bPawn ? ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn : ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    const auto Spawn = [&](const FVector& At) -> AActor*
    {
        FTransform SpawnTransform;
        SpawnTransform.Rotation = Rotation;
        SpawnTransform.Translation = At;
        SpawnTransform.Scale3D = Scale3D;
        AActor* Spawned = UGameplayStatics::BeginDeferredActorSpawnFromClass(Pawn, Class, SpawnTransform, Collision);
        Spawned = Spawned ? UGameplayStatics::FinishSpawningActor(Spawned, SpawnTransform) : nullptr;
        if (Spawned && Spawned->GetRootComponent())
        {
            Spawned->SetActorScale3D(Scale3D);
        }
        return Spawned;
    };
    AActor* Actor = Spawn(Location);
    if (!Actor)
    {
        Reply(FString::Printf(TEXT("Couldn't spawn %s (an abstract class, or one that can't be spawned here)"), *Class->GetPathName()));
        return;
    }

    const bool bScaled = Actor->GetRootComponent() != nullptr;
    if (bScaled)
    {
        // Something bigger than the 300 units in front moves out until it clears the player. A root that can't move
        // (static mobility) is spawned again there instead; nothing has replicated it yet.
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(false, Origin, Extent);
        const float Radius = std::sqrt(Extent.X * Extent.X + Extent.Y * Extent.Y);
        const float Distance = Radius + 150.0f;
        if (Distance > 300.0f && Radius < 50000.0f)
        {
            const FVector Center = InFrontOf(Pawn, Distance);
            const FVector ActorLocation = Actor->GetTransform().GetLocation();
            const FVector Clear(Center.X - (Origin.X - ActorLocation.X), Center.Y - (Origin.Y - ActorLocation.Y), ActorLocation.Z);
            if (!Actor->SetActorLocation(Clear, false, true))
            {
                if (AActor* Moved = Spawn(Clear))
                {
                    Actor->K2_DestroyActor();
                    Actor = Moved;
                }
            }
        }
    }

    // The clients only see an actor that replicates; most objects (a StaticMeshActor, a prop) don't by default.
    const bool bReplicatedByDefault = Actor->bReplicates;
    if (!bReplicatedByDefault)
    {
        Actor->SetReplicates(true);
    }
    const bool bReplicates = Actor->bReplicates;
    Actor->ForceNetUpdate();

    const FVector Actual = Actor->GetActorScale3D();
    UE_LOG(LogFortCheats, Log, TEXT("SpawnActor: %s spawned %s (%s) at scale %g %g %g, replicates %d (by default %d)"), *GetNameSafe(PlayerController),
        *Actor->GetName(), *Class->GetPathName(), Actual.X, Actual.Y, Actual.Z, bReplicates ? 1 : 0, bReplicatedByDefault ? 1 : 0);
    Reply(FString::Printf(TEXT("Spawned %s (%s)%s%s"), *Actor->GetName(), *Class->GetPathName(),
        bScaled ? *FString::Printf(TEXT(" at scale %s"), *ScaleText) : TEXT(", which has no root component to scale"),
        !bReplicates ? TEXT("; it doesn't replicate, so only the server has it") : bReplicatedByDefault ? TEXT("") : TEXT("; Core made it replicate, as it didn't")));
#endif
}

void UFortCheatManager::TpActor(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // #146. The actors are those whose class (or a parent class) has the name or part of it, or whose own name has it;
    // a path means that class and its subclasses. Index 1 is the nearest, 2 the next, and so on.
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const FString Usage = TEXT("Usage: tpactor <class, name or path> [n], n 1 for the nearest (the default), 2 for the next (\"tpactor Tiered_Chest\", \"tpactor BuildingContainer 3\")");
    if (Words.empty() || Words.size() > 2)
    {
        Reply(Usage);
        return;
    }
    int32_t Wanted = 1;
    if (Words.size() == 2)
    {
        wchar_t* End = nullptr;
        const long Value = std::wcstol(Words[1].c_str(), &End, 10);
        if (!End || *End != L'\0' || Value < 1 || Value > 1000000)
        {
            Reply(Usage);
            return;
        }
        Wanted = static_cast<int32_t>(Value);
    }

    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    if (!Pawn)
    {
        Reply(TEXT("No pawn to teleport"));
        return;
    }
    if (!UGameplayStatics::FindStaticFunction_GetAllActorsOfClass())
    {
        Reply(TEXT("TpActor needs GameplayStatics.GetAllActorsOfClass, which this build doesn't have"));
        return;
    }

    const std::wstring& Query = Words[0];
    std::vector<AActor*> Matches;
    const auto IsCandidate = [&](const AActor* Actor)
    {
        // Not the player's own pawn or what it carries, nor what has no place in the world (infos, controllers).
        return Actor && Actor != Pawn && Actor->GetRootComponent() && Actor->GetOwner() != Pawn && Actor->GetOwner() != PlayerController;
    };
    if (Query.find(L'/') != std::wstring::npos)
    {
        UClass* Class = FindActorClass(Query);
        if (!Class || !Class->IsChildOf(AActor::StaticClass()))
        {
            Reply(FString::Printf(TEXT("No actor class at '%s'"), Query.c_str()));
            return;
        }
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Pawn, Class))
        {
            if (IsCandidate(Actor))
            {
                Matches.push_back(Actor);
            }
        }
    }
    else
    {
        const std::wstring Term = Simplified(Query);
        if (Term.empty())
        {
            Reply(Usage);
            return;
        }
        // Each class is checked once: its name or a parent's, up to AActor.
        std::map<UClass*, bool> ClassMatches;
        const auto ClassMatch = [&](UClass* Class)
        {
            const auto Found = ClassMatches.find(Class);
            if (Found != ClassMatches.end())
            {
                return Found->second;
            }
            bool bMatch = false;
            for (UClass* Super = Class; Super && !bMatch; Super = Super == AActor::StaticClass() ? nullptr : Super->GetSuperClass())
            {
                bMatch = Simplified(Super->GetName().ToStdString()).find(Term) != std::wstring::npos;
            }
            ClassMatches.emplace(Class, bMatch);
            return bMatch;
        };
        for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Pawn, AActor::StaticClass()))
        {
            if (IsCandidate(Actor) && (ClassMatch(Actor->GetClass()) || Simplified(Actor->GetName().ToStdString()).find(Term) != std::wstring::npos))
            {
                Matches.push_back(Actor);
            }
        }
    }
    if (Matches.empty())
    {
        Reply(FString::Printf(TEXT("No actor in the world matches '%s' (give part of its class or its name)"), Query.c_str()));
        return;
    }
    // Something you can see comes before what has no bounds at all, a mission's or a manager's logic actor at the
    // world's origin (#147: "tpactor StormChest_C" went to Mission_StormChest_C_0, not a chest). Only those, if that's
    // all there is.
    const auto HasBounds = [](AActor* Actor)
    {
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(false, Origin, Extent);
        return Extent.X > 0.0f || Extent.Y > 0.0f || Extent.Z > 0.0f;
    };
    if (std::any_of(Matches.begin(), Matches.end(), HasBounds))
    {
        std::erase_if(Matches, [&HasBounds](AActor* Actor) { return !HasBounds(Actor); });
    }
    if (Wanted > static_cast<int32_t>(Matches.size()))
    {
        Reply(FString::Printf(TEXT("Only %d actors match '%s'"), static_cast<int32_t>(Matches.size()), Query.c_str()));
        return;
    }

    const FVector PawnLocation = Pawn->GetTransform().GetLocation();
    const auto DistanceSquared = [&](const FVector& Location)
    {
        return FMath::Square(Location.X - PawnLocation.X) + FMath::Square(Location.Y - PawnLocation.Y) + FMath::Square(Location.Z - PawnLocation.Z);
    };
    std::vector<std::pair<float, AActor*>> ByDistance;
    ByDistance.reserve(Matches.size());
    for (AActor* Actor : Matches)
    {
        ByDistance.emplace_back(DistanceSquared(Actor->GetTransform().GetLocation()), Actor);
    }
    std::nth_element(ByDistance.begin(), ByDistance.begin() + (Wanted - 1), ByDistance.end(),
        [](const auto& A, const auto& B) { return A.first < B.first; });
    AActor* Target = ByDistance[Wanted - 1].second;

    // On top of it: its bounds (what collides, else all of it) give its top, and a trace down from just over that lands
    // the pawn on what is there, the actor itself or the floor under one that doesn't collide. An actor under the map
    // finds nothing below it, so the trace goes again from high above, down to the ground over it. The pawn's feet go on
    // the hit, and the engine's TeleportTo moves it out of anything it would stand in.
    constexpr float TraceClearance = 100.0f;
    constexpr float TraceHeight = 50000.0f;
    constexpr float TraceDepth = 100000.0f;
    FVector Origin;
    FVector Extent;
    Target->GetActorBounds(true, Origin, Extent);
    if (Extent.X <= 0.0f && Extent.Y <= 0.0f && Extent.Z <= 0.0f)
    {
        Target->GetActorBounds(false, Origin, Extent);
    }
    if ((Extent.X <= 0.0f && Extent.Y <= 0.0f && Extent.Z <= 0.0f) || Extent.Z > TraceHeight)
    {
        Origin = Target->GetTransform().GetLocation();
        Extent = FVector(0.0f, 0.0f, 0.0f);
    }
    FVector PawnOrigin;
    FVector PawnExtent;
    Pawn->GetActorBounds(true, PawnOrigin, PawnExtent);
    // How far the pawn's location is over its feet (its capsule's half height; a standing player's 90 if it has none).
    const float FeetOffset = PawnExtent.Z > 0.0f ? PawnLocation.Z - (PawnOrigin.Z - PawnExtent.Z) : 90.0f;
    const float Top = Origin.Z + Extent.Z;
    const float Bottom = Origin.Z - Extent.Z;
    FVector Floor;
    bool bFloor = UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Origin.X, Origin.Y, Top + TraceClearance), FVector(Origin.X, Origin.Y, Bottom - TraceDepth), 0, false, true, Floor);
    bool bAboveMap = false;
    if (!bFloor)
    {
        bFloor = UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Origin.X, Origin.Y, Top + TraceHeight), FVector(Origin.X, Origin.Y, Bottom - TraceDepth), 0, false, true, Floor);
        bAboveMap = bFloor && Floor.Z > Top;
    }
    const FVector Destination = bFloor ? FVector(Floor.X, Floor.Y, Floor.Z + FeetOffset + 2.0f) : FVector(Origin.X, Origin.Y, Top + FeetOffset + TraceClearance);
    const FRotator Rotation = Pawn->GetActorRotation();
    bool bTeleported = false;
    for (int32_t Attempt = 0; Attempt < 4 && !bTeleported; ++Attempt)
    {
        bTeleported = Pawn->TeleportTo(FVector(Destination.X, Destination.Y, Destination.Z + 100.0f * static_cast<float>(Attempt)), Rotation);
    }
    const FString What = FString::Printf(TEXT("%s (%s), %d of %d matching"), *Target->GetName(), *Target->GetClass()->GetName(), Wanted, static_cast<int32_t>(Matches.size()));
    if (!bTeleported)
    {
        Reply(FString::Printf(TEXT("Can't teleport on top of %s: there's no room there"), *What));
        return;
    }
    const float Distance = std::sqrt(ByDistance[Wanted - 1].first) / 100.0f;
    UE_LOG(LogFortCheats, Log, TEXT("TpActor: %s to %s at X=%.1f Y=%.1f Z=%.1f (floor %d, above the map %d)"), *GetNameSafe(PlayerController), *What,
        Destination.X, Destination.Y, Destination.Z, bFloor ? 1 : 0, bAboveMap ? 1 : 0);
    Reply(FString::Printf(TEXT("Teleported to %s, %.0fm away%s"), *What, Distance,
        bAboveMap ? TEXT("; it's under the map, so onto the ground over it") : bFloor ? TEXT("") : TEXT("; no floor found under it, so just over it")));
#endif
}

#if WITH_SERVER_CODE
namespace
{
    // ---- "cheat teleportto" and "cheat bring" (#155) -------------------------------------------------------------------

    // Puts Pawn on the floor Distance away from Anchor (a pawn) at the yaw Direction, facing Facing. A wall between them
    // brings the spot back to this side of it, a trace down from the anchor's height finds the floor there, and the
    // engine's TeleportTo moves the pawn out of anything it would stand in; failing all that, next to the anchor itself.
    bool TeleportNextTo(APawn* Pawn, const AActor* Anchor, float Distance, float Direction, float Facing)
    {
        constexpr float WallClearance = 60.0f;
        constexpr float TraceUp = 50.0f;
        constexpr float TraceDown = 3000.0f;
        const FVector AnchorLocation = Anchor->GetTransform().GetLocation();
        const float Radians = Direction * 3.14159265f / 180.0f;
        const FVector Wanted(AnchorLocation.X + std::cos(Radians) * Distance, AnchorLocation.Y + std::sin(Radians) * Distance, AnchorLocation.Z);
        FVector Spot = Wanted;
        FVector Wall;
        if (UKismetSystemLibrary::LineTraceSingle(Pawn, AnchorLocation, Wanted, 0, false, true, Wall))
        {
            const float Reach = std::max(0.0f, std::sqrt(FMath::Square(Wall.X - AnchorLocation.X) + FMath::Square(Wall.Y - AnchorLocation.Y)) - WallClearance);
            Spot = FVector(AnchorLocation.X + std::cos(Radians) * Reach, AnchorLocation.Y + std::sin(Radians) * Reach, AnchorLocation.Z);
        }

        FVector PawnOrigin;
        FVector PawnExtent;
        Pawn->GetActorBounds(true, PawnOrigin, PawnExtent);
        const float FeetOffset = PawnExtent.Z > 0.0f ? Pawn->GetTransform().GetLocation().Z - (PawnOrigin.Z - PawnExtent.Z) : 90.0f;
        FVector Floor;
        const bool bFloor = UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Spot.X, Spot.Y, Spot.Z + TraceUp), FVector(Spot.X, Spot.Y, Spot.Z - TraceDown), 0, false, true, Floor);
        const FVector Destination = bFloor ? FVector(Floor.X, Floor.Y, Floor.Z + FeetOffset + 2.0f) : Spot;
        const FRotator Rotation(0.0f, Facing, 0.0f);
        for (int32_t Attempt = 0; Attempt < 3; ++Attempt)
        {
            if (Pawn->TeleportTo(FVector(Destination.X, Destination.Y, Destination.Z + 100.0f * static_cast<float>(Attempt)), Rotation))
            {
                return true;
            }
        }
        return Pawn->TeleportTo(FVector(AnchorLocation.X, AnchorLocation.Y, AnchorLocation.Z + 2.0f * FeetOffset), Rotation);
    }

    // The yaw from one actor to another.
    float YawTowards(const AActor* From, const AActor* To)
    {
        const FVector A = From->GetTransform().GetLocation();
        const FVector B = To->GetTransform().GetLocation();
        return std::atan2(B.Y - A.Y, B.X - A.X) * 180.0f / 3.14159265f;
    }

    // The pawn a player plays now, when it is alive.
    APawn* LivePawnOf(AFortPlayerController* PlayerController)
    {
        APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
        AFortPawn* FortPawn = Pawn ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
        return Pawn && !(FortPawn && FortPawn->IsDead()) ? Pawn : nullptr;
    }

    // The words of a command's arguments as one name ("Big Bob").
    std::wstring JoinedName(const FString& Arguments)
    {
        std::wstring Name;
        for (const std::wstring& Word : SplitWords(Arguments.ToStdString()))
        {
            Name += (Name.empty() ? L"" : L" ") + Word;
        }
        // "/Bob/" names a player the way a cheat's target does.
        if (Name.size() > 2 && Name.front() == L'/' && Name.back() == L'/')
        {
            Name = Name.substr(1, Name.size() - 2);
        }
        return Name;
    }
}
#endif

void UFortCheatManager::TeleportTo(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::wstring Name = JoinedName(Arguments);
    if (Name.empty())
    {
        Reply(TEXT("Usage: teleportto <player>, their name or its start (\"teleportto Bob\"): you go next to them"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = LivePawnOf(PlayerController);
    if (!Pawn)
    {
        Reply(TEXT("You have no pawn to teleport"));
        return;
    }
    FString Error;
    AFortPlayerController* Target = FindCheatTarget(PlayerController, FString(Name.c_str()), Error);
    if (!Target)
    {
        Reply(Error);
        return;
    }
    const FString TargetName = PlayerNameOf(Target);
    APawn* TargetPawn = Target->GetPawn();
    if (Target == PlayerController || TargetPawn == Pawn)
    {
        Reply(TEXT("That's you"));
        return;
    }
    if (!TargetPawn)
    {
        Reply(FString::Printf(TEXT("%s has no pawn to teleport to (dead, or in the aircraft)"), *TargetName));
        return;
    }

    // In front of them, facing them.
    const float TargetYaw = TargetPawn->GetActorRotation().Yaw;
    if (!TeleportNextTo(Pawn, TargetPawn, 150.0f, TargetYaw, TargetYaw + 180.0f))
    {
        Reply(FString::Printf(TEXT("Can't teleport to %s: there's no room there"), *TargetName));
        return;
    }
    UE_LOG(LogFortCheats, Log, TEXT("TeleportTo: %s to %s (%s)"), *GetNameSafe(PlayerController), *TargetName, *GetNameSafe(TargetPawn));
    Reply(FString::Printf(TEXT("Teleported to %s"), *TargetName));
#endif
}

void UFortCheatManager::Bring(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::wstring Name = JoinedName(Arguments);
    if (Name.empty())
    {
        Reply(TEXT("Usage: bring <player | all>, their name or its start (\"bring Bob\"): they come to stand in front of you"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    APawn* Pawn = LivePawnOf(PlayerController);
    if (!Pawn)
    {
        Reply(TEXT("You have no pawn to bring anyone to"));
        return;
    }

    std::vector<AFortPlayerController*> Targets;
    if (Lower(Name) == L"all")
    {
        UWorld* World = PlayerController->GetWorld();
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        const TArray<APlayerState*>* PlayerArray = GameState ? &static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray) : nullptr;
        for (int32_t Index = 0; PlayerArray && Index < PlayerArray->Num(); ++Index)
        {
            APlayerState* PlayerState = (*PlayerArray)[Index];
            AFortPlayerController* Controller = PlayerState ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(PlayerState->GetOwner())) : nullptr;
            if (Controller && Controller != PlayerController)
            {
                Targets.push_back(Controller);
            }
        }
        if (Targets.empty())
        {
            Reply(TEXT("There's nobody else to bring"));
            return;
        }
    }
    else
    {
        FString Error;
        AFortPlayerController* Target = FindCheatTarget(PlayerController, FString(Name.c_str()), Error);
        if (!Target)
        {
            Reply(Error);
            return;
        }
        if (Target == PlayerController)
        {
            Reply(TEXT("That's you"));
            return;
        }
        Targets.push_back(Target);
    }

    // In front of the player, facing them: one 200 units out, several spread over an arc, more in rows behind.
    constexpr int32_t PerRow = 7;
    constexpr float ArcStep = 25.0f;
    const float Yaw = Pawn->GetActorRotation().Yaw;
    std::wstring Brought;
    std::wstring Skipped;
    int32_t Placed = 0;
    for (AFortPlayerController* Target : Targets)
    {
        const FString TargetName = PlayerNameOf(Target);
        APawn* TargetPawn = LivePawnOf(Target);
        if (!TargetPawn || TargetPawn == Pawn)
        {
            Skipped += (Skipped.empty() ? L"" : L", ") + TargetName.ToStdString();
            continue;
        }
        const int32_t Row = Placed / PerRow;
        const int32_t InRow = std::min<int32_t>(PerRow, static_cast<int32_t>(Targets.size()) - Row * PerRow);
        const float Offset = (static_cast<float>(Placed % PerRow) - static_cast<float>(InRow - 1) * 0.5f) * ArcStep;
        if (TeleportNextTo(TargetPawn, Pawn, 200.0f + 120.0f * static_cast<float>(Row), Yaw + Offset, Yaw + Offset + 180.0f))
        {
            ++Placed;
            Brought += (Brought.empty() ? L"" : L", ") + TargetName.ToStdString();
        }
        else
        {
            Skipped += (Skipped.empty() ? L"" : L", ") + TargetName.ToStdString();
        }
    }
    UE_LOG(LogFortCheats, Log, TEXT("Bring: %s brought %s (not: %s)"), *GetNameSafe(PlayerController), Brought.c_str(), Skipped.c_str());
    if (Brought.empty())
    {
        Reply(FString::Printf(TEXT("Brought nobody: %s %s no live pawn or no room by you"), Skipped.c_str(), Targets.size() == 1 ? TEXT("has") : TEXT("have")));
        return;
    }
    Reply(Skipped.empty() ? FString::Printf(TEXT("Brought %s"), Brought.c_str())
                          : FString::Printf(TEXT("Brought %s (not %s: no live pawn, the aircraft, or no room)"), Brought.c_str(), Skipped.c_str()));
#endif
}

#if WITH_SERVER_CODE
namespace
{
    // "cheat nocooldown" (#148): the players whose ability and gadget cooldowns end as soon as they start.
    std::vector<TWeakObjectPtr<AFortPlayerController>> NoCooldownPlayers;
    bool bClearingCooldowns = false;

    bool HasNoCooldown(const AFortPlayerController* PlayerController)
    {
        return std::any_of(NoCooldownPlayers.begin(), NoCooldownPlayers.end(),
            [PlayerController](const TWeakObjectPtr<AFortPlayerController>& Known) { return Known.Get() == PlayerController; });
    }

    // The ability systems a player's abilities are granted on: the player state's (the hero's, the gadgets' and the
    // player's own) and the pawn's when it has another.
    std::vector<UAbilitySystemComponent*> PlayerAbilitySystems(AFortPlayerController* PlayerController)
    {
        std::vector<UAbilitySystemComponent*> AbilitySystems;
        const auto AddAbilitySystem = [&AbilitySystems](UAbilitySystemComponent* AbilitySystem)
        {
            if (AbilitySystem && std::find(AbilitySystems.begin(), AbilitySystems.end(), AbilitySystem) == AbilitySystems.end())
            {
                AbilitySystems.push_back(AbilitySystem);
            }
        };
        if (AFortPlayerState* PlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))))
        {
            AddAbilitySystem(PlayerState->AbilitySystemComponent);
        }
        if (AFortPawn* Pawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())))
        {
            AddAbilitySystem(Pawn->AbilitySystemComponent);
        }
        return AbilitySystems;
    }

    // Ends the player's cooldowns. A cooldown is the effect an ability's CommitAbility applies to its owner (its
    // CooldownGameplayEffectClass, whose granted tags CheckCooldown refuses an activation on), for a hero ability and a
    // gadget alike (GA_AirStrike: GE_AirStrike_Cooldown). Every one of those on the player's ability systems comes off.
    // The removal replicates, so the client's cooldown tags and HUD timer clear and its next activation isn't refused
    // before it reaches the server. How many ability systems it went through.
    int32_t ClearCooldowns(AFortPlayerController* PlayerController)
    {
        const std::vector<UAbilitySystemComponent*> AbilitySystems = PlayerAbilitySystems(PlayerController);
        for (UAbilitySystemComponent* AbilitySystem : AbilitySystems)
        {
            // The classes first: removing an effect can change what the ability system grants.
            std::vector<UClass*> Cooldowns;
            TStructArray<FGameplayAbilitySpec>& Specs = AbilitySystem->ActivatableAbilities.Items;
            for (int32_t Index = 0; Index < Specs.Num(); ++Index)
            {
                UGameplayAbility* Ability = Specs[Index].Ability;
                UClass* Cooldown = Ability ? static_cast<UClass*>(Ability->CooldownGameplayEffectClass) : nullptr;
                if (Cooldown && std::find(Cooldowns.begin(), Cooldowns.end(), Cooldown) == Cooldowns.end())
                {
                    Cooldowns.push_back(Cooldown);
                }
            }
            for (UClass* Cooldown : Cooldowns)
            {
                AbilitySystem->RemoveActiveGameplayEffectBySourceEffect(Cooldown);
            }
        }
        return static_cast<int32_t>(AbilitySystems.size());
    }

    // The items the player's abilities spend as their cost when they commit (#155): an Item cost of a UFortGameplayAbility
    // that is an ammo item (GA_Generic_BotTurret: one AmmoDataBotTurret). A gadget's or a hero ability's charges are such
    // an item's count, with or without a RegenCooldown. A consumable spending itself isn't a charge, so it stays out.
    std::vector<UFortItemDefinition*> AbilityChargeItems(AFortPlayerController* PlayerController)
    {
        std::vector<UFortItemDefinition*> Items;
        if (!UFortGameplayAbility::HasAbilityCosts())
        {
            return Items;
        }
        const uint8_t ItemCost = EFortAbilityCostSource::GetItem();
        for (UAbilitySystemComponent* AbilitySystem : PlayerAbilitySystems(PlayerController))
        {
            TStructArray<FGameplayAbilitySpec>& Specs = AbilitySystem->ActivatableAbilities.Items;
            for (int32_t Index = 0; Index < Specs.Num(); ++Index)
            {
                UFortGameplayAbility* Ability = Cast<UFortGameplayAbility>(static_cast<UObject*>(static_cast<UGameplayAbility*>(Specs[Index].Ability)));
                if (!Ability)
                {
                    continue;
                }
                TStructArray<FFortAbilityCost>& Costs = Ability->AbilityCosts;
                for (int32_t CostIndex = 0; CostIndex < Costs.Num(); ++CostIndex)
                {
                    FFortAbilityCost& Cost = Costs[CostIndex];
                    UFortItemDefinition* Item = Cost.ItemDefinition;
                    if (Item && static_cast<uint8_t>(Cost.CostSource) == ItemCost && Cast<UFortAmmoItemDefinition>(Item)
                        && std::find(Items.begin(), Items.end(), Item) == Items.end())
                    {
                        Items.push_back(Item);
                    }
                }
            }
        }
        return Items;
    }

    // The ability charge items each nocooldown player has had (#155), so one whose last charge was spent and that left the
    // inventory with it comes back.
    std::vector<std::pair<TWeakObjectPtr<AFortPlayerController>, std::vector<TWeakObjectPtr<UFortWorldItemDefinition>>>> KnownChargeItems;

    // Recharges the player's gadgets and ability charges (#148, #155): every stack of uses an ability spends is kept full,
    // so it never runs out. Those are the items the player's abilities spend as an Item cost (AbilityChargeItems), and
    // every ammo item with a RegenCooldown (AmmoDataBotTurret: 240 s), whose world item keeps a timer while its count is
    // under the regen stack. Each one under its stack size is set full through the inventory, as that timer does: the
    // world item's own SetItemCount clears the timer once the count is full, and the count replicates, so the client's
    // slot shows the gadget ready. One the player had that is gone from the inventory is given back. How many items it
    // refilled.
    int32_t RechargeGadgets(AFortPlayerController* PlayerController)
    {
        AFortInventory* Inventory = PlayerController->WorldInventory;
        if (!Inventory)
        {
            return 0;
        }

        auto Known = std::find_if(KnownChargeItems.begin(), KnownChargeItems.end(), [PlayerController](const auto& Entry) { return Entry.first.Get() == PlayerController; });
        if (Known == KnownChargeItems.end())
        {
            KnownChargeItems.emplace_back(PlayerController, std::vector<TWeakObjectPtr<UFortWorldItemDefinition>>());
            Known = KnownChargeItems.end() - 1;
        }
        std::vector<TWeakObjectPtr<UFortWorldItemDefinition>>& Had = Known->second;

        const std::vector<UFortItemDefinition*> ChargeItems = AbilityChargeItems(PlayerController);
        std::vector<UFortWorldItemDefinition*> Present;
        int32_t Recharged = 0;
        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            UFortWorldItem* WorldItem = Instances[Index];
            FFortItemEntry* Entry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
            UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
            if (!Definition)
            {
                continue;
            }
            UFortAmmoItemDefinition* AmmoDefinition = Cast<UFortAmmoItemDefinition>(Definition);
            const bool bRegenerates = AmmoDefinition && AmmoDefinition->RegenCooldown > 0.0f;
            const bool bAbilityCharge = std::find(ChargeItems.begin(), ChargeItems.end(), Definition) != ChargeItems.end();
            if (!bRegenerates && !bAbilityCharge)
            {
                continue;
            }
            if (UFortWorldItemDefinition* WorldDefinition = Cast<UFortWorldItemDefinition>(Definition))
            {
                Present.push_back(WorldDefinition);
                if (std::none_of(Had.begin(), Had.end(), [WorldDefinition](const TWeakObjectPtr<UFortWorldItemDefinition>& Item) { return Item.Get() == WorldDefinition; }))
                {
                    Had.emplace_back(WorldDefinition);
                }
            }
            const int32_t Full = std::max(Inventory->GetMaxStackSize(Definition), 1);
            if (Entry->Count < Full)
            {
                Inventory->UpdateWorldItemCount(WorldItem, Full);
                ++Recharged;
            }
        }

        // A charge item its last use took out of the inventory comes back as a full stack, while an ability still spends it.
        std::erase_if(Had, [](const TWeakObjectPtr<UFortWorldItemDefinition>& Item) { return !Item.Get(); });
        for (const TWeakObjectPtr<UFortWorldItemDefinition>& Item : Had)
        {
            UFortWorldItemDefinition* Definition = Item.Get();
            const bool bSpent = std::find(ChargeItems.begin(), ChargeItems.end(), static_cast<UFortItemDefinition*>(Definition)) != ChargeItems.end();
            if (bSpent && std::find(Present.begin(), Present.end(), Definition) == Present.end())
            {
                UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, Definition, std::max(Inventory->GetMaxStackSize(Definition), 1), false);
                ++Recharged;
            }
        }
        return Recharged;
    }

    // Every frame on the game thread while a player has no cooldowns: a cooldown applied since the last frame comes off,
    // and a gadget used since then is recharged. Nothing is hooked: the abilities commit their cooldowns and spend their
    // gadget ammo as ever, and the engine's own removal and the inventory's own count update undo them.
    void TickNoCooldown()
    {
        std::erase_if(NoCooldownPlayers, [](const TWeakObjectPtr<AFortPlayerController>& Known) { return !Known.Get(); });
        std::erase_if(KnownChargeItems, [](const auto& Entry) { return !HasNoCooldown(Entry.first.Get()); });
        for (const TWeakObjectPtr<AFortPlayerController>& Known : NoCooldownPlayers)
        {
            ClearCooldowns(Known.Get());
            RechargeGadgets(Known.Get());
        }
        bClearingCooldowns = !NoCooldownPlayers.empty() && GameThread::Enqueue(&TickNoCooldown);
    }
}
#endif

void UFortCheatManager::NoCooldown(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.empty() ? std::wstring() : Lower(Words[0]);
    if (Words.size() > 1 || (!Word.empty() && Word != L"on" && Word != L"off"))
    {
        Reply(TEXT("Usage: nocooldown [on | off], no cooldown on your abilities and gadgets; without on or off it toggles"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController)
    {
        return;
    }
    if (!UAbilitySystemComponent::FindStaticFunction_RemoveActiveGameplayEffectBySourceEffect())
    {
        Reply(TEXT("NoCooldown needs AbilitySystemComponent.RemoveActiveGameplayEffectBySourceEffect, which this build doesn't have"));
        return;
    }

    const bool bEnable = Word.empty() ? !HasNoCooldown(PlayerController) : Word == L"on";
    std::erase_if(NoCooldownPlayers, [PlayerController](const TWeakObjectPtr<AFortPlayerController>& Known) { return !Known.Get() || Known.Get() == PlayerController; });
    if (bEnable)
    {
        NoCooldownPlayers.emplace_back(PlayerController);
    }
    // The game's own flag (replicated), so it says the same as Core.
    if (PlayerController->HasNoCoolDownFlag())
    {
        PlayerController->bNoCoolDown = bEnable;
    }
    if (!bEnable)
    {
        std::erase_if(KnownChargeItems, [PlayerController](const auto& Entry) { return Entry.first.Get() == PlayerController; });
        Reply(TEXT("No cooldown OFF: the next ability or gadget you use cools down and spends its charge again"));
        return;
    }
    const int32_t AbilitySystems = ClearCooldowns(PlayerController);
    const int32_t Recharged = RechargeGadgets(PlayerController);
    if (!bClearingCooldowns)
    {
        bClearingCooldowns = GameThread::Enqueue(&TickNoCooldown);
    }
    UE_LOG(LogFortCheats, Log, TEXT("NoCooldown: %s on, %d ability systems, %d gadgets recharged"), *GetNameSafe(PlayerController), AbilitySystems, Recharged);
    Reply(AbilitySystems > 0 ? TEXT("No cooldown ON: your abilities and gadgets are ready again as soon as you use them, their charges always full (nocooldown off: normal again)")
                             : TEXT("No cooldown ON (nocooldown off: normal again); you have no abilities yet, and they'll have none once you do"));
#endif
}

#if WITH_SERVER_CODE
namespace
{
    // "cheat infinitemag" (#149): the players whose magazine never empties.
    std::vector<TWeakObjectPtr<AFortPlayerController>> InfiniteMagPlayers;

    bool bTickingWeaponCheats = false;

    bool HasInfiniteMag(const AFortPlayerController* PlayerController)
    {
        return std::any_of(InfiniteMagPlayers.begin(), InfiniteMagPlayers.end(),
            [PlayerController](const TWeakObjectPtr<AFortPlayerController>& Known) { return Known.Get() == PlayerController; });
    }

    // Tops the equipped weapon's magazine up through its own ModAmmo: on the server that sets the replicated AmmoCount
    // and the item's LoadedAmmo, as a reload does. Melee weapons and tools (no magazine) are left alone.
    void FillMagazine(AFortPlayerController* PlayerController)
    {
        AFortPawn* Pawn = PlayerController->MyFortPawn;
        AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
        const int32_t BulletsPerClip = Weapon ? Weapon->GetBulletsPerClip() : 0;
        const int32_t AmmoCount = Weapon ? static_cast<int32_t>(Weapon->AmmoCount) : 0;
        if (BulletsPerClip > 0 && AmmoCount < BulletsPerClip)
        {
            Weapon->ModAmmo(BulletsPerClip - AmmoCount);
        }
    }

    // ---- Weapon attribute boosts: fire rate and damage -------------------------------------------------------------

    // "cheat firerate" (#150) and "cheat weapondamage" (#152) apply the game's own weapon perk effects (infinite
    // GE_Alteration_Attribute_* effects whose multiplicative modifier scales an attribute by a CombatEffects row at the
    // effect's level) at the level that gives the magnitude wanted. A stage is one attribute for one kind of weapon (the
    // modifier's required source tag) and the effect Core uses for it.
    struct FBoostStage
    {
        const wchar_t* EffectPath;
        const wchar_t* AttributeSetPath;
        const wchar_t* Attribute;
        // The tag the modifier requires on the source (the weapon's), and its parent, both passed to the evaluation.
        const wchar_t* SourceTag;
        const wchar_t* SourceParentTag;
        // "guns", "melee weapons".
        const wchar_t* What;
        // The effect's modifier adds to the attribute instead of multiplying it: the boost's Multiplier is then an amount
        // added, measured from 0 (infinitemag's chance to not consume ammo).
        bool bAdditive = false;
    };

    struct FBoostKind
    {
        // "fire rate", "weapon damage": for replies and logs.
        const wchar_t* Name;
        const FBoostStage* Stages;
        size_t NumStages;
    };

    // AFortWeaponRanged's fire rate is its stats' FiringRate run through UFortWeaponAttrSet's WeaponRateOfFire aggregator
    // on the pawn's ability system (1.9 0x9412D0), on the server and the owning client alike.
    constexpr FBoostStage FireRateStages[] = {
        { L"/Game/Abilities/Player/Alterations/GE_Alteration_Attribute_FireRate.GE_Alteration_Attribute_FireRate_C",
            L"/Script/FortniteGame.FortWeaponAttrSet", L"WeaponRateOfFire", L"weapon.ranged", L"weapon", L"guns" },
    };
    constexpr FBoostKind FireRateBoost{ L"fire rate", FireRateStages, std::size(FireRateStages) };

    // Weapon damage goes through UFortDamageSet's OutgoingAbilityDamage, captured from the shooter by the damage
    // execution on the server (1.9 0x686AB0). GE_Alteration_Attribute_WeaponDamage multiplies it for "weapon.ranged" by
    // GE.Guns.All.Damage and for "weapon.melee" by GE.Melee.All.Damage, so its level is chosen for the guns, and
    // GE_Alteration_Attribute_WeaponDamage_Melee (melee only) then brings the melee weapons to the same multiplier when
    // the two rows differ.
    constexpr FBoostStage WeaponDamageStages[] = {
        { L"/Game/Abilities/Player/Alterations/GE_Alteration_Attribute_WeaponDamage.GE_Alteration_Attribute_WeaponDamage_C",
            L"/Script/FortniteGame.FortDamageSet", L"OutgoingAbilityDamage", L"weapon.ranged", L"weapon", L"guns" },
        { L"/Game/Abilities/Player/Alterations/GE_Alteration_Attribute_WeaponDamage_Melee.GE_Alteration_Attribute_WeaponDamage_Melee_C",
            L"/Script/FortniteGame.FortDamageSet", L"OutgoingAbilityDamage", L"weapon.melee", L"weapon", L"melee weapons" },
    };
    constexpr FBoostKind WeaponDamageBoost{ L"weapon damage", WeaponDamageStages, std::size(WeaponDamageStages) };

    // "cheat speed" in Battle Royale (#155): the game's own move speed alteration multiplies FortMovementSet's RunSpeed
    // (by GE.Character.Move.Run), for no weapon in particular. Battle Royale's pawn moves at its stats row's speeds up to
    // 1.8.x, not its movement set's, so there the effect alone moves nothing; it is how the multiplier reaches the owning
    // client, whose ability system the effect replicates to, and the server and that client both scale the row by it.
    // 1.9's pawn moves at its movement set's speeds, of which the effect scales only the run: the server scales the
    // others (walk, sprint, crouched) with the pawn's multiplier (AFortPawn::SetCheatSpeedMultiplier, #155).
    constexpr FBoostStage MoveSpeedStages[] = {
        { L"/Game/Abilities/Player/Alterations/GE_Alteration_Attribute_MoveSpeed.GE_Alteration_Attribute_MoveSpeed_C",
            L"/Script/FortniteGame.FortMovementSet", L"RunSpeed", nullptr, nullptr, L"running speed" },
    };
    constexpr FBoostKind MoveSpeedBoost{ L"movement speed", MoveSpeedStages, std::size(MoveSpeedStages) };

    // "cheat infinitemag" (#155): a shot takes its rounds through the weapon's ammo use (1.9 0x939490), on the server and
    // on the owning client, which counts its own magazine. Before it takes any, it evaluates UFortWeaponAttrSet's
    // WeaponChanceToNotConsumeAmmo on the shooter's ability system for the weapon's tags, clamped to 0-1, plus the
    // AccumulatedWeaponChanceToNotConsumeAmmo left from earlier shots: at 1 or more the shot takes nothing. The game's
    // own ammo-saving perk, GE_Alteration_Attribute_AmmoSave, adds to that chance for "weapon.ranged" (CombatEffects'
    // Guns.All.AmmoSave at the effect's level). With effects worth more than 1 on the pawn's ability system, which
    // replicates them to its owner, neither the server nor the client takes a round: a one-round weapon stays loaded,
    // no reload starts, and the magazine shows its normal size.
    constexpr FBoostStage AmmoSaveStages[] = {
        { L"/Game/Abilities/Player/Alterations/GE_Alteration_Attribute_AmmoSave.GE_Alteration_Attribute_AmmoSave_C",
            L"/Script/FortniteGame.FortWeaponAttrSet", L"WeaponChanceToNotConsumeAmmo", L"weapon.ranged", L"weapon", L"guns", true },
    };
    constexpr FBoostKind AmmoSaveBoost{ L"ammo saving", AmmoSaveStages, std::size(AmmoSaveStages) };
    // Over 1, so the effects' sum can't fall a hair short of it: the shot clamps the chance to 1 anyway.
    constexpr float InfiniteMagAmmoSaveChance = 1.5f;

    // The effects Core put on a player's ability system for one kind, kept through respawns.
    struct FAttributeBoost
    {
        const FBoostKind* Kind = nullptr;
        TWeakObjectPtr<AFortPlayerController> Player;
        TWeakObjectPtr<UAbilitySystemComponent> AbilitySystem;
        TWeakObjectPtr<AFortPawn> Pawn;
        float Multiplier = 1.0f;
        // FActiveGameplayEffectHandle (an int32 handle and a bool, 8 bytes) of each effect Core applied.
        std::vector<uint64_t> Handles;
    };
    std::vector<FAttributeBoost> AttributeBoosts;

    constexpr int32_t BoostCurveLevels = 100;
    // The move speed effect gives at most about 2x a stack, so "speed 1000" (#155) takes about a thousand.
    constexpr int32_t MaxBoostEffects = 1000;

    // Not kept: nothing roots the class once no effect of it is applied, so it may be collected and loaded again.
    UClass* BoostEffectClass(const FBoostStage& Stage)
    {
        UClass* Class = FindObject<UClass>(nullptr, Stage.EffectPath);
        return Class ? Class : LoadObject<UClass>(nullptr, Stage.EffectPath, nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet));
    }

    UClass* BoostAttributeSet(const FBoostStage& Stage)
    {
        return FindObject<UClass>(nullptr, Stage.AttributeSetPath);
    }

    UProperty* BoostAttribute(const FBoostStage& Stage)
    {
        UClass* AttributeSet = BoostAttributeSet(Stage);
        return AttributeSet ? AttributeSet->FindPropertyByName(FName(Stage.Attribute, FNAME_Find)) : nullptr;
    }

    // The modifier's source tag requirements let the stage's weapons through: none, or the stage's tag among them.
    bool ModifierAppliesTo(FGameplayModifierInfo& Modifier, const FBoostStage& Stage)
    {
        const FGameplayTagContainer& Required = static_cast<FGameplayTagRequirements&>(Modifier.SourceTags).RequireTags;
        if (Required.GameplayTags.Num() == 0)
        {
            return true;
        }
        for (const FGameplayTag& Tag : Required.GameplayTags)
        {
            if (Stage.SourceTag && _wcsicmp(Tag.TagName.ToString().ToStdString().c_str(), Stage.SourceTag) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // The effect's multiplicative (an additive stage's: additive) modifier on the stage's attribute for the stage's
    // weapons (a scalable float), or null.
    const FScalableFloat* BoostMagnitude(UClass* EffectClass, const FBoostStage& Stage)
    {
        UGameplayEffect* Effect = EffectClass ? Cast<UGameplayEffect>(EffectClass->GetDefaultObject()) : nullptr;
        UProperty* Attribute = BoostAttribute(Stage);
        if (!Effect || !Attribute)
        {
            return nullptr;
        }
        TStructArray<FGameplayModifierInfo>& Modifiers = Effect->Modifiers;
        for (int32_t Index = 0; Index < Modifiers.Num(); ++Index)
        {
            FGameplayModifierInfo& Modifier = Modifiers[Index];
            const FGameplayAttribute& ModifiedAttribute = Modifier.Attribute;
            const FGameplayEffectModifierMagnitude& Magnitude = Modifier.ModifierMagnitude;
            if (ModifiedAttribute.Attribute == Attribute && Modifier.ModifierOp == (Stage.bAdditive ? EGameplayModOp::GetAdditive() : EGameplayModOp::GetMultiplicitive())
                && Magnitude.MagnitudeCalculationType == EGameplayEffectMagnitudeCalculation::GetScalableFloat() && ModifierAppliesTo(Modifier, Stage))
            {
                return &static_cast<const FScalableFloat&>(Magnitude.ScalableFloatMagnitude);
            }
        }
        return nullptr;
    }

    UFunction* FunctionAt(const wchar_t* Path)
    {
        return FindObject<UFunction>(nullptr, Path);
    }

    UObject* DefaultObjectOf(const wchar_t* ClassPath)
    {
        UClass* Class = FindObject<UClass>(nullptr, ClassPath);
        return Class ? Class->GetDefaultObject() : nullptr;
    }

    // FScalableFloat::GetValueAtLevel: Value, scaled by its curve table row at Level.
    std::optional<float> ValueAtLevel(const FScalableFloat& ScalableFloat, float Level)
    {
        const FCurveTableRowHandle& Curve = ScalableFloat.Curve;
        UObject* CurveTable = Curve.CurveTable;
        if (!CurveTable)
        {
            return static_cast<float>(ScalableFloat.Value);
        }
        UObject* Library = DefaultObjectOf(L"/Script/Engine.DataTableFunctionLibrary");
        Reflection::FParams Params(FunctionAt(L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow"), true);
        if (!Library || !Params.Set(L"CurveTable", CurveTable) || !Params.Set(L"RowName", static_cast<FName>(Curve.RowName)) || !Params.Set(L"InXY", Level)
            || !Params.Call(Library) || Params.Take<uint8_t>(L"OutResult") != 0)
        {
            return std::nullopt;
        }
        return ScalableFloat.Value * Params.Take<float>(L"OutXY");
    }

    // The magnitudes one effect can have: its curve at levels 0 to BoostCurveLevels, and the level that gives a
    // magnitude (the curve is piecewise linear, so a bisection inside the step that spans it finds it).
    struct FBoostCurve
    {
        const FScalableFloat* Magnitude = nullptr;
        std::vector<float> Values;
        float Min = 1.0f;
        float Max = 1.0f;

        bool Load(const FScalableFloat* InMagnitude)
        {
            Magnitude = InMagnitude;
            Values.clear();
            for (int32_t Level = 0; Level <= BoostCurveLevels; ++Level)
            {
                const std::optional<float> Value = ValueAtLevel(*Magnitude, static_cast<float>(Level));
                if (!Value)
                {
                    return false;
                }
                Values.push_back(*Value);
            }
            Min = *std::min_element(Values.begin(), Values.end());
            Max = *std::max_element(Values.begin(), Values.end());
            return true;
        }

        std::optional<float> LevelFor(float Wanted) const
        {
            for (int32_t Level = 0; Level < static_cast<int32_t>(Values.size()) - 1; ++Level)
            {
                float Low = Values[Level];
                float High = Values[Level + 1];
                if (Low == Wanted)
                {
                    return static_cast<float>(Level);
                }
                if ((Low - Wanted) * (High - Wanted) > 0.0f || Low == High)
                {
                    continue;
                }
                float From = static_cast<float>(Level);
                float To = From + 1.0f;
                for (int32_t Step = 0; Step < 24; ++Step)
                {
                    const float Middle = (From + To) * 0.5f;
                    const float Value = ValueAtLevel(*Magnitude, Middle).value_or(Low);
                    if ((Low - Wanted) * (Value - Wanted) <= 0.0f)
                    {
                        To = Middle;
                        High = Value;
                    }
                    else
                    {
                        From = Middle;
                        Low = Value;
                    }
                }
                return (From + To) * 0.5f;
            }
            return std::nullopt;
        }
    };

    // The ability system the stage's attribute is evaluated on: the pawn's (its IAbilitySystemInterface), when it has the
    // stage's attribute set.
    UAbilitySystemComponent* BoostAbilitySystem(AFortPlayerController* PlayerController, const FBoostStage& Stage)
    {
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        UAbilitySystemComponent* AbilitySystem = Pawn ? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn) : nullptr;
        if (!AbilitySystem)
        {
            return nullptr;
        }
        UClass* AttributeSet = BoostAttributeSet(Stage);
        const TArray<UAttributeSet*>& Sets = AbilitySystem->SpawnedAttributes;
        const bool bHasSet = AttributeSet && std::any_of(Sets.begin(), Sets.end(), [AttributeSet](UAttributeSet* Set) { return Set && Set->IsA(AttributeSet); });
        return bHasSet ? AbilitySystem : nullptr;
    }

    // The multiplier the stage's attribute has for its weapons, from every effect on it (perks included); for an additive
    // stage, what the effects add up to.
    std::optional<float> CurrentBoostMultiplier(UAbilitySystemComponent* AbilitySystem, const FBoostStage& Stage)
    {
        UProperty* Property = BoostAttribute(Stage);
        if (!AbilitySystem || !Property || !UAbilitySystemBlueprintLibrary::FindStaticFunction_EvaluateAttributeValueWithTagsAndBase())
        {
            return std::nullopt;
        }
        FGameplayTagContainer SourceTags;
        if (Stage.SourceTag)
        {
            SourceTags.GameplayTags.Add(FGameplayTag{ FName(Stage.SourceTag, FNAME_Find) });
            SourceTags.ParentTags.Add(FGameplayTag{ FName(Stage.SourceParentTag, FNAME_Find) });
        }
        bool bSuccess = false;
        const float Multiplier = UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTagsAndBase(AbilitySystem, FGameplayAttribute(Property), SourceTags, FGameplayTagContainer(),
            Stage.bAdditive ? 0.0f : 1.0f, bSuccess);
        return bSuccess ? std::optional<float>(Multiplier) : std::nullopt;
    }

    void RemoveBoostEffects(FAttributeBoost& Boost)
    {
        UAbilitySystemComponent* AbilitySystem = Boost.AbilitySystem.Get();
        UFunction* Remove = FunctionAt(L"/Script/GameplayAbilities.AbilitySystemComponent:RemoveActiveGameplayEffect");
        for (const uint64_t Handle : Boost.Handles)
        {
            Reflection::FParams Params(Remove, true);
            if (AbilitySystem && Params.Set(L"Handle", Handle) && Params.Set(L"StacksToRemove", -1))
            {
                Params.Call(AbilitySystem);
            }
        }
        Boost.Handles.clear();
    }

    // Applies one effect at the level to the ability system; the handle of the active effect, or nullopt.
    std::optional<uint64_t> ApplyBoostEffect(UAbilitySystemComponent* AbilitySystem, UClass* EffectClass, float Level)
    {
        Reflection::FParams Context(FunctionAt(L"/Script/GameplayAbilities.AbilitySystemComponent:MakeEffectContext"), true);
        if (!Context.Call(AbilitySystem))
        {
            return std::nullopt;
        }
        Reflection::FParams Outgoing(FunctionAt(L"/Script/GameplayAbilities.AbilitySystemComponent:MakeOutgoingSpec"), true);
        if (!Outgoing.Set(L"GameplayEffectClass", EffectClass) || !Outgoing.Set(L"Level", Level) || !Outgoing.CopyStruct(L"Context", Context.GetAddress(L"ReturnValue"))
            || !Outgoing.Call(AbilitySystem))
        {
            return std::nullopt;
        }
        // The handle's first member is its spec, null when the effect class is.
        const void* SpecHandle = Outgoing.GetAddress(L"ReturnValue");
        if (!SpecHandle || !*static_cast<void* const*>(SpecHandle))
        {
            return std::nullopt;
        }
        Reflection::FParams Apply(FunctionAt(L"/Script/GameplayAbilities.AbilitySystemComponent:BP_ApplyGameplayEffectSpecToSelf"), true);
        if (!Apply.CopyStruct(L"SpecHandle", SpecHandle) || !Apply.Call(AbilitySystem))
        {
            return std::nullopt;
        }
        uint64_t* Handle = Apply.Get<uint64_t>(L"ReturnValue");
        return Handle ? std::optional<uint64_t>(*Handle) : std::nullopt;
    }

    // Brings one stage's attribute from Current to Target (multipliers, 1 + the sum of every modifier's magnitude - 1)
    // with effects of the stage's: the effects' magnitudes have to add up to Target - Current, as a few effects of the
    // same magnitude when one alone can't reach it. Nothing, or why it couldn't.
    std::optional<FString> ApplyBoostStage(FAttributeBoost& Boost, UAbilitySystemComponent* AbilitySystem, const FBoostStage& Stage, float Current, float Target)
    {
        const wchar_t* const Name = Boost.Kind->Name;
        UClass* EffectClass = BoostEffectClass(Stage);
        const FScalableFloat* Magnitude = BoostMagnitude(EffectClass, Stage);
        FBoostCurve Curve;
        if (!Magnitude || !Curve.Load(Magnitude))
        {
            return FString::Printf(TEXT("this build has no %s effect for %s (%s on %s)"), Name, Stage.What, Stage.EffectPath, Stage.Attribute);
        }
        const float Wanted = Target - Current;
        if (std::fabs(Wanted) < 0.0001f)
        {
            return std::nullopt;
        }
        // The furthest one effect goes the way it has to: a multiplier from 1, an addition from 0.
        const float Neutral = Stage.bAdditive ? 0.0f : 1.0f;
        const float Reach = Wanted > 0.0f ? Curve.Max - Neutral : Curve.Min - Neutral;
        if (Wanted * Reach <= 0.0f)
        {
            return FString::Printf(TEXT("the %s effect for %s only goes from %.2fx to %.2fx a stack, so it can't make them %s"), Name, Stage.What, Curve.Min, Curve.Max,
                Wanted > 0.0f ? TEXT("higher") : TEXT("lower"));
        }
        const int32_t Count = static_cast<int32_t>(std::ceil(Wanted / Reach - 0.0001f));
        if (Count > MaxBoostEffects)
        {
            return FString::Printf(TEXT("that needs %d stacks of the %s effect for %s (at most %.2fx each); up to %.1fx works"), Count, Name, Stage.What, Curve.Max,
                1.0f + MaxBoostEffects * Reach / std::max(Current, 0.0001f));
        }
        // Each one's magnitude, kept inside what the curve has.
        const float Each = std::clamp(Neutral + Wanted / static_cast<float>(Count), Curve.Min, Curve.Max);
        const std::optional<float> Level = Curve.LevelFor(Each);
        if (!Level)
        {
            return FString::Printf(TEXT("no level of the %s effect gives %.3f"), Name, Each);
        }
        for (int32_t Index = 0; Index < Count; ++Index)
        {
            const std::optional<uint64_t> Handle = ApplyBoostEffect(AbilitySystem, EffectClass, *Level);
            if (!Handle)
            {
                return FString::Printf(TEXT("the ability system wouldn't apply the %s effect"), Name);
            }
            Boost.Handles.push_back(*Handle);
        }
        UE_LOG(LogFortCheats, Log, TEXT("%s: %s, %s: %d effects at level %.3f (%.3f each), %.3fx before"), Name, *GetNameSafe(Boost.Player.Get()), Stage.What, Count, *Level, Each, Current);
        return std::nullopt;
    }

    // Puts the boost's effects on the player's ability system so the attribute is Multiplier times what it is without
    // Core's effects, stage by stage. The first stage has to work; a later one that can't is left out and said so. The
    // multiplier each stage's weapons have afterwards (to what they had without Core's effects), or an error.
    struct FBoostResult
    {
        std::vector<float> Multipliers;
        std::vector<FString> Notes;
    };

    std::variant<FBoostResult, FString> ApplyBoost(FAttributeBoost& Boost)
    {
        RemoveBoostEffects(Boost);
        AFortPlayerController* PlayerController = Boost.Player.Get();
        const FBoostKind& Kind = *Boost.Kind;
        UAbilitySystemComponent* AbilitySystem = BoostAbilitySystem(PlayerController, Kind.Stages[0]);
        Boost.AbilitySystem = AbilitySystem;
        Boost.Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        if (!AbilitySystem)
        {
            return FString::Printf(TEXT("you have no pawn with the attributes it needs (%s) yet"), Kind.Stages[0].AttributeSetPath);
        }

        // What each stage has without Core's effects, before any stage changes it (a stage's effect may move another's).
        std::vector<std::optional<float>> Originals;
        for (size_t Index = 0; Index < Kind.NumStages; ++Index)
        {
            Originals.push_back(CurrentBoostMultiplier(BoostAbilitySystem(PlayerController, Kind.Stages[Index]), Kind.Stages[Index]));
        }

        FBoostResult Result;
        for (size_t Index = 0; Index < Kind.NumStages; ++Index)
        {
            const FBoostStage& Stage = Kind.Stages[Index];
            const std::optional<float> Current = Originals[Index] ? CurrentBoostMultiplier(AbilitySystem, Stage) : std::nullopt;
            std::optional<FString> Error = Current ? ApplyBoostStage(Boost, AbilitySystem, Stage, *Current,
                Stage.bAdditive ? *Originals[Index] + Boost.Multiplier : *Originals[Index] * Boost.Multiplier)
                                                   : std::optional<FString>(FString::Printf(TEXT("%s can't be measured on your ability system"), Stage.Attribute));
            if (Error && Index == 0)
            {
                RemoveBoostEffects(Boost);
                return *Error;
            }
            if (Error)
            {
                UE_LOG(LogFortCheats, Warning, TEXT("%s: %s: %s"), Kind.Name, *GetNameSafe(PlayerController), **Error);
                Result.Notes.push_back(*Error);
            }
        }
        for (size_t Index = 0; Index < Kind.NumStages; ++Index)
        {
            const std::optional<float> After = CurrentBoostMultiplier(AbilitySystem, Kind.Stages[Index]);
            const float Original = Originals[Index].value_or(1.0f);
            Result.Multipliers.push_back(After && Original > 0.0001f ? *After / Original : (Index == 0 ? Boost.Multiplier : 1.0f));
        }
        return Result;
    }

    // After a respawn the pawn (and maybe its ability system) is new, and the effects may be gone with the old one.
    void KeepBoost(FAttributeBoost& Boost)
    {
        AFortPlayerController* PlayerController = Boost.Player.Get();
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        if (!Pawn || Pawn == Boost.Pawn.Get())
        {
            return;
        }
        bool bStillApplied = Boost.AbilitySystem.Get() && Boost.AbilitySystem.Get() == BoostAbilitySystem(PlayerController, Boost.Kind->Stages[0]) && !Boost.Handles.empty();
        // The library's (static) one: the handle knows its ability system.
        UFunction* StackCount = FunctionAt(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:GetActiveGameplayEffectStackCount");
        UObject* Library = DefaultObjectOf(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary");
        if (bStillApplied && StackCount && Library)
        {
            Reflection::FParams Params(StackCount, true);
            bStillApplied = Params.Set(L"ActiveHandle", Boost.Handles.front()) && Params.Call(Library) && Params.Take<int32_t>(L"ReturnValue") > 0;
        }
        if (bStillApplied)
        {
            Boost.Pawn = Pawn;
            return;
        }
        Boost.Handles.clear();
        const std::variant<FBoostResult, FString> Result = ApplyBoost(Boost);
        if (const FString* Error = std::get_if<FString>(&Result))
        {
            UE_LOG(LogFortCheats, Warning, TEXT("%s: %s's new pawn %s: %s"), Boost.Kind->Name, *GetNameSafe(PlayerController), *GetNameSafe(Pawn), **Error);
        }
    }

    FAttributeBoost* FindBoost(const AFortPlayerController* PlayerController, const FBoostKind& Kind)
    {
        auto Found = std::find_if(AttributeBoosts.begin(), AttributeBoosts.end(),
            [PlayerController, &Kind](const FAttributeBoost& Boost) { return Boost.Kind == &Kind && Boost.Player.Get() == PlayerController; });
        return Found != AttributeBoosts.end() ? &*Found : nullptr;
    }

    // "2", "0.5", "2x": a multiplier from 0.05 to Max.
    std::optional<float> ParseMultiplier(std::wstring Word, float Max)
    {
        if (!Word.empty() && Word.back() == L'x')
        {
            Word.pop_back();
        }
        wchar_t* End = nullptr;
        const float Value = std::wcstof(Word.c_str(), &End);
        if (End && End != Word.c_str() && *End == L'\0' && std::isfinite(Value) && Value >= 0.05f && Value <= Max)
        {
            return Value;
        }
        return std::nullopt;
    }

    // ---- Dance ------------------------------------------------------------------------------------------------------

    // "cheat dance" (#152): the folders these builds keep their emotes in (AthenaDanceItemDefinition, EID_*).
    constexpr const wchar_t* EmoteFolders[] = {
        L"/Game/Athena/Items/Cosmetics/Dances/",
        L"/Game/Athena/Items/Cosmetics/VictoryPoses/",
        L"/Game/Athena/Items/Cosmetics/ConsumableEmotes/",
    };

    // The emotes the asset manager lists ("AthenaDance"), and those already loaded (any a player has played) it doesn't.
    std::vector<FCheatAsset> EmoteAssets()
    {
        std::vector<FCheatAsset> Emotes = ListAssets({ L"AthenaDance" });
        if (!UFortMontageItemDefinitionBase::StaticClass())
        {
            return Emotes;
        }
        for (TObjectIterator<UFortMontageItemDefinitionBase> It(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)); It; ++It)
        {
            const std::wstring Path = It->GetPathName().ToStdString();
            const bool bListed = std::any_of(Emotes.begin(), Emotes.end(), [&Path](const FCheatAsset& Asset) { return _wcsicmp(Asset.Path.c_str(), Path.c_str()) == 0; });
            if (!bListed && Path.starts_with(L"/Game/"))
            {
                Emotes.push_back({ Path, It->GetName().ToStdString() });
            }
        }
        return Emotes;
    }

    // ---- SetMaxHealth and Speed (#155) --------------------------------------------------------------------------------

    // "cheat setmaxhealth": the maximum a player asked for, and the one their pawn had of its own.
    struct FMaxHealth
    {
        TWeakObjectPtr<AFortPlayerController> Player;
        TWeakObjectPtr<AFortPawn> Pawn;
        float Wanted = 0.0f;
        float Own = 0.0f;
    };
    std::vector<FMaxHealth> MaxHealths;

    FMaxHealth* FindMaxHealth(const AFortPlayerController* PlayerController)
    {
        auto Found = std::find_if(MaxHealths.begin(), MaxHealths.end(), [PlayerController](const FMaxHealth& Entry) { return Entry.Player.Get() == PlayerController; });
        return Found != MaxHealths.end() ? &*Found : nullptr;
    }

    // Brings the pawn's MaxHealth to Wanted through its SetMaxHealth, which sets the attribute's base value. Effects on it
    // (Save the World's hero and perks add to it) make its value Scale * base + Add: a second base measures them, and
    // the base that gives Wanted goes on. An effect that overrides it (Battle Royale's GE_Map_Athena_Vitality_Health
    // holds it at 100, #155) leaves no base that gets there: its current value is then set outright, and kept so
    // (KeepMaxHealth). The maximum it has afterwards, or nullopt.
    std::optional<float> ApplyMaxHealth(AFortPawn* Pawn, float Wanted)
    {
        if (!Pawn->SetMaxHealth(Wanted))
        {
            return std::nullopt;
        }
        float Got = Pawn->GetMaxHealth();
        if (std::fabs(Got - Wanted) > 0.5f)
        {
            const float Probe = Wanted * 2.0f;
            Pawn->SetMaxHealth(Probe);
            const float Scale = (Pawn->GetMaxHealth() - Got) / (Probe - Wanted);
            Pawn->SetMaxHealth(Scale > 0.01f ? std::max(1.0f, Wanted + (Wanted - Got) / Scale) : Wanted);
            Got = Pawn->GetMaxHealth();
        }
        if (std::fabs(Got - Wanted) > 0.5f && Pawn->ForceMaxHealth(Wanted))
        {
            Got = Pawn->GetMaxHealth();
        }
        // Health's clamp follows the maximum it got either way, so the pawn can be healed to it (#155).
        Pawn->ForceMaxHealth(Got);
        return Got;
    }

    // A respawned (or new) pawn gets the maximum again, and its health with it. The pawn that has it keeps it: an effect
    // on MaxHealth that changes (or an override that held it) works its value out again, which goes back to the one asked.
    void KeepMaxHealth(FMaxHealth& Entry)
    {
        AFortPlayerController* PlayerController = Entry.Player.Get();
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        // The pawn a possession has them play is another player's: it keeps its own maximum.
        if (!Pawn || Pawn->IsDead() || UFortCheatManager::IsInPossession(PlayerController))
        {
            return;
        }
        if (Pawn == Entry.Pawn.Get())
        {
            Pawn->ForceMaxHealth(Entry.Wanted);
            return;
        }
        Entry.Pawn = Pawn;
        const float Current = Pawn->GetMaxHealth();
        if (std::fabs(Current - Entry.Wanted) > 0.5f && Current > 0.0f)
        {
            Entry.Own = Current;
        }
        if (const std::optional<float> Got = ApplyMaxHealth(Pawn, Entry.Wanted))
        {
            Pawn->SetHealth(*Got);
            UE_LOG(LogFortCheats, Log, TEXT("SetMaxHealth: %s's new pawn %s has %.0f maximum health"), *GetNameSafe(PlayerController), *GetNameSafe(Pawn), *Got);
        }
    }

    // "cheat speed" on a pawn that moves at its movement set's speeds (Save the World): the multiplier Core keeps on
    // whatever pawn the player has.
    struct FSpeedPlayer
    {
        TWeakObjectPtr<AFortPlayerController> Player;
        TWeakObjectPtr<AFortPawn> Pawn;
        float Multiplier = 1.0f;
    };
    std::vector<FSpeedPlayer> SpeedPlayers;

    void KeepSpeed(FSpeedPlayer& Entry)
    {
        AFortPlayerController* PlayerController = Entry.Player.Get();
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        if (Pawn && Pawn != Entry.Pawn.Get() && !Pawn->IsDead() && Pawn->MovesWithMovementSet())
        {
            // The set is the player state's, which the old pawn (still dying) scaled: it gives the set back first, so
            // the new pawn scales the player's own speeds and the old one's end doesn't undo them later.
            if (AFortPawn* OldPawn = Entry.Pawn.Get())
            {
                OldPawn->SetCheatSpeedMultiplier(1.0f);
            }
            Entry.Pawn = Pawn;
            Pawn->SetCheatSpeedMultiplier(Entry.Multiplier);
        }
    }

    // A Battle Royale pawn moves at the RunSpeed multiplier its ability system has (Core's move speed effects): the
    // owning client works it out the same way (StartSpeedView), so the two move it alike.
    void KeepRowSpeed(FAttributeBoost& Boost)
    {
        AFortPlayerController* PlayerController = Boost.Player.Get();
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        if (!Pawn || Pawn->MovesWithMovementSet())
        {
            return;
        }
        const std::optional<float> Multiplier = CurrentBoostMultiplier(BoostAbilitySystem(PlayerController, MoveSpeedStages[0]), MoveSpeedStages[0]);
        if (Multiplier && *Multiplier > 0.01f && std::fabs(*Multiplier - Pawn->GetCheatSpeedMultiplier()) > 0.001f)
        {
            Pawn->SetCheatSpeedMultiplier(*Multiplier);
        }
    }

    // Every frame on the game thread while a player has infinitemag, a fire rate, a weapon damage, a maximum health or
    // a speed: the magazine is topped up as soon as a shot takes from it, and a respawned pawn gets the effects, the
    // maximum health and the speed again. Nothing is hooked.
    void TickWeaponCheats()
    {
        std::erase_if(InfiniteMagPlayers, [](const TWeakObjectPtr<AFortPlayerController>& Known) { return !Known.Get(); });
        std::erase_if(AttributeBoosts, [](const FAttributeBoost& Boost) { return !Boost.Player.Get(); });
        std::erase_if(MaxHealths, [](const FMaxHealth& Entry) { return !Entry.Player.Get(); });
        std::erase_if(SpeedPlayers, [](const FSpeedPlayer& Entry) { return !Entry.Player.Get(); });
        for (const TWeakObjectPtr<AFortPlayerController>& Known : InfiniteMagPlayers)
        {
            FillMagazine(Known.Get());
        }
        for (FAttributeBoost& Boost : AttributeBoosts)
        {
            KeepBoost(Boost);
            if (Boost.Kind == &MoveSpeedBoost)
            {
                KeepRowSpeed(Boost);
            }
        }
        for (FMaxHealth& Entry : MaxHealths)
        {
            KeepMaxHealth(Entry);
        }
        for (FSpeedPlayer& Entry : SpeedPlayers)
        {
            KeepSpeed(Entry);
        }
        bTickingWeaponCheats = (!InfiniteMagPlayers.empty() || !AttributeBoosts.empty() || !MaxHealths.empty() || !SpeedPlayers.empty())
            && GameThread::Enqueue(&TickWeaponCheats);
    }

    void StartWeaponCheatTick()
    {
        if (!bTickingWeaponCheats)
        {
            bTickingWeaponCheats = GameThread::Enqueue(&TickWeaponCheats);
        }
    }
}
#endif

void UFortCheatManager::InfiniteMag(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.empty() ? std::wstring() : Lower(Words[0]);
    if (Words.size() > 1 || (!Word.empty() && Word != L"on" && Word != L"off"))
    {
        Reply(TEXT("Usage: infinitemag [on | off], your magazine never empties so you never reload; without on or off it toggles"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController)
    {
        return;
    }

    const bool bEnable = Word.empty() ? !HasInfiniteMag(PlayerController) : Word == L"on";
    std::erase_if(InfiniteMagPlayers, [PlayerController](const TWeakObjectPtr<AFortPlayerController>& Known) { return !Known.Get() || Known.Get() == PlayerController; });
    if (bEnable)
    {
        InfiniteMagPlayers.emplace_back(PlayerController);
    }
    // The game's own flag (replicated): the reserve never runs out, and the player's client knows to keep its own count
    // of the magazine full (StartInfiniteMagazineView).
    if (PlayerController->HasInfiniteAmmoFlag())
    {
        PlayerController->bInfiniteAmmo = bEnable;
    }
    // The game's own ammo saving, at a chance over 1, so a shot takes no round on the server or the client (#155); kept
    // through respawns like firerate's effects.
    if (FAttributeBoost* AmmoSave = FindBoost(PlayerController, AmmoSaveBoost))
    {
        RemoveBoostEffects(*AmmoSave);
        std::erase_if(AttributeBoosts, [AmmoSave](const FAttributeBoost& Boost) { return &Boost == AmmoSave; });
    }
    if (!bEnable)
    {
        Reply(TEXT("Infinite magazine OFF: your shots use up the magazine again"));
        return;
    }
    FAttributeBoost AmmoSave;
    AmmoSave.Kind = &AmmoSaveBoost;
    AmmoSave.Player = PlayerController;
    AmmoSave.Multiplier = InfiniteMagAmmoSaveChance;
    const std::variant<FBoostResult, FString> SaveResult = ApplyBoost(AmmoSave);
    if (const FString* Error = std::get_if<FString>(&SaveResult))
    {
        // Without it (no pawn yet, or a build without the effect) the magazine is still topped up every frame; a new
        // pawn tries again.
        UE_LOG(LogFortCheats, Warning, TEXT("InfiniteMag: %s's ammo saving: %s"), *GetNameSafe(PlayerController), **Error);
    }
    AttributeBoosts.push_back(std::move(AmmoSave));
    FillMagazine(PlayerController);
    StartWeaponCheatTick();
    UE_LOG(LogFortCheats, Log, TEXT("InfiniteMag: %s on"), *GetNameSafe(PlayerController));
    Reply(TEXT("Infinite magazine ON: your weapon stays loaded and never needs a reload (infinitemag off: normal again)"));
#endif
}

void UFortCheatManager::UnlimitedHealth(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // The game's ToggleUnlimitedHealth flips the flag without a word; this sets it and says so (#155).
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.empty() ? std::wstring() : Lower(Words[0]);
    if (Words.size() > 1 || (!Word.empty() && Word != L"on" && Word != L"off"))
    {
        Reply(TEXT("Usage: unlimitedhealth [on | off], nothing can hurt you; without on or off it toggles"));
        return;
    }
    if (!HasUnlimitedHealthFlag())
    {
        Reply(TEXT("This build's cheat manager has no bUnlimitedHealth"));
        return;
    }
    const bool bEnable = Word.empty() ? !bUnlimitedHealth : Word == L"on";
    bUnlimitedHealth = bEnable;
    UE_LOG(LogFortCheats, Log, TEXT("UnlimitedHealth: %s %s"), *GetNameSafe(GetPlayerController()), bEnable ? TEXT("on") : TEXT("off"));
    Reply(bEnable ? TEXT("Unlimited health ON: nothing can hurt you (unlimitedhealth off: normal again)")
                  : TEXT("Unlimited health OFF: you can be hurt again"));
#endif
}

void UFortCheatManager::Repair(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const bool bAll = Words.size() == 1 && Lower(Words[0]) == L"all";
    if (Words.size() > 1 || (!Words.empty() && !bAll))
    {
        Reply(TEXT("Usage: repair [all], your weapon (or with all, every item that wears out) back to full durability"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!Inventory)
    {
        Reply(TEXT("Repair needs your inventory, which you don't have yet"));
        return;
    }

    std::vector<UFortWorldItem*> Items;
    if (bAll)
    {
        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            if (Instances[Index])
            {
                Items.push_back(Instances[Index]);
            }
        }
    }
    else
    {
        AFortPawn* Pawn = PlayerController->MyFortPawn;
        AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
        UFortWorldItem* WorldItem = Weapon ? Inventory->FindWorldItem(static_cast<FGuid&>(Weapon->ItemEntryGuid)) : nullptr;
        if (!WorldItem)
        {
            Reply(TEXT("You have no weapon from your inventory in your hands (repair all: everything you carry)"));
            return;
        }
        Items.push_back(WorldItem);
    }

    // Each one through ModDurability with its full durability forced: the world item's own SetDurability, as the game's
    // wear does, and the replicated entry the client's inventory shows.
    int32_t Repaired = 0;
    int32_t Durable = 0;
    FFortItemList& ItemList = Inventory->Inventory;
    for (UFortWorldItem* WorldItem : Items)
    {
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP());
        if (!ItemDefinition || !WorldItem->HasDurability())
        {
            continue;
        }
        ++Durable;
        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        const float MaxDurability = ItemDefinition->GetMaxDurability(WorldItem->GetLevel());
        if (!(MaxDurability > 0.0f) || ItemEntry.Durability >= MaxDurability)
        {
            continue;
        }
        const FGuid ItemGuid = ItemEntry.ItemGuid;
        if (!PlayerController->ModDurability(ItemGuid, MaxDurability, true))
        {
            continue;
        }
        if (FFortItemEntry* ReplicatedEntry = ItemList.FindReplicatedEntry(ItemGuid))
        {
            ReplicatedEntry->Durability = static_cast<float>(ItemEntry.Durability);
            ItemList.MarkItemDirty(*ReplicatedEntry);
        }
        ++Repaired;
    }
    if (Repaired > 0)
    {
        Inventory->HandleInventoryLocalUpdate();
    }
    UE_LOG(LogFortCheats, Log, TEXT("Repair: %s, %d of %d items that wear out repaired"), *GetNameSafe(PlayerController), Repaired, Durable);
    if (!bAll)
    {
        Reply(Durable == 0 ? TEXT("Your weapon doesn't wear out")
            : Repaired > 0 ? TEXT("Your weapon is back to full durability")
                           : TEXT("Your weapon is already at full durability"));
        return;
    }
    Reply(Durable == 0 ? FString(TEXT("You carry nothing that wears out"))
                       : FString::Printf(TEXT("Repaired %d item%s to full durability (%d already were)"), Repaired, Repaired == 1 ? TEXT("") : TEXT("s"), Durable - Repaired));
#endif
}

#if WITH_SERVER_CODE
namespace
{
    // "firerate" and "weapondamage": a multiplier (Max at most) or reset for the player, as the reply to send.
    FString RunBoostCommand(AFortPlayerController* PlayerController, const FBoostKind& Kind, const std::wstring& Command, const FString& Arguments, float Max,
        const wchar_t* Example, const wchar_t* ResetReply)
    {
        const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
        const std::wstring Word = Words.size() == 1 ? Lower(Words[0]) : std::wstring();
        const bool bReset = Word == L"reset" || Word == L"off";
        const std::optional<float> Multiplier = !Word.empty() && !bReset ? ParseMultiplier(Word, Max) : std::nullopt;
        FAttributeBoost* Existing = FindBoost(PlayerController, Kind);
        const FString Usage = FString::Printf(TEXT("Usage: %s <multiplier 0.05-%.0f | reset>, %s; reset puts it back"), Command.c_str(), Max, Example);
        if (!bReset && !Multiplier)
        {
            return Existing ? FString::Printf(TEXT("Your %s is %.2fx. %s"), Kind.Name, Existing->Multiplier, *Usage) : Usage;
        }
        if (!PlayerController)
        {
            return FString();
        }

        if (Existing)
        {
            RemoveBoostEffects(*Existing);
            std::erase_if(AttributeBoosts, [Existing](const FAttributeBoost& Boost) { return &Boost == Existing; });
        }
        if (bReset || *Multiplier == 1.0f)
        {
            return FString(ResetReply);
        }

        FAttributeBoost Boost;
        Boost.Kind = &Kind;
        Boost.Player = PlayerController;
        Boost.Multiplier = *Multiplier;
        const std::variant<FBoostResult, FString> Result = ApplyBoost(Boost);
        if (const FString* Error = std::get_if<FString>(&Result))
        {
            return FString::Printf(TEXT("Can't change your %s: %s"), Kind.Name, **Error);
        }
        AttributeBoosts.push_back(std::move(Boost));
        StartWeaponCheatTick();

        const FBoostResult& Applied = std::get<FBoostResult>(Result);
        std::wstring Reply;
        for (size_t Index = 0; Index < Kind.NumStages && Index < Applied.Multipliers.size(); ++Index)
        {
            Reply += (Index == 0 ? L"" : L", ") + std::wstring(Kind.Stages[Index].What) + L" " + std::to_wstring(std::lround(Applied.Multipliers[Index] * 100.0f)) + L"%";
        }
        for (const FString& Note : Applied.Notes)
        {
            Reply += L" (" + Note.ToStdString() + L")";
        }
        return FString::Printf(TEXT("Your %s is %.2fx what it was: %s of before (%s reset: back to normal)"), Kind.Name, *Multiplier, Reply.c_str(), Command.c_str());
    }
}
#endif

void UFortCheatManager::FireRate(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const FString Message = RunBoostCommand(GetPlayerController(), FireRateBoost, L"firerate", Arguments, 100.0f,
        L"\"firerate 2\" fires twice as fast as your guns do on their own", L"Your guns fire at their own rate again");
    if (!Message.IsEmpty())
    {
        Reply(Message);
    }
#endif
}

void UFortCheatManager::WeaponDamage(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const FString Message = RunBoostCommand(GetPlayerController(), WeaponDamageBoost, L"weapondamage", Arguments, 100.0f,
        L"\"weapondamage 2\" makes your guns and melee weapons do twice the damage they do on their own", L"Your weapons do their own damage again");
    if (!Message.IsEmpty())
    {
        Reply(Message);
    }
#endif
}

void UFortCheatManager::MaxHealth(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // #155. Health is UFortHealthSet's Health and MaxHealth on the pawn's ability system; the pawn's own SetMaxHealth
    // and SetHealth set their base values there, which replicate (the HUD's health bar follows).
    constexpr float MaxValue = 1000000.0f;
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.size() == 1 ? Lower(Words[0]) : std::wstring();
    const bool bReset = Word == L"reset" || Word == L"off";
    wchar_t* End = nullptr;
    const float Value = !Word.empty() && !bReset ? std::wcstof(Word.c_str(), &End) : 0.0f;
    if (!bReset && (Word.empty() || !End || *End != L'\0' || !std::isfinite(Value) || Value < 1.0f || Value > MaxValue))
    {
        AFortPlayerController* PlayerController = GetPlayerController();
        AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
        Reply(FString::Printf(TEXT("%sUsage: maxhealth <1-%d | reset>, \"maxhealth 500\" makes your maximum health 500 and heals you to it"),
            Pawn ? *FString::Printf(TEXT("Your maximum health is %.0f. "), Pawn->GetMaxHealth()) : TEXT(""), static_cast<int32_t>(MaxValue)));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (!Pawn || Pawn->IsDead())
    {
        Reply(TEXT("You have no pawn that is alive to change the health of"));
        return;
    }
    if (!AFortPawn::FindStaticFunction_SetMaxHealth() || !AFortPawn::FindStaticFunction_SetHealth())
    {
        Reply(TEXT("MaxHealth needs FortPawn's SetMaxHealth and SetHealth, which this build doesn't have"));
        return;
    }

    FMaxHealth* Existing = FindMaxHealth(PlayerController);
    if (bReset)
    {
        if (!Existing)
        {
            Reply(FString::Printf(TEXT("Your maximum health is your own already (%.0f)"), Pawn->GetMaxHealth()));
            return;
        }
        const float Own = Existing->Own;
        MaxHealths.erase(MaxHealths.begin() + (Existing - MaxHealths.data()));
        const float Health = Pawn->GetHealth();
        const float Got = ApplyMaxHealth(Pawn, Own).value_or(Pawn->GetMaxHealth());
        Pawn->SetHealth(std::min(Health, Got));
        Reply(FString::Printf(TEXT("Your maximum health is your own again: %.0f"), Got));
        return;
    }

    const float Own = Existing ? Existing->Own : Pawn->GetMaxHealth();
    const std::optional<float> Got = ApplyMaxHealth(Pawn, Value);
    if (!Got)
    {
        Reply(TEXT("Your pawn wouldn't take a maximum health"));
        return;
    }
    Pawn->SetHealth(*Got);
    if (!Existing)
    {
        Existing = &MaxHealths.emplace_back();
        Existing->Player = PlayerController;
    }
    Existing->Pawn = Pawn;
    Existing->Wanted = Value;
    Existing->Own = Own;
    StartWeaponCheatTick();
    UE_LOG(LogFortCheats, Log, TEXT("MaxHealth: %s asked for %.0f, has %.0f (own %.0f), health %.0f"), *GetNameSafe(PlayerController), Value, *Got, Own, Pawn->GetHealth());
    Reply(FString::Printf(TEXT("Max health set to %.0f, health %.0f/%.0f%s (your own: %.0f; maxhealth reset gives it back)"), *Got, Pawn->GetHealth(), Pawn->GetMaxHealth(),
        std::fabs(*Got - Value) > 0.5f ? TEXT(", the game won't let it go further") : TEXT(""), Own));
#endif
}

void UFortCheatManager::HealthPercent(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // The game's SetHealthPercent (1.9 0x8179C0) sets the health to the pawn's maximum times the fraction and answers
    // nothing; this does the same with the maximum the pawn has now (maxhealth's included, #155) and says what it set.
    // 0 to 1 is a fraction ("0.5"), above 1 a percent ("50").
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    std::wstring Word = Words.size() == 1 ? Words[0] : std::wstring();
    if (!Word.empty() && Word.back() == L'%')
    {
        Word.pop_back();
    }
    wchar_t* End = nullptr;
    const float Value = !Word.empty() ? std::wcstof(Word.c_str(), &End) : -1.0f;
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (Word.empty() || !End || End == Word.c_str() || *End != L'\0' || !std::isfinite(Value) || Value < 0.0f || Value > 100.0f)
    {
        Reply(FString::Printf(TEXT("%sUsage: healthpercent <0-100>, \"healthpercent 50\" (or 0.5) puts your health at half your maximum"),
            Pawn ? *FString::Printf(TEXT("Your health is %.0f/%.0f. "), Pawn->GetHealth(), Pawn->GetMaxHealth()) : TEXT("")));
        return;
    }
    if (!Pawn || Pawn->IsDead())
    {
        Reply(TEXT("You have no pawn that is alive to change the health of"));
        return;
    }
    const float Fraction = Value > 1.0f ? Value / 100.0f : Value;
    const float Max = Pawn->GetMaxHealth();
    // At least 1: a pawn at 0 health that isn't dead is stuck between the two.
    if (!Pawn->SetHealth(std::max(1.0f, Max * Fraction)))
    {
        Reply(TEXT("HealthPercent needs FortPawn's SetHealth, which this build doesn't have"));
        return;
    }
    Reply(FString::Printf(TEXT("Health set to %.0f/%.0f (%.0f%%)"), Pawn->GetHealth(), Max, Fraction * 100.0f));
#endif
}

void UFortCheatManager::InfiniteDurability(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // The game's ToggleInfiniteDurability flips the controller's replicated bInfiniteDurability and writes the result to
    // the server's own console only (1.9 0x81DD30), so the player saw nothing.
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.empty() ? std::wstring() : Lower(Words[0]);
    if (Words.size() > 1 || (!Word.empty() && Word != L"on" && Word != L"off"))
    {
        Reply(TEXT("Usage: infinitedurability [on | off], your items don't wear out; without on or off it toggles"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController || !AFortPlayerController::HasInfiniteDurabilityFlag())
    {
        Reply(TEXT("This build has no infinite durability flag"));
        return;
    }
    const bool bEnable = Word.empty() ? !PlayerController->bInfiniteDurability : Word == L"on";
    PlayerController->bInfiniteDurability = bEnable;
    PlayerController->ForceNetUpdate();
    Reply(bEnable ? TEXT("Infinite durability ON: your items don't wear out") : TEXT("Infinite durability OFF: your items wear out again"));
#endif
}

void UFortCheatManager::Speed(const FString& Arguments)
{
#if WITH_SERVER_CODE
    // Up to 1000x (#155: 10 was too few); ParseMultiplier turns away anything not finite or under 0.05.
    constexpr float MaxMultiplier = 1000.0f;
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    const std::wstring Word = Words.size() == 1 ? Lower(Words[0]) : std::wstring();
    const bool bReset = Word == L"reset" || Word == L"off";
    const std::optional<float> Multiplier = !Word.empty() && !bReset ? ParseMultiplier(Word, MaxMultiplier) : std::nullopt;
    AFortPlayerController* PlayerController = GetPlayerController();
    FSpeedPlayer* const SetSpeed = [PlayerController]() -> FSpeedPlayer*
    {
        auto Found = std::find_if(SpeedPlayers.begin(), SpeedPlayers.end(), [PlayerController](const FSpeedPlayer& Entry) { return Entry.Player.Get() == PlayerController; });
        return Found != SpeedPlayers.end() ? &*Found : nullptr;
    }();
    FAttributeBoost* const RowSpeed = FindBoost(PlayerController, MoveSpeedBoost);
    if (!bReset && !Multiplier)
    {
        const float Current = SetSpeed ? SetSpeed->Multiplier : RowSpeed ? RowSpeed->Multiplier : 1.0f;
        Reply(FString::Printf(TEXT("You move at %.2fx your own speed. Usage: speed <0.05-%.0f | reset>, \"speed 2\" walks, runs and sprints twice as fast"),
            Current, MaxMultiplier));
        return;
    }
    AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (!Pawn)
    {
        Reply(TEXT("You have no pawn to change the speed of"));
        return;
    }

    // The last speed goes first.
    if (SetSpeed)
    {
        SpeedPlayers.erase(SpeedPlayers.begin() + (SetSpeed - SpeedPlayers.data()));
    }
    if (RowSpeed)
    {
        RemoveBoostEffects(*RowSpeed);
        std::erase_if(AttributeBoosts, [RowSpeed](const FAttributeBoost& Boost) { return &Boost == RowSpeed; });
    }
    Pawn->SetCheatSpeedMultiplier(1.0f);
    if (bReset || *Multiplier == 1.0f)
    {
        Reply(TEXT("Speed reset: you move at your own speed again"));
        return;
    }

    // Save the World: the movement set's walk, run and sprint speeds, which replicate to the player's client.
    if (Pawn->MovesWithMovementSet())
    {
        FSpeedPlayer& Entry = SpeedPlayers.emplace_back();
        Entry.Player = PlayerController;
        Entry.Pawn = Pawn;
        Entry.Multiplier = *Multiplier;
        const float OwnSpeed = Pawn->SetCheatSpeedMultiplier(*Multiplier);
        StartWeaponCheatTick();
        UE_LOG(LogFortCheats, Log, TEXT("Speed: %s moves at %.2fx (movement set, own run speed %.0f)"), *GetNameSafe(PlayerController), *Multiplier, OwnSpeed);
        Reply(FString::Printf(TEXT("Speed set to %.2fx: running at %.0f, your own %.0f (speed reset: your own speed)"), *Multiplier, OwnSpeed * *Multiplier, OwnSpeed));
        return;
    }

    // Battle Royale: the move speed effect carries the multiplier to the client; both sides scale the stats row by it.
    FAttributeBoost Boost;
    Boost.Kind = &MoveSpeedBoost;
    Boost.Player = PlayerController;
    Boost.Multiplier = *Multiplier;
    const std::variant<FBoostResult, FString> Result = ApplyBoost(Boost);
    if (const FString* Error = std::get_if<FString>(&Result))
    {
        Reply(FString::Printf(TEXT("Can't change your speed: %s"), **Error));
        return;
    }
    AttributeBoosts.push_back(std::move(Boost));
    StartWeaponCheatTick();
    const std::optional<float> Applied = CurrentBoostMultiplier(BoostAbilitySystem(PlayerController, MoveSpeedStages[0]), MoveSpeedStages[0]);
    const float Moved = Applied && *Applied > 0.01f ? *Applied : *Multiplier;
    const float OwnSpeed = Pawn->SetCheatSpeedMultiplier(Moved);
    UE_LOG(LogFortCheats, Log, TEXT("Speed: %s moves at %.3fx (asked %.2fx; stats row, own run speed %.0f)"), *GetNameSafe(PlayerController), Moved, *Multiplier, OwnSpeed);
    Reply(FString::Printf(TEXT("Speed set to %.2fx: running at %.0f, your own %.0f (speed reset: your own speed)"), Moved, OwnSpeed * Moved, OwnSpeed));
#endif
}

void UFortCheatManager::Dance(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    if (Words.size() > 1)
    {
        Reply(TEXT("Usage: dance [EID], plays the emote (\"dance EID_DanceMoves\", or its path); without one, a random emote"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController || !PlayerController->MyFortPawn)
    {
        Reply(TEXT("Dance needs a pawn to dance"));
        return;
    }
    UFunction* PlayEmote = AFortPlayerController::FindStaticFunction_ServerPlayEmoteItem();
    if (!PlayEmote)
    {
        Reply(TEXT("This build has no emotes (AFortPlayerController::ServerPlayEmoteItem wasn't found)"));
        return;
    }

    UFortMontageItemDefinitionBase* Emote = nullptr;
    const std::wstring Query = Words.empty() ? std::wstring() : Words[0];
    if (!Query.empty() && Query.front() == L'/')
    {
        Emote = LoadAsset<UFortMontageItemDefinitionBase>(Query);
    }
    else
    {
        const std::vector<FCheatAsset> Emotes = EmoteAssets();
        const FCheatAsset* Found = nullptr;
        if (Query.empty() && !Emotes.empty())
        {
            static std::mt19937 Random{ std::random_device{}() };
            Found = &Emotes[std::uniform_int_distribution<size_t>(0, Emotes.size() - 1)(Random)];
        }
        else if (!Query.empty())
        {
            Found = FindAsset(Emotes, Query, AFortGameModeAthena::GetCurrent(PlayerController) != nullptr);
        }
        if (Found)
        {
            Emote = LoadAsset<UFortMontageItemDefinitionBase>(Found->Path);
        }
        else if (!Query.empty())
        {
            // Not listed: the folders these builds keep their emotes in.
            for (const wchar_t* Folder : EmoteFolders)
            {
                const std::wstring Name = Lower(Query).starts_with(L"eid_") ? Query : L"EID_" + Query;
                Emote = LoadObject<UFortMontageItemDefinitionBase>(nullptr, (std::wstring(Folder) + Name + L"." + Name).c_str(), nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet));
                if (Emote)
                {
                    break;
                }
            }
        }
        if (!Emote)
        {
            Reply(Query.empty() ? FString(TEXT("This build lists no emotes to pick from (\"dance <EID or path>\" plays one by name)"))
                                : FString::Printf(TEXT("No emote called '%s' (an EID_* name, or its path: /Game/Athena/Items/Cosmetics/Dances/EID_DanceMoves)"), Query.c_str()));
            return;
        }
    }
    if (!Emote || !Emote->IsA(UFortMontageItemDefinitionBase::StaticClass()))
    {
        Reply(FString::Printf(TEXT("%s isn't an emote"), Query.c_str()));
        return;
    }

    // The emote wheel's own RPC, run here on the server: the build's body (or Core's, where 1.9 strips it) gives the pawn
    // the emote ability with the emote and activates it, and the montage replicates to everyone.
    Reflection::FParams Params(PlayEmote);
    if (!Params.Set(L"EmoteAsset", Emote) || !Params.Call(PlayerController))
    {
        Reply(TEXT("Couldn't play the emote"));
        return;
    }
    UE_LOG(LogFortCheats, Log, TEXT("Dance: %s plays %s"), *GetNameSafe(PlayerController), *Emote->GetName());
    Reply(FString::Printf(TEXT("Dancing %s (moving or using an item stops it)"), *Emote->GetName()));
#endif
}

#if !UE_SERVER
namespace
{
    // The client's half of "cheat infinitemag" (#149). A client counts its own magazine: its shots take from that count
    // at once, and the server's replicated AmmoCount only replaces it once one of the two is empty (AFortWeapon's
    // OnRep_AmmoCount), so a server that keeps AmmoCount full alone can't stop the client running dry and reloading.
    // While the server has bInfiniteAmmo on for this player, and its AmmoCount still has rounds, the client's count is
    // topped up through the weapon's own ModAmmo, which on a client changes only that count.
    void TickInfiniteMagazineView()
    {
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
        const TArray<ULocalPlayer*>* LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
        ULocalPlayer* Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
        AFortPlayerController* PlayerController = Player ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<APlayerController*>(Player->PlayerController))) : nullptr;
        AFortPawn* Pawn = PlayerController && PlayerController->HasInfiniteAmmoFlag() && PlayerController->bInfiniteAmmo
            ? Cast<AFortPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())) : nullptr;
        AFortWeapon* Weapon = Pawn && static_cast<ENetRole>(Pawn->Role) == ROLE_AutonomousProxy ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
        if (Weapon && static_cast<ENetRole>(Weapon->Role) != ROLE_Authority && static_cast<int32_t>(Weapon->AmmoCount) > 0)
        {
            const int32_t BulletsPerClip = Weapon->GetBulletsPerClip();
            const int32_t Local = Weapon->GetLocalMagazineAmmoCount();
            if (BulletsPerClip > 0 && Local < BulletsPerClip)
            {
                // Without the getter (-1), a whole magazine: ModAmmo stops at a full one.
                Weapon->ModAmmo(Local >= 0 ? BulletsPerClip - Local : BulletsPerClip);
            }
        }
        GameThread::Enqueue(&TickInfiniteMagazineView);
    }
}
#endif

void UFortCheatManager::StartInfiniteMagazineView()
{
#if !UE_SERVER
    static bool bStarted = false;
    if (!bStarted)
    {
        bStarted = GameThread::Enqueue(&TickInfiniteMagazineView);
    }
#endif
}

#if !UE_SERVER
namespace
{
    // The client's half of "cheat speed" in Battle Royale (#155). The pawn moves at its stats row's speeds, which nothing
    // replicates, and its movement is predicted here, so this client has to move it at the server's speed. The server
    // puts the game's move speed alteration (RunSpeed) on the pawn's ability system, which replicates here, and scales
    // the row by the RunSpeed multiplier it gives; four times a second this client does the same. A Save the World pawn
    // reads its movement set, which the server scales and replicates itself, so it is left alone.
    constexpr ULONGLONG SpeedViewMs = 250;
    ULONGLONG NextSpeedView = 0;

    void TickSpeedView()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now >= NextSpeedView)
        {
            NextSpeedView = Now + SpeedViewMs;
            UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
            UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
            const TArray<ULocalPlayer*>* LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
            ULocalPlayer* Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
            APlayerController* PlayerController = Player ? static_cast<APlayerController*>(Player->PlayerController) : nullptr;
            AFortPawn* Pawn = PlayerController ? Cast<AFortPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())) : nullptr;
            UClass* MovementSetClass = Pawn && static_cast<ENetRole>(Pawn->Role) == ROLE_AutonomousProxy && !Pawn->MovesWithMovementSet()
                ? FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortMovementSet") : nullptr;
            UProperty* RunSpeed = MovementSetClass ? MovementSetClass->FindPropertyByName(FName(L"RunSpeed", FNAME_Find)) : nullptr;
            UAbilitySystemComponent* AbilitySystem = RunSpeed && UAbilitySystemBlueprintLibrary::FindStaticFunction_EvaluateAttributeValueWithTagsAndBase()
                ? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn) : nullptr;
            // Only with the set: an attribute of a set the ability system doesn't have can't be evaluated.
            const TArray<UAttributeSet*>* Sets = AbilitySystem ? &static_cast<const TArray<UAttributeSet*>&>(AbilitySystem->SpawnedAttributes) : nullptr;
            const bool bHasSet = Sets && std::any_of(Sets->begin(), Sets->end(), [MovementSetClass](UAttributeSet* Set) { return Set && Set->IsA(MovementSetClass); });
            if (bHasSet)
            {
                bool bSuccess = false;
                const float Multiplier = UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTagsAndBase(AbilitySystem, FGameplayAttribute(RunSpeed), FGameplayTagContainer(), FGameplayTagContainer(), 1.0f, bSuccess);
                if (bSuccess && Multiplier > 0.01f && std::fabs(Multiplier - Pawn->GetCheatSpeedMultiplier()) > 0.001f)
                {
                    const float OwnSpeed = Pawn->SetCheatSpeedMultiplier(Multiplier);
                    UE_LOG(LogFortCheats, Log, TEXT("SpeedView: %s moves at %.3fx (own run speed %.0f)"), *GetNameSafe(Pawn), Multiplier, OwnSpeed);
                }
            }
        }
        GameThread::Enqueue(&TickSpeedView);
    }
}
#endif

void UFortCheatManager::StartSpeedView()
{
#if !UE_SERVER
    static bool bStarted = false;
    if (!bStarted)
    {
        bStarted = GameThread::Enqueue(&TickSpeedView);
    }
#endif
}

#if !UE_SERVER
namespace
{
    // ---- "cheat togglebloom" and "cheat togglerecoil" (#152), on the client ------------------------------------------

    // A ranged weapon's spread and recoil are this game's own: its shots are traced here, with the spread and recoil of
    // its stats row (FFortRangedWeaponStats in the item definition's WeaponStatHandle table, read again for every shot,
    // 1.9 0x3CF110), and only the hits go to the server. Spread is Spread times the aiming, crouching and moving
    // multipliers, and every shot heats it up by Spread again (1.9 0x933ED0); aiming down sights lerps it by
    // SpreadDownsights (0x95C590). Recoil is RecoilVert and RecoilHoriz (times their keyboard or gamepad scales) through
    // UFortWeaponAttrSet's recoil attributes, clamped at 0 (0x948070, 0x9419E0), or in Battle Royale, when the game's
    // console variable asks for it, a random magnitude and roll between AthenaRecoilMagnitudeMin/Max and
    // AthenaRecoilRollMagnitudeMin/Max (0x9339C0). Core sets those to 0 in the rows of the weapons this player holds, and
    // puts them back when the toggle goes off. The rows are shared by every weapon of the same stats here, which on this
    // client only this player shoots.
    enum class EWeaponStatCheat : uint8_t
    {
        Bloom,
        Recoil,
    };

    constexpr const wchar_t* BloomFields[] = { L"Spread", L"SpreadDownsights" };
    constexpr const wchar_t* RecoilFields[] = { L"RecoilVert", L"RecoilHoriz", L"AthenaRecoilMagnitudeMin", L"AthenaRecoilMagnitudeMax", L"AthenaRecoilRollMagnitudeMin", L"AthenaRecoilRollMagnitudeMax" };

    // A stats row field Core zeroed, and the value it had.
    struct FWeaponStatEdit
    {
        EWeaponStatCheat Cheat;
        TWeakObjectPtr<UDataTable> Table;
        FName RowName;
        int32_t Offset = 0;
        float Original = 0.0f;
    };
    std::vector<FWeaponStatEdit> WeaponStatEdits;
    bool bNoBloom = false;
    bool bNoRecoil = false;
    bool bTickingWeaponStats = false;

    AFortPlayerController* LocalFortPlayerController()
    {
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
        const TArray<ULocalPlayer*>* LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
        ULocalPlayer* Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
        return Player ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<APlayerController*>(Player->PlayerController))) : nullptr;
    }

    void ZeroWeaponStats(UDataTable* Table, FName RowName, EWeaponStatCheat Cheat)
    {
        UScriptStruct* RowStruct = Table->RowStruct;
        uint8_t* Row = RowStruct ? Table->FindRowUnchecked(RowName) : nullptr;
        if (!Row)
        {
            return;
        }
        const auto Fields = Cheat == EWeaponStatCheat::Bloom ? std::span<const wchar_t* const>(BloomFields) : std::span<const wchar_t* const>(RecoilFields);
        for (const wchar_t* Field : Fields)
        {
            // A melee row has none of them, and an early build may lack the Athena ones.
            const UProperty* Property = RowStruct->FindPropertyByName(FName(Field, FNAME_Find));
            if (!Property || Property->ElementSize != sizeof(float))
            {
                continue;
            }
            const int32_t Offset = Property->GetOffset_ForInternal();
            const bool bEdited = std::any_of(WeaponStatEdits.begin(), WeaponStatEdits.end(), [Table, RowName, Offset](const FWeaponStatEdit& Edit)
                { return Edit.Table.Get() == Table && Edit.RowName == RowName && Edit.Offset == Offset; });
            float& Value = *reinterpret_cast<float*>(Row + Offset);
            if (bEdited || Value == 0.0f)
            {
                continue;
            }
            WeaponStatEdits.push_back({ Cheat, TWeakObjectPtr<UDataTable>(Table), RowName, Offset, Value });
            Value = 0.0f;
        }
    }

    void RestoreWeaponStats(EWeaponStatCheat Cheat)
    {
        for (const FWeaponStatEdit& Edit : WeaponStatEdits)
        {
            UDataTable* Table = Edit.Cheat == Cheat ? Edit.Table.Get() : nullptr;
            if (uint8_t* Row = Table ? Table->FindRowUnchecked(Edit.RowName) : nullptr)
            {
                *reinterpret_cast<float*>(Row + Edit.Offset) = Edit.Original;
            }
        }
        std::erase_if(WeaponStatEdits, [Cheat](const FWeaponStatEdit& Edit) { return Edit.Cheat == Cheat; });
    }

    // Every frame while either is on: the weapon in the player's hands gets its row's fields zeroed as soon as it is
    // equipped. Nothing is hooked.
    void TickWeaponStatsView()
    {
        if (!bNoBloom && !bNoRecoil)
        {
            bTickingWeaponStats = false;
            return;
        }
        AFortPlayerController* PlayerController = LocalFortPlayerController();
        AFortPawn* Pawn = PlayerController ? Cast<AFortPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())) : nullptr;
        AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
        UFortWeaponItemDefinition* WeaponData = Weapon ? static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData) : nullptr;
        if (WeaponData)
        {
            const FDataTableRowHandle& StatHandle = WeaponData->WeaponStatHandle;
            UDataTable* Table = StatHandle.DataTable;
            const FName RowName = StatHandle.RowName;
            if (Table && !RowName.IsNone())
            {
                if (bNoBloom)
                {
                    ZeroWeaponStats(Table, RowName, EWeaponStatCheat::Bloom);
                }
                if (bNoRecoil)
                {
                    ZeroWeaponStats(Table, RowName, EWeaponStatCheat::Recoil);
                }
            }
        }
        bTickingWeaponStats = GameThread::Enqueue(&TickWeaponStatsView);
    }

    FString ToggleWeaponStats(EWeaponStatCheat Cheat, const std::wstring& Word)
    {
        bool& bOn = Cheat == EWeaponStatCheat::Bloom ? bNoBloom : bNoRecoil;
        bOn = Word.empty() ? !bOn : Word == L"on";
        if (!bOn)
        {
            RestoreWeaponStats(Cheat);
        }
        else if (!bTickingWeaponStats)
        {
            bTickingWeaponStats = GameThread::Enqueue(&TickWeaponStatsView);
        }
        if (Cheat == EWeaponStatCheat::Bloom)
        {
            return bOn ? FString(TEXT("No bloom ON: your guns shoot dead on where you aim (bloom off: normal)"))
                       : FString(TEXT("No bloom OFF: your guns spread as they do on their own"));
        }
        return bOn ? FString(TEXT("No recoil ON: your aim stays put when you fire (recoil off: normal)"))
                   : FString(TEXT("No recoil OFF: your guns kick as they do on their own"));
    }

    // ---- "cheat copytargetpath" (#152), on the client ---------------------------------------------------------------

    class APlayerCameraManagerFunctions : public UObject
    {
        DefineUnrealClass(APlayerCameraManager, Engine)
        DefineUFunction(GetCameraLocation)
        DefineUFunction(GetCameraRotation)

    public:
        FVector GetCameraLocation()
        {
            Reflection::FParams Params(FindStaticFunction_GetCameraLocation());
            return Params.Call(this) ? Params.Take<FVector>(L"ReturnValue") : FVector();
        }

        FRotator GetCameraRotation()
        {
            Reflection::FParams Params(FindStaticFunction_GetCameraRotation());
            return Params.Call(this) ? Params.Take<FRotator>(L"ReturnValue") : FRotator();
        }
    };

    // The object property Name of Object's class (a component's StaticMesh or SkeletalMesh), or null.
    UObject* ObjectPropertyOf(UObject* Object, const wchar_t* Name)
    {
        UClass* Class = Object ? Object->GetClass() : nullptr;
        const UProperty* Property = Class ? Class->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property || !Property->IsA(UObjectPropertyBase::StaticClass()) || Property->ElementSize != sizeof(UObject*))
        {
            return nullptr;
        }
        return *Property->ContainerPtrToValuePtr<UObject*>(Object);
    }

    // The clipboard, through Windows' own API (FPlatformApplicationMisc::ClipboardCopy isn't exported by these builds).
    bool CopyToClipboard(const std::wstring& Text)
    {
        if (!OpenClipboard(nullptr))
        {
            return false;
        }
        bool bCopied = false;
        const size_t Bytes = (Text.size() + 1) * sizeof(wchar_t);
        if (EmptyClipboard())
        {
            if (HGLOBAL Memory = GlobalAlloc(GMEM_MOVEABLE, Bytes))
            {
                if (void* Destination = GlobalLock(Memory))
                {
                    std::memcpy(Destination, Text.c_str(), Bytes);
                    GlobalUnlock(Memory);
                    bCopied = SetClipboardData(CF_UNICODETEXT, Memory) != nullptr;
                }
                if (!bCopied)
                {
                    GlobalFree(Memory);
                }
            }
        }
        CloseClipboard();
        return bCopied;
    }

    // What the player looks at: a trace from the camera along its view, past the player's own pawn. The hit actor's
    // name and class path, and the mesh its hit component shows; the class path is copied (spawnactor, tpactor and
    // disguise take it), or the mesh for an actor of a native class (a StaticMeshActor), unless Which picks one.
    FString CopyTargetPath(APlayerController* PlayerController, const std::wstring& Which)
    {
        if (!Which.empty() && Which != L"class" && Which != L"mesh" && Which != L"name")
        {
            return FString(TEXT("Usage: copytargetpath [class | mesh | name], copies the path of what you look at (the class, or the mesh of a mesh actor, without one)"));
        }
        UObject* CameraManager = reinterpret_cast<UObject*>(static_cast<APlayerCameraManager*>(PlayerController->PlayerCameraManager));
        if (!CameraManager || !APlayerCameraManagerFunctions::FindStaticFunction_GetCameraLocation() || !APlayerCameraManagerFunctions::FindStaticFunction_GetCameraRotation())
        {
            return FString(TEXT("Copytargetpath needs your camera"));
        }
        APlayerCameraManagerFunctions* Camera = static_cast<APlayerCameraManagerFunctions*>(CameraManager);
        const FVector Start = Camera->GetCameraLocation();
        const FRotator Rotation = Camera->GetCameraRotation();
        constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
        const float Pitch = Rotation.Pitch * DegreesToRadians;
        const float Yaw = Rotation.Yaw * DegreesToRadians;
        const FVector Direction(std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), std::sin(Pitch));
        const FVector End = Start + Direction * 100000.0f;

        // The pawn as the world context, so the trace goes through it (third person).
        APawn* Pawn = PlayerController->GetPawn();
        UObject* Context = Pawn ? reinterpret_cast<UObject*>(Pawn) : reinterpret_cast<UObject*>(PlayerController);
        FVector Impact;
        UObject* HitActor = nullptr;
        UObject* HitComponent = nullptr;
        // Visibility, then Camera for what only blocks the camera.
        if (!UKismetSystemLibrary::LineTraceSingle(Context, Start, End, 0, true, true, Impact, HitActor, HitComponent) || !HitActor)
        {
            UKismetSystemLibrary::LineTraceSingle(Context, Start, End, 1, true, true, Impact, HitActor, HitComponent);
        }
        if (!HitActor)
        {
            return FString(TEXT("You aren't looking at anything (nothing within 1000 m blocks your view)"));
        }

        const std::wstring Name = HitActor->GetName().ToStdString();
        UClass* Class = HitActor->GetClass();
        const std::wstring ClassPath = Class ? Class->GetPathName().ToStdString() : std::wstring();
        UObject* Mesh = ObjectPropertyOf(HitComponent, L"StaticMesh");
        if (!Mesh)
        {
            Mesh = ObjectPropertyOf(HitComponent, L"SkeletalMesh");
        }
        const std::wstring MeshPath = Mesh ? Mesh->GetPathName().ToStdString() : std::wstring();

        std::wstring Copied;
        if (Which == L"name")
        {
            Copied = Name;
        }
        else if (Which == L"mesh")
        {
            Copied = MeshPath;
        }
        else if (Which == L"class")
        {
            Copied = ClassPath;
        }
        else
        {
            Copied = !MeshPath.empty() && !ClassPath.starts_with(L"/Game/") ? MeshPath : ClassPath;
        }
        if (Copied.empty())
        {
            return FString::Printf(TEXT("%s (%s) has no %s to copy"), Name.c_str(), ClassPath.c_str(), Which.c_str());
        }
        const bool bCopied = CopyToClipboard(Copied);
        const float Distance = (Impact - Start).Size() / 100.0f;
        Log::Info(L"CopyTargetPath: {} ({}), mesh {}, {:.1f} m", Name, ClassPath, MeshPath.empty() ? std::wstring(L"none") : MeshPath, Distance);
        return FString::Printf(TEXT("%s %s%s. %.1f m away: %s, class %s%s%s"), bCopied ? TEXT("Copied") : TEXT("Couldn't copy"), Copied.c_str(),
            bCopied ? TEXT("") : TEXT(" (the clipboard is busy)"), Distance, Name.c_str(), ClassPath.c_str(), MeshPath.empty() ? TEXT("") : TEXT(", mesh "), MeshPath.c_str());
    }
}
#endif

bool UFortCheatManager::RunClientCheatCommand(APlayerController* PlayerController, const FString& Msg)
{
#if !UE_SERVER
    FString Command = Msg;
    FString TargetName;
    const bool bTargeted = SplitCheatTarget(Msg, Command, TargetName);
    const std::vector<std::wstring> Words = SplitWords(Command.ToStdString());
    // "bloom" and "recoil" too (#155).
    const auto IsClientCommand = [](const std::wstring& Candidate) { return Candidate == L"togglebloom" || Candidate == L"togglerecoil" || Candidate == L"copytargetpath"; };
    const std::wstring Name = Words.empty() ? std::wstring() : ResolveCommandName(PlayerController, Words[0], IsClientCommand);
    if (!IsClientCommand(Name))
    {
        return false;
    }

    FString Message;
    const std::wstring Word = Words.size() > 1 ? Lower(Words[1]) : std::wstring();
    if (bTargeted)
    {
        Message = FString::Printf(TEXT("%s only works on your own game: %s's game would have to run it"), Words[0].c_str(), *TargetName);
    }
    else if (Name == L"copytargetpath")
    {
        Message = Words.size() > 2 ? FString(TEXT("Usage: copytargetpath [class | mesh | name]")) : CopyTargetPath(PlayerController, Word);
    }
    else if (Words.size() > 2 || (!Word.empty() && Word != L"on" && Word != L"off"))
    {
        Message = FString::Printf(TEXT("Usage: %s [on | off]; without on or off it toggles"), Words[0].c_str());
    }
    else
    {
        Message = ToggleWeaponStats(Name == L"togglebloom" ? EWeaponStatCheat::Bloom : EWeaponStatCheat::Recoil, Word);
    }
    UE_LOG(LogFortCheats, Log, TEXT("%s: %s"), *Msg, *Message);
    if (PlayerController)
    {
        // A client function called on its own client runs here.
        PlayerController->ClientMessage(Message);
    }
    return true;
#else
    (void)PlayerController;
    (void)Msg;
    return false;
#endif
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

std::wstring UFortCheatManager::ResolveCommandName(APlayerController* PlayerController, const std::wstring& Name, bool (*IsKnown)(const std::wstring&))
{
    // The exec functions the console runs a command as: the controller's, its cheat manager's (this game's class when the
    // client has none) and its pawn's.
    UObject* CheatManagerObject = PlayerController ? static_cast<UObject*>(PlayerController->CheatManager) : nullptr;
    APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
    const UClass* Classes[] = {
        PlayerController ? PlayerController->GetClass() : nullptr,
        CheatManagerObject ? CheatManagerObject->GetClass() : UFortCheatManager::StaticClass(),
        Pawn ? Pawn->GetClass() : nullptr,
    };
    const auto IsCommand = [&](const std::wstring& Candidate)
    {
        if (IsKnown && IsKnown(Candidate))
        {
            return true;
        }
        const FName FunctionName(Candidate.c_str(), FNAME_Find);
        if (FunctionName.IsNone())
        {
            return false;
        }
        return std::any_of(std::begin(Classes), std::end(Classes), [&FunctionName](const UClass* Class)
        {
            UFunction* Function = Class ? Class->FindFunctionByName(FunctionName) : nullptr;
            return Function && Function->HasAnyFunctionFlags(FUNC_Exec);
        });
    };

    const std::wstring Lowered = Lower(Name);
    if (Lowered.empty() || IsCommand(Lowered))
    {
        return Lowered;
    }
    for (const wchar_t* Prefix : { L"set", L"toggle" })
    {
        if (IsCommand(Prefix + Lowered))
        {
            return Prefix + Lowered;
        }
    }
    return Lowered;
}

bool UFortCheatManager::ParseCheatMovement(const FString& Command, FString& OutCommand, float& OutSpeed, FString& OutError)
{
    // The speed is how fast the pawn runs while it flies, in cm/s; it walks and sprints faster by as much.
    constexpr float MaxSpeed = 100000.0f;
    const std::vector<std::wstring> Words = SplitWords(Command.ToStdString());
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
        const FString PlayerName = PlayerState->GetPlayerName();
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

