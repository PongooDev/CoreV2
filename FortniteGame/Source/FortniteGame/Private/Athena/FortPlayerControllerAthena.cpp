#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Abilities/FortHomebaseSet.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "GameThread.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Logging/LogMacros.h"
#include "Math/Rotator.h"
#include "Math/UnrealMathUtility.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

bool AFortPlayerControllerAthena::PlaysBattleRoyaleHeroes()
{
    return DefaultHeroes.Num() > 0 || Member_CustomizationLoadout.IsAvailable();
}

bool AFortPlayerControllerAthena::IsDefaultHero(UFortHero* Hero)
{
    UFortHeroType* HeroType = Hero ? Hero->GetHeroTypeBP() : nullptr;
    for (UFortHeroType* DefaultHero : DefaultHeroes)
    {
        if (DefaultHero && DefaultHero == HeroType)
        {
            return true;
        }
    }
    return HeroType && HeroType == GetLoadoutHero();
}

UFortHeroType* AFortPlayerControllerAthena::GetLoadoutHero()
{
    UAthenaCharacterItemDefinition* Character = Member_CustomizationLoadout.IsAvailable() ? static_cast<UAthenaCharacterItemDefinition*>(CustomizationLoadout.Character) : nullptr;
    return Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr;
}

UFortWorldItemDefinition* AFortPlayerControllerAthena::GetLoadoutPickaxe()
{
    // The loadout is the athena profile's favorite_* items, and nothing fills an empty favorite_pickaxe in: the
    // profile's loadout (0x41B610) replaces the whole loadout (0x41B2A0), and only an empty dance gets a fallback. The
    // pickaxe is then the one Athena_PlayerController_C's default loadout has.
    if (!Member_CustomizationLoadout.IsAvailable())
    {
        return nullptr;
    }
    UAthenaPickaxeItemDefinition* Pickaxe = CustomizationLoadout.Pickaxe;
    if (!Pickaxe)
    {
        const AFortPlayerControllerAthena* Default = static_cast<const AFortPlayerControllerAthena*>(GetClass()->GetDefaultObject(false));
        Pickaxe = Default ? static_cast<UAthenaPickaxeItemDefinition*>(Default->CustomizationLoadout.Pickaxe) : nullptr;
    }
    return Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr;
}

