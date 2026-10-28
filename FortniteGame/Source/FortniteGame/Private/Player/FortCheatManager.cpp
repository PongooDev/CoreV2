#include "pch.h"
#include "Player/FortCheatManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortKismetLibrary.h"
#include "GameFramework/GameModeBase.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Finder.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
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

    UFortWorldItemDefinition* LoadItem(std::wstring Path)
    {
        // A package path names the asset of the same name: /Game/Items/Ingredients/Ingredient_Planks.Ingredient_Planks.
        const size_t Slash = Path.rfind(L'/');
        if (Slash != std::wstring::npos && Path.find(L'.', Slash) == std::wstring::npos)
        {
            Path += L"." + Path.substr(Slash + 1);
        }
        return LoadObject<UFortWorldItemDefinition>(nullptr, Path.c_str());
    }

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
    UE_LOG(LogFortCheats, Log, TEXT("%s: %s"), *GetNameSafe(GetPlayerController()), *Message);
    if (AFortPlayerController* PlayerController = GetPlayerController())
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

void UFortCheatManager::HookNatives()
{
    // Each of these is stripped on 1.2, 1.7.2 and 1.8 alike (Cheats.md): GiveWeapon, GiveAmmo, GiveTrap, GiveGadget(s),
    // GiveSpecificItem only free their string (1.7.2 0x7D01A0), GiveResources/Wood/Stone/Metal, GiveWeapons, GiveAllTraps,
    // GiveAllConsumables, GiveAllIngredients, RespawnPlayerServer and WannaGun are the shared "ret 0", and the rest call one of
    // those or something else compiled out (TeleportToLocation a virtual in Battle Royale, SetSafeZoneLocationToPlayer and
    // SetSafeZoneRadius the game mode's setters, GiveCheatInventory its load callback).
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    for (const auto& [Function, Native, Name] : {
             std::tuple{ StaticFunction_GiveWeapon(), &execGiveWeapon, L"UFortCheatManager::GiveWeapon" },
             std::tuple{ StaticFunction_GiveAmmo(), &execGiveAmmo, L"UFortCheatManager::GiveAmmo" },
             std::tuple{ StaticFunction_GiveTrap(), &execGiveTrap, L"UFortCheatManager::GiveTrap" },
             std::tuple{ StaticFunction_GiveGadget(), &execGiveGadget, L"UFortCheatManager::GiveGadget" },
             std::tuple{ StaticFunction_GiveGadgets(), &execGiveGadgets, L"UFortCheatManager::GiveGadgets" },
             std::tuple{ StaticFunction_GiveSpecificItem(), &execGiveSpecificItem, L"UFortCheatManager::GiveSpecificItem" },
             std::tuple{ StaticFunction_GiveResources(), &execGiveResources, L"UFortCheatManager::GiveResources" },
             std::tuple{ StaticFunction_GiveWood(), &execGiveWood, L"UFortCheatManager::GiveWood" },
             std::tuple{ StaticFunction_GiveStone(), &execGiveStone, L"UFortCheatManager::GiveStone" },
             std::tuple{ StaticFunction_GiveMetal(), &execGiveMetal, L"UFortCheatManager::GiveMetal" },
             std::tuple{ StaticFunction_GiveWeapons(), &execGiveWeapons, L"UFortCheatManager::GiveWeapons" },
             std::tuple{ StaticFunction_GiveAllMelee(), &execGiveAllMelee, L"UFortCheatManager::GiveAllMelee" },
             std::tuple{ StaticFunction_GiveAllPistols(), &execGiveAllPistols, L"UFortCheatManager::GiveAllPistols" },
             std::tuple{ StaticFunction_GiveAllRifles(), &execGiveAllRifles, L"UFortCheatManager::GiveAllRifles" },
             std::tuple{ StaticFunction_GiveAllShotguns(), &execGiveAllShotguns, L"UFortCheatManager::GiveAllShotguns" },
             std::tuple{ StaticFunction_GiveAllWeapons(), &execGiveAllWeapons, L"UFortCheatManager::GiveAllWeapons" },
             std::tuple{ StaticFunction_GiveAllTraps(), &execGiveAllTraps, L"UFortCheatManager::GiveAllTraps" },
             std::tuple{ StaticFunction_GiveAllConsumables(), &execGiveAllConsumables, L"UFortCheatManager::GiveAllConsumables" },
             std::tuple{ StaticFunction_GiveAllIngredients(), &execGiveAllIngredients, L"UFortCheatManager::GiveAllIngredients" },
             std::tuple{ StaticFunction_GiveUsefulThings(), &execGiveUsefulThings, L"UFortCheatManager::GiveUsefulThings" },
             std::tuple{ StaticFunction_GiveCheatInventory(), &execGiveCheatInventory, L"UFortCheatManager::GiveCheatInventory" },
             std::tuple{ StaticFunction_WannaGun(), &execWannaGun, L"UFortCheatManager::WannaGun" },
             std::tuple{ StaticFunction_TeleportToLocation(), &execTeleportToLocation, L"UFortCheatManager::TeleportToLocation" },
             std::tuple{ StaticFunction_SetSafeZoneLocationToPlayer(), &execSetSafeZoneLocationToPlayer, L"UFortCheatManager::SetSafeZoneLocationToPlayer" },
             std::tuple{ StaticFunction_SetSafeZoneRadius(), &execSetSafeZoneRadius, L"UFortCheatManager::SetSafeZoneRadius" },
             std::tuple{ StaticFunction_RespawnPlayerServer(), &execRespawnPlayerServer, L"UFortCheatManager::RespawnPlayerServer" },
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