void AFortPlayerControllerAthena::FillEmptyLoadout(AFortPlayerPawnAthena* Pawn)
{
#if WITH_SERVER_CODE
    // One player without a glider crashed every 1.8 client in the match (Core-Backend #74, #80): every client sets up the
    // parachute of every player's pawn when its loadout replicates (OnRep_CustomizationLoadout, 0x43D4A0) and reads the
    // glider's mesh without checking it. The pawn's loadout is the controller's, copied by Possess (0x444480), and the
    // controller's is the athena profile's (GetOrInitializeHero, through GetAthenaLoadoutWithOverrides, 0x827300), which
    // replaces the whole loadout and fills only an empty character: a profile with no favorite_glider, or one the game
    // doesn't know, gave none. The class default's loadout has DefaultGlider and DefaultPickaxe in it.
    if (Role != ROLE_Authority || !Member_CustomizationLoadout.IsAvailable() || !FFortAthenaLoadout::HasGlider())
    {
        return;
    }
    const AFortPlayerControllerAthena* Default = static_cast<const AFortPlayerControllerAthena*>(GetClass()->GetDefaultObject(false));
    UAthenaGliderItemDefinition* DefaultGlider = Default ? static_cast<UAthenaGliderItemDefinition*>(Default->CustomizationLoadout.Glider) : nullptr;
    UAthenaPickaxeItemDefinition* DefaultPickaxe = Default ? static_cast<UAthenaPickaxeItemDefinition*>(Default->CustomizationLoadout.Pickaxe) : nullptr;
    if (!DefaultGlider)
    {
        DefaultGlider = LoadObject<UAthenaGliderItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Gliders/DefaultGlider.DefaultGlider");
    }
    if (!DefaultPickaxe)
    {
        DefaultPickaxe = LoadObject<UAthenaPickaxeItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Pickaxes/DefaultPickaxe.DefaultPickaxe");
    }
    if (!DefaultGlider || !DefaultPickaxe)
    {
        UE_LOG(LogFort, Error, TEXT("FillEmptyLoadout: DefaultGlider (%s) or DefaultPickaxe (%s) wasn't found"), *GetNameSafe(DefaultGlider), *GetNameSafe(DefaultPickaxe));
    }

    bool bFilledGlider = false;
    bool bFilledPickaxe = false;
    const auto Fill = [&](FFortAthenaLoadout& Loadout)
    {
        const bool bGlider = !Loadout.Glider && DefaultGlider;
        const bool bPickaxe = !Loadout.Pickaxe && DefaultPickaxe;
        if (bGlider)
        {
            Loadout.Glider = DefaultGlider;
        }
        if (bPickaxe)
        {
            Loadout.Pickaxe = DefaultPickaxe;
        }
        bFilledGlider |= bGlider;
        bFilledPickaxe |= bPickaxe;
        return bGlider || bPickaxe;
    };

    // The controller's loadout is filled too, so the pawns it possesses later get its copy whole.
    Fill(CustomizationLoadout);
    if (Pawn && Pawn->HasCustomizationLoadout() && Fill(Pawn->CustomizationLoadout))
    {
        Pawn->ForceNetUpdate();
    }

    if (bFilledGlider)
    {
        UE_LOG(LogFort, Warning, TEXT("FillEmptyLoadout: %s's loadout had no glider, it has %s"), *GetName(), *DefaultGlider->GetName());
    }
    if (bFilledPickaxe)
    {
        UE_LOG(LogFort, Warning, TEXT("FillEmptyLoadout: %s's loadout had no pickaxe, it has %s"), *GetName(), *DefaultPickaxe->GetName());
    }
#endif
}

UFortHero* AFortPlayerControllerAthena::GetOrInitializeHero()
{
    // The override (1.7.2 0x42E5E0) keeps the controller's hero, or makes a hero of the next of its DefaultHeroes (a
    // counter over every controller, divided by their number) and sets it through TrySetHeroImplementation. 1.8's
    // (0x4347F0) has no DefaultHeroes: it first makes the loadout from the athena profile (GetAthenaLoadoutWithOverrides,
    // 0x827300; a random character of AthenaGameData's when it has none) and then the hero of its character.
    if (!PlaysBattleRoyaleHeroes() || !Finder::ResolveAthenaHeroLive())
    {
        return nullptr;
    }
    return CallVirtual<UFortHero*>(this, Natives::AFortPlayerController_GetOrInitializeHeroIndex);
}

UFortHero* AFortPlayerControllerAthena::SetupHero()
{
    // Battle Royale played a level 1 Save the World soldier's stats (#73: Soldier_Balanced_UC_T1 in AttributesHeroScaling,
    // 440 health and 172 shield) instead of its hero's (GE_Map_Athena_Vitality_Health: 100 health and a 100 shield cap,
    // GE_Map_Athena_StartingShields: no shield). GetOrInitializeHero binds the player's UFortRegisteredPlayerInfo to the
    // player state before it makes the hero, and binding initializes the attributes (0x5EA3F0) with the controller's hero
    // or, while there is none, with UFortGameData::DefaultHero (HID_Commando_GrenadeGun_UC_T01). Setting the hero
    // afterwards doesn't initialize them again; ResetGameplayAttributes does, with AFortPlayerController::GetHero (0x808A80),
    // which only returns the hero whose id the player state holds. So with the pawn possessed the hero is set once more,
    // which puts its id back on the player state, before Possess resets the attributes with it.
    UFortHero* Hero = GetOrInitializeHero();
    if (Hero)
    {
        TrySetHeroImplementation(Hero, false, false);
    }

    // 1.10's in-match banner (the elimination screen's, #166) is the player state's Banner, which only the call that
    // makes the hero fills from the loadout (0x43B7D0: IconId and ColorId from BannerIconId and BannerColorId). A hero
    // made before the player's athena profile had arrived kept the loadout's default banner there, while the loadout
    // itself came from the profile; every later call returns that hero first. The player state takes the loadout's
    // banner here as that call does, whenever they differ.
    AFortPlayerStateAthena* AthenaPlayerState = AFortPlayerStateAthena::HasBanner() && Member_CustomizationLoadout.IsAvailable()
        ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
    if (AthenaPlayerState && Role == ROLE_Authority)
    {
        const FFortAthenaLoadout& Loadout = CustomizationLoadout;
        FPlayerBannerInfo& Banner = AthenaPlayerState->Banner;
        const FString& IconId = Loadout.BannerIconId;
        const FString& ColorId = Loadout.BannerColorId;
        if (IconId.Len() > 0 && (std::wcscmp(*Banner.IconId, *IconId) != 0 || std::wcscmp(*Banner.ColorId, *ColorId) != 0))
        {
            Banner.IconId = IconId;
            Banner.ColorId = ColorId;
            AthenaPlayerState->ForceNetUpdate();
            UE_LOG(LogFort, Log, TEXT("SetupHero: %s shows the banner %s, %s"), *GetName(), *IconId, *ColorId);
        }
    }
    return Hero;
}

void AFortPlayerControllerAthena::SetupBackpackSize()
{
    // The HUD's backpack showed "0 | 0" instead of "0 | 5" (#77). AthenaPickupManager's RefreshCounts shows
    // UFortInventoryContext::GetBackpackItemCounts (0x2DB19B0): the world inventory's limited items against its size
    // (0x63CA40), which is OverriddenBackpackSize when set, else the player info's ability system actor's (the player
    // state's) FortHomebaseSet.WorldInventorySizeBonus. Battle Royale's +5 to that is GE_Map_Athena_Inventory_StartingSize
    // in its heroes' CombinedStatGEs, which reach the player state only when Possess resets the attributes (#73, before
    // that never), after the starting items, and the HUD reads the size only when it's built and when the inventory
    // changes. The size goes on the controller instead, read from the same effects, before the starting items. It
    // replicates while the controller has a cheat manager (0x81FBA0), which every player gets when they join.
    // 1.8 showed "0 | 0" again (#106): its Athena_PlayerController_C has no DefaultHeroes. Its heroes are the loadout
    // characters' (HID_*_Athena_Commando_*, with the same effect), and the loadout is made when the pawn first spawns
    // (GetOrInitializeHero), so the size is set by Possess then, before it gives the loadout's pickaxe.
    std::vector<UFortHeroType*> Heroes(DefaultHeroes.begin(), DefaultHeroes.end());
    if (UFortHeroType* LoadoutHero = GetLoadoutHero())
    {
        Heroes.push_back(LoadoutHero);
    }
    if (Heroes.empty() && Member_CustomizationLoadout.IsAvailable())
    {
        return;
    }

    UClass* HomebaseSetClass = UFortHomebaseSet::StaticClass();
    const UProperty* WorldInventorySizeBonus = HomebaseSetClass ? HomebaseSetClass->FindPropertyByName(FName(L"WorldInventorySizeBonus", FNAME_Find)) : nullptr;
    for (UFortHeroType* HeroType : Heroes)
    {
        const int32_t BackpackSize = HeroType ? static_cast<int32_t>(HeroType->GetCombinedStatBonus(WorldInventorySizeBonus)) : 0;
        if (BackpackSize > 0)
        {
            OverriddenBackpackSize = BackpackSize;
            UE_LOG(LogFort, Log, TEXT("SetupBackpackSize: %s has a backpack of %d (%s)"), *GetName(), BackpackSize, *HeroType->GetName());
            return;
        }
    }
    UE_LOG(LogFort, Warning, TEXT("SetupBackpackSize: %s's heroes give no backpack size, it stays the attribute's"), *GetName());
}

void AFortPlayerControllerAthena::GiveLoadoutPickaxe()
{
#if WITH_SERVER_CODE
    // 1.8 players had no pickaxe (#106): 1.8's Athena_GameMode StartingItems are only the building pieces, the edit tool
    // and wood, the pickaxe being the loadout's (DefaultPickaxe: WID_Harvest_Pickaxe_Athena_C_T01), and nothing in the
    // shipping exe gives it (the controller's loadout pickaxe, +0x2900, is touched only by constructors). It goes in the
    // harvesting tool's slot, primary 0, before Possess activates the focused slot.
    UFortWorldItemDefinition* Pickaxe = Role == ROLE_Authority && WorldInventory ? GetLoadoutPickaxe() : nullptr;
    if (Pickaxe && GetItemQuantity(Pickaxe) <= 0)
    {
        UFortKismetLibrary::K2_GiveItemToPlayer(this, Pickaxe, 1, false);
        UE_LOG(LogFort, Log, TEXT("GiveLoadoutPickaxe: %s gets %s"), *GetName(), *Pickaxe->GetName());
    }
#endif
}

void AFortPlayerControllerAthena::DropInventoryOnDeath(AFortPawn* DeadPawn)
{
#if WITH_SERVER_CODE
    // An eliminated player's loot went with them (#89): the pawn-death handler's inventory drop (1.7.2 0x864F55, before
    // the death report and the elimination) is stripped. The only drop on death left is Save the World's backpack
    // (AFortPlayerController slot 0x2C0, 0x80DB20, "Removing %d items from inventory on death": one backpack pickup
    // sized by the game mode's DeathPenaltyData), which nothing calls either, and Battle Royale's Anthem_DeathPenalty
    // (respawn delays only) would leave it empty. Battle Royale drops each item as its own pickup: whole stacks of what
    // bCanBeDropped allows, so not the pickaxe or the edit tool, nor the building pieces (UFortBuildingItemDefinition's
    // constructor 0x6249D0 clears it), with the weapons' loaded ammo. bDropOnDeath is not the rule: in 1.7.2's content
    // only Athena_Shields sets it. The warmup island respawns the player with what they hold, so nothing drops until the
    // aircraft, when a death is an elimination (0x437C30 takes the player out of the alive players after Warmup).
    UWorld* World = DeadPawn && Role == ROLE_Authority ? GetWorld() : nullptr;
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortInventory* Inventory = WorldInventory;
    if (!GameState || GameState->GamePhase <= EAthenaGamePhase::GetWarmup() || !Inventory)
    {
        return;
    }

    std::vector<UFortWorldItem*> Drops;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* WorldItem = Instances[Index];
        FFortItemEntry* ItemEntry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = ItemEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition)) : nullptr;
        if (ItemDefinition && ItemDefinition->bCanBeDropped && ItemEntry->Count > 0)
        {
            Drops.push_back(WorldItem);
        }
    }

    const FVector DeathLocation = DeadPawn->GetTransform().GetLocation();
    const int32_t DropCount = static_cast<int32_t>(Drops.size());
    const std::vector<FVector> Spots = GetInventoryDropSpots(DeadPawn, DeathLocation, DropCount);

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    int32_t Dropped = 0;
    for (int32_t Index = 0; Index < DropCount; ++Index)
    {
        FFortItemEntry& ItemEntry = Drops[Index]->ItemEntry;
        const FGuid ItemGuid = ItemEntry.ItemGuid;
        const int32_t Count = ItemEntry.Count;
        EntryStruct->InitializeStruct(DropEntry);
        EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
        if (RemoveInventoryItem(ItemGuid, Count, false))
        {
            if (AFortPickup* Pickup = SpawnDropPickup(*DropEntry, &DeathLocation, nullptr))
            {
                Pickup->PawnWhoDroppedPickup = DeadPawn;
                Pickup->TossOntoFloor(Spots[Index], DeadPawn);
                ++Dropped;
            }
        }
        EntryStruct->DestroyStruct(DropEntry);
    }
    UE_LOG(LogFort, Log, TEXT("DropInventoryOnDeath: %s dropped %d of %d items"), *GetName(), Dropped, DropCount);
#endif
}

std::vector<FVector> AFortPlayerControllerAthena::GetInventoryDropSpots(AActor* DeadPawn, const FVector& DeathLocation, int32_t Count)
{
    // The pickups are tossed from the body to their own spots around it. A ring 80 out with 50 between them left a pile
    // (#89: pickups are 60 across, AFortPickup's constructor 0x65BA90), and rings from 1.5m a meter apart spread too wide.
    // They gather close around the body instead: rings from 75 out, 65 apart, with a hand's width between neighbours, each
    // ring turned against the last and each spot moved a little at random. The stripped death drop leaves nothing to say how far 1.7.2 threw them; the toss itself
    // (AFortPickup::TossPickup, 0x6622F0) only has a fallback, a pickup aimed at the spot it starts from goes 512 out in a
    // random direction (VRand, 0x4F4E10), so these distances are chosen. A wall between the body and a spot pulls the spot
    // in to the wall, and each spot is traced down to the floor below it, from a little over the body's height (the toss
    // lands a pickup where it is aimed and finds no ground, #72).
    constexpr float Pi = 3.1415926535897932f;
    constexpr float FirstRingRadius = 75.0f;
    constexpr float RingSpacing = 65.0f;
    constexpr float PickupSpacing = 70.0f;
    constexpr float PickupRadius = 30.0f;
    constexpr float TraceHeight = 50.0f;
    constexpr float TraceDepth = 100000.0f;
    std::vector<FVector> Spots;
    const float StartAngle = FMath::FRand() * 2.0f * Pi;
    for (float Radius = FirstRingRadius; static_cast<int32_t>(Spots.size()) < Count; Radius += RingSpacing)
    {
        const int32_t RingCount = std::min(std::max(static_cast<int32_t>(2.0f * Pi * Radius / PickupSpacing), 1), Count - static_cast<int32_t>(Spots.size()));
        const float Step = 2.0f * Pi / static_cast<float>(RingCount);
        const float RingStart = StartAngle + Radius / FirstRingRadius;
        for (int32_t Index = 0; Index < RingCount; ++Index)
        {
            const float Angle = RingStart + Step * (static_cast<float>(Index) + (FMath::FRand() - 0.5f) * 0.4f);
            const float Distance = Radius + (FMath::FRand() - 0.5f) * RingSpacing * 0.4f;
            const FVector Start(DeathLocation.X, DeathLocation.Y, DeathLocation.Z + TraceHeight);
            FVector Spot(Start.X + std::cos(Angle) * Distance, Start.Y + std::sin(Angle) * Distance, Start.Z);
            FVector WallHit;
            if (UKismetSystemLibrary::LineTraceSingle(DeadPawn, Start, Spot, 0, false, true, WallHit))
            {
                const float Reach = std::max(std::sqrt(FMath::Square(WallHit.X - Start.X) + FMath::Square(WallHit.Y - Start.Y)) - PickupRadius, 1.0f);
                Spot = FVector(Start.X + std::cos(Angle) * Reach, Start.Y + std::sin(Angle) * Reach, Start.Z);
            }
            FVector Floor = Spot;
            UKismetSystemLibrary::LineTraceSingle(DeadPawn, Spot, FVector(Spot.X, Spot.Y, Spot.Z - TraceDepth), 0, false, true, Floor);
            Spots.push_back(Floor);
        }
    }
    return Spots;
}

#if WITH_SERVER_CODE
namespace
{
    // A dead player whose client is still to be told where its controller stands.
    struct FPendingDeathView
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
        TWeakObjectPtr<AFortPawn> DeadPawn;
        FVector ViewLocation;
        float Deadline = 0.0f;
        int32_t Frames = 0;
    };

    // The client is told two frames after the death at the earliest, so the game's own death RPCs of that frame and the
    // pawn's replication (Pawn null) are ahead of it, and only before SpectateOnDeath's 5 seconds are up.
    constexpr int32_t DeathViewMinFrames = 2;
    constexpr float DeathViewWaitSeconds = 4.0f;

    std::vector<FPendingDeathView> PendingDeathViews;
    bool bWatchingDeathViews = false;

    // Moves the client's controller to ViewLocation: ClientSetSpectatorCamera (1.8.2 0x820630, 1.8.1 0x820E80, 1.8
    // 0x819040) is SetInitialLocationAndRotation (Athena's, 1.8.2 0x4366D0: APlayerController's 0x23B6B40, which moves
    // the controller and its spawn location and sets its control rotation, then SetViewTarget(this)) and SetViewTarget
    // (this). A controller that already views itself keeps its camera: APlayerCameraManager::SetViewTarget (0x23B75D0)
    // only drops a pending blend for the target it has.
    bool SendDeathView(AFortPlayerControllerAthena* Controller, const FVector& ViewLocation)
    {
        UClass* Class = Controller->GetClass();
        UFunction* SetSpectatorCamera = Class->FindFunctionByName(FName(L"ClientSetSpectatorCamera", FNAME_Find));
        UFunction* GetControlRotation = Class->FindFunctionByName(FName(L"GetControlRotation", FNAME_Find));
        if (!SetSpectatorCamera || !GetControlRotation)
        {
            return false;
        }

        // The rotation it has: with itself as its view target, Athena's GetControlRotation (1.8.2 0x41F5E0) is its root's,
        // which SetControlRotation keeps at the control rotation.
        Reflection::FParams RotationParams(GetControlRotation);
        RotationParams.Call(Controller);
        const FRotator* ControlRotation = RotationParams.Get<FRotator>(L"ReturnValue");
        Reflection::FParams Params(SetSpectatorCamera);
        Params.Set(L"CameraLocation", ViewLocation);
        Params.Set(L"CameraRotation", ControlRotation ? *ControlRotation : FRotator());
        Params.Call(Controller);
        return true;
    }

    void WatchDeathViews()
    {
        std::erase_if(PendingDeathViews, [](FPendingDeathView& Pending)
        {
            AFortPlayerControllerAthena* Controller = Pending.Controller.Get();
            UWorld* World = Controller ? Controller->GetWorld() : nullptr;
            if (!World)
            {
                return true;
            }
            // A new pawn (a respawn) has its own place, and once the time is up SpectateOnDeath may be watching someone.
            APawn* Pawn = Controller->GetPawn();
            const bool bNewPawn = Pawn && Pawn != static_cast<APawn*>(Pending.DeadPawn.Get());
            if (bNewPawn || World->GetTimeSeconds() > Pending.Deadline)
            {
                UE_LOG(LogFort, Log, TEXT("PlaceAtDeathLocation: %s's client wasn't told where it stands (%s)"), *Controller->GetName(),
                    bNewPawn ? TEXT("it has a new pawn") : Pawn ? TEXT("it never let its dead pawn go") : TEXT("it never viewed itself"));
                return true;
            }
            // Until the dead pawn is let go (the unpossess, at the end of the same death call, AFortPawn's 1.8.2
            // 0x7BE18F) and the controller views itself, the game's death handling isn't done.
            if (++Pending.Frames < DeathViewMinFrames || Pawn || Controller->GetViewTarget() != Controller)
            {
                return false;
            }
            const bool bSent = SendDeathView(Controller, Pending.ViewLocation);
            UE_LOG(LogFort, Log, TEXT("PlaceAtDeathLocation: %s's client %s it stands over its body"), *Controller->GetName(), bSent ? TEXT("is told") : TEXT("can't be told (no ClientSetSpectatorCamera)"));
            return true;
        });
        bWatchingDeathViews = !PendingDeathViews.empty() && GameThread::Enqueue(&WatchDeathViews);
    }
}
#endif

void AFortPlayerControllerAthena::PlaceAtDeathLocation(AFortPawn* DeadPawn)
{
#if WITH_SERVER_CODE
    // A dead player's controller goes where Fortnite puts a dead player's view, 160 over the body (ClientOnPawnDied's
    // PendingSpectatorLocation, 1.8.2 0x8724B0): it stood where it was put when the player joined (#117). The minimap
    // follows it. SFortMiniMapAthena's paint (1.8.2 0x30CAFB0, 1.8.1 0x30C8240, 1.8 0x30B6A40) puts the player's marker
    // at their pawn and, with no pawn, at their view target (GetViewTarget, slot 0x650), which after death is the
    // controller itself, turned by its GetControlRotation (slot 0x5F0). Controllers aren't moved by replication, so the
    // client's stays where it was spawned unless it is told (#139: the marker at Pleasant Park after a death at Dusty
    // Depot, 1.8.2).
    //
    // #117 told the client from here with ClientSetSpectatorCamera, which makes the controller its own view target too.
    // This is ahead of the game's own death handling (this handler then sends ClientOnPawnDied, 0x9F8560, and the pawn
    // lets the controller go at the end of the same death call), while the client still played its pawn, and it once
    // left the camera under the middle of the map (#125). So the client is now told a couple of frames later, once the
    // server's controller has let the pawn go and views itself (WatchDeathViews), when the client's view target is the
    // controller already or becomes it with the death camera the game gives it then anyway. The death camera itself is
    // the game's: the camera picks DeathCameraClass for a controller viewing itself (AFortPlayerCameraBase, 1.8.2
    // 0x4FDFB0), which stays where the camera last was (UFortDeathCameraMode, 0x519650) and turns to the death report's
    // KillerPawn. In a self-elimination (a fall, their own grenade: "YOU ELIMINATED YOURSELF!") that is the dead pawn,
    // so it looks down at the body, at the floor once the body is gone (#139's camera that showed only the floor). Five
    // seconds after the death, SpectateOnDeath (1.8.2 0x4387F0, the Athena handler's timer) spectates a teammate, the
    // killer or anyone still alive, and the minimap then follows them.
    constexpr float DeathViewHeight = 160.0f;
    UWorld* World = GetWorld();
    if (!DeadPawn || Role != ROLE_Authority || !World)
    {
        return;
    }

    const FVector Body = DeadPawn->GetTransform().GetLocation();
    const FVector ViewLocation(Body.X, Body.Y, Body.Z + DeathViewHeight);
    SetActorLocation(ViewLocation, false, true);
    std::erase_if(PendingDeathViews, [this](const FPendingDeathView& Pending) { return Pending.Controller.Get() == this; });
    PendingDeathViews.push_back({ TWeakObjectPtr<AFortPlayerControllerAthena>(this), TWeakObjectPtr<AFortPawn>(DeadPawn), ViewLocation, World->GetTimeSeconds() + DeathViewWaitSeconds });
    if (!bWatchingDeathViews)
    {
        bWatchingDeathViews = GameThread::Enqueue(&WatchDeathViews);
    }
    UE_LOG(LogFort, Log, TEXT("PlaceAtDeathLocation: %s stands over its body at (%.0f, %.0f, %.0f)"), *GetName(), ViewLocation.X, ViewLocation.Y, ViewLocation.Z);
#endif
}

void AFortPlayerControllerAthena::HookCallSites()
{
    if (uint8_t* Call = Finder::PawnDiedInventoryDropCall())
    {
        Hooks::RedirectCall(L"AFortPlayerControllerZone pawn-death inventory drop", Call, reinterpret_cast<void*>(&PawnDiedInventoryDropCall));
    }
}

void AFortPlayerControllerAthena::PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack)
{
    // The handler passes the controller's MyFortPawn. Battle Royale drops no backpack, so bDroppedBackpack stays false
    // and the other controllers keep the stub's nothing. It is also where the dead player's controller is moved to their
    // body, before the handler sends ClientOnPawnDied.
    if (AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)))
    {
        AthenaController->DropInventoryOnDeath(DeadPawn);
        AthenaController->PlaceAtDeathLocation(DeadPawn);
    }
}

void AFortPlayerControllerAthena::ResetToStartingItems()
{
#if WITH_SERVER_CODE
    // What the warmup island gave stayed for the whole match (#86). The aircraft phase (StartAircraftPhase, 1.7.2 0x3FFB60)
    // calls a stripped function with each controller after it destroys the warmup pawn (Finder::AircraftPhasePlayerCleanupCall;
    // on 1.8.1/1.8.2 the one just before, once the pawn's weapon is unequipped), which runs this: every item that isn't one of Athena_GameMode's StartingItems goes (weapons, ammo, consumables, traps,
    // and the materials, which start at 0), the starting items stay at their starting count.
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    AFortInventory* Inventory = WorldInventory;
    if (Role != ROLE_Authority || !GameMode || !Inventory)
    {
        return;
    }

    // 1.8's pickaxe is the loadout's, not a starting item: it stays too.
    const TStructArray<FItemAndCount>& StartingItems = GameMode->StartingItems;
    const UFortWorldItemDefinition* Pickaxe = GetLoadoutPickaxe();
    const auto StartingCount = [&StartingItems, Pickaxe](const UFortItemDefinition* ItemDefinition)
    {
        if (Pickaxe && ItemDefinition == Pickaxe)
        {
            return 1;
        }
        for (int32_t Index = 0; Index < StartingItems.Num(); ++Index)
        {
            const FItemAndCount& Entry = StartingItems[Index];
            if (static_cast<UFortItemDefinition*>(Entry.Item) == ItemDefinition)
            {
                return std::max(static_cast<int32_t>(Entry.Count), 0);
            }
        }
        return 0;
    };

    // Count -1 takes the whole item.
    std::vector<std::pair<FGuid, int32_t>> Removals;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        const int32_t Keep = Entry ? StartingCount(Entry->ItemDefinition) : 0;
        if (Entry && Entry->Count > Keep)
        {
            Removals.emplace_back(Entry->ItemGuid, Keep > 0 ? Entry->Count - Keep : -1);
        }
    }

    // A focused slot that empties would be activated again (ServerRemoveItemInternal), for a pawn that is gone. The
    // player leaves the aircraft with the harvesting tool (slot 0) instead, which Possess equips.
    AFortQuickBars* Bars = QuickBars;
    FQuickBar* PrimaryQuickBar = Bars ? &static_cast<FQuickBar&>(Bars->PrimaryQuickBar) : nullptr;
    const int32_t FocusedSlot = PrimaryQuickBar ? static_cast<int32_t>(PrimaryQuickBar->CurrentFocusedSlot) : -1;
    if (FocusedSlot > 0 && FocusedSlot < PrimaryQuickBar->Slots.Num())
    {
        const TArray<FGuid>& SlotItems = Bars->GetSlot(EFortQuickBars::GetPrimary(), FocusedSlot).Items;
        const bool bEmptied = std::all_of(SlotItems.begin(), SlotItems.end(), [&Removals](const FGuid& Guid)
        {
            return std::any_of(Removals.begin(), Removals.end(), [&Guid](const auto& Removal) { return Removal.first == Guid && Removal.second < 0; });
        });
        if (bEmptied)
        {
            PrimaryQuickBar->CurrentFocusedSlot = 0;
            PrimaryQuickBar->PreviousFocusedSlot = 0;
        }
    }

    for (const auto& [ItemGuid, Count] : Removals)
    {
        RemoveInventoryItem(ItemGuid, Count, true);
    }
    GiveStartingItems(StartingItems);
    UE_LOG(LogFort, Log, TEXT("ResetToStartingItems: %s boards the aircraft with its starting items, %d warmup items taken"), *GetName(), static_cast<int32_t>(Removals.size()));
#endif
}
