#include "pch.h"
#include "Player/FortCheatManager.h"
#include "Abilities/FortAbilityKit.h"
#include "Abilities/FortAbilitySet.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Components/SceneComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "FortKismetLibrary.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Pawns/FortAIPawn.h"
#include "Pawns/FortPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/ScriptInterface.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortDisguise, Log, All);

// "cheat hero" and "cheat disguise" / "cheat undisguise" (#134). Everything here goes through the game's and the engine's
// own functions (UFunctions through reflection, and Core's existing SetHeroType and Possess wrappers); nothing is hooked.

namespace
{
    // ------------------------------------------------------------------------------------------------------------------
    // Shared helpers
    // ------------------------------------------------------------------------------------------------------------------

    std::wstring Lower(std::wstring_view Text)
    {
        std::wstring Result(Text);
        std::transform(Result.begin(), Result.end(), Result.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
        return Result;
    }

    // Lower case without spaces, underscores or quotes.
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
                std::wstring Word(Line.substr(Start, End - Start));
                std::erase_if(Word, [](wchar_t Char) { return Char == L'"' || Char == L'\''; });
                if (!Word.empty())
                {
                    Words.push_back(std::move(Word));
                }
            }
            Start = End;
        }
        return Words;
    }

    // A package path names the asset of the same name: /Game/Athena/Heroes/HID_X is /Game/Athena/Heroes/HID_X.HID_X.
    std::wstring ObjectPathOf(const std::wstring& Path)
    {
        const size_t Slash = Path.rfind(L'/');
        if (Slash != std::wstring::npos && Path.find(L'.', Slash) == std::wstring::npos)
        {
            return Path + L"." + Path.substr(Slash + 1);
        }
        return Path;
    }

    template <typename T>
    T* LoadQuietly(const std::wstring& Path)
    {
        return T::StaticClass() ? LoadObject<T>(nullptr, ObjectPathOf(Path).c_str(), nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet)) : nullptr;
    }

    FVector ActorLocation(const AActor* Actor)
    {
        return Actor->GetTransform().GetLocation();
    }

    // The lowest point of the actor's colliding bounds (its feet, for a pawn), and its half height.
    float FeetOf(const AActor* Actor, float* OutHalfHeight = nullptr)
    {
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(true, Origin, Extent);
        if (Extent.Z <= 0.0f)
        {
            Origin = ActorLocation(Actor);
        }
        if (OutHalfHeight)
        {
            *OutHalfHeight = Extent.Z;
        }
        return Origin.Z - Extent.Z;
    }

    bool IsDeadPawn(APawn* Pawn)
    {
        AFortPawn* FortPawn = Pawn ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
        return FortPawn && FortPawn->IsDead();
    }

    bool IsInAircraftPhase(const UObject* WorldContextObject)
    {
        UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
        AFortGameStateAthena* GameState = World && AFortGameStateAthena::StaticClass() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        return GameState && GameState->GamePhase == EAthenaGamePhase::GetAircraft();
    }

    AFortPlayerState* FortPlayerStateOf(AController* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(Controller->PlayerState) : nullptr;
        return PlayerState ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(PlayerState)) : nullptr;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Hero: the hero type a name means, a hero of it, and its perks' and abilities' kits
    // ------------------------------------------------------------------------------------------------------------------

    UFortHeroType* AsHeroType(UObject* Object)
    {
        if (!Object)
        {
            return nullptr;
        }
        if (UFortHeroType::StaticClass() && Object->IsA(UFortHeroType::StaticClass()))
        {
            return static_cast<UFortHeroType*>(Object);
        }
        // A Battle Royale outfit (1.8 on): its hero.
        if (UAthenaCharacterItemDefinition::StaticClass() && Object->IsA(UAthenaCharacterItemDefinition::StaticClass()))
        {
            return static_cast<UFortHeroType*>(static_cast<UAthenaCharacterItemDefinition*>(Object)->HeroDefinition);
        }
        return nullptr;
    }

    // The heroes and outfits the asset manager lists (the primary asset types "Hero" and "AthenaCharacter"), loaded or not.
    std::vector<std::pair<std::wstring, std::wstring>> ListHeroAssets()
    {
        std::vector<std::pair<std::wstring, std::wstring>> Assets;
        UAssetManager* Manager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
        for (const wchar_t* Type : { L"Hero", L"AthenaCharacter" })
        {
            TArray<FAssetData> AssetDataList;
            if (!Manager || !Manager->GetPrimaryAssetDataList(FName(Type), AssetDataList))
            {
                continue;
            }
            for (int32_t Index = 0; Index < AssetDataList.Num(); ++Index)
            {
                Assets.emplace_back(AssetDataList[Index].AssetName.ToString().ToStdString(), AssetDataList[Index].ObjectPath.ToString().ToStdString());
            }
        }
        return Assets;
    }

    // Where the game keeps a hero or an outfit of this name: Battle Royale's outfits and heroes, and Save the World's
    // heroes by class (HID_Ninja_007_R_T01 under /Game/Heroes/Ninja/ItemDefinition).
    std::vector<std::wstring> GuessHeroPaths(const std::wstring& Name)
    {
        const std::wstring Wanted = Lower(Name);
        std::vector<std::wstring> Paths;
        if (Wanted.starts_with(L"cid_"))
        {
            Paths.push_back(L"/Game/Athena/Items/Cosmetics/Characters/" + Name);
        }
        if (Wanted.starts_with(L"hid_"))
        {
            Paths.push_back(L"/Game/Athena/Heroes/" + Name);
            const size_t ClassEnd = Name.find(L'_', 4);
            if (ClassEnd != std::wstring::npos)
            {
                Paths.push_back(L"/Game/Heroes/" + Name.substr(4, ClassEnd - 4) + L"/ItemDefinition/" + Name);
            }
            for (const wchar_t* HeroClass : { L"Commando", L"Constructor", L"Ninja", L"Outlander" })
            {
                Paths.push_back(std::wstring(L"/Game/Heroes/") + HeroClass + L"/ItemDefinition/" + Name);
            }
        }
        return Paths;
    }

    // The hero type a "cheat hero" argument means: a path, an HID_* or CID_* name, or part of one.
    UFortHeroType* FindHeroType(const std::wstring& Query, std::wstring& OutFound)
    {
        if (Query.empty())
        {
            return nullptr;
        }
        if (Query.front() == L'/')
        {
            UObject* Object = LoadQuietly<UFortItemDefinition>(Query);
            OutFound = Object ? Object->GetName().ToStdString() : std::wstring();
            return AsHeroType(Object);
        }

        // Loaded already, by its exact name.
        if (UFortItemDefinition* Loaded = FindObject<UFortItemDefinition>(ANY_PACKAGE, Query.c_str()))
        {
            if (UFortHeroType* HeroType = AsHeroType(Loaded))
            {
                OutFound = Loaded->GetName().ToStdString();
                return HeroType;
            }
        }

        // The asset manager's heroes and outfits, by name or (shortest first) part of it.
        const std::wstring Wanted = Lower(Query);
        const std::wstring WantedSimplified = Simplified(Query);
        const std::vector<std::pair<std::wstring, std::wstring>> Assets = ListHeroAssets();
        const std::pair<std::wstring, std::wstring>* Partial = nullptr;
        for (const auto& Asset : Assets)
        {
            if (Lower(Asset.first) == Wanted)
            {
                if (UFortHeroType* HeroType = AsHeroType(LoadQuietly<UFortItemDefinition>(Asset.second)))
                {
                    OutFound = Asset.first;
                    return HeroType;
                }
            }
            else if (!WantedSimplified.empty() && Simplified(Asset.first).find(WantedSimplified) != std::wstring::npos && (!Partial || Asset.first.size() < Partial->first.size()))
            {
                Partial = &Asset;
            }
        }

        // Where the game keeps them, for a whole name.
        for (const std::wstring& Path : GuessHeroPaths(Query))
        {
            if (UObject* Object = LoadQuietly<UFortItemDefinition>(Path))
            {
                if (UFortHeroType* HeroType = AsHeroType(Object))
                {
                    OutFound = Object->GetName().ToStdString();
                    return HeroType;
                }
            }
        }

        if (Partial)
        {
            if (UFortHeroType* HeroType = AsHeroType(LoadQuietly<UFortItemDefinition>(Partial->second)))
            {
                OutFound = Partial->first;
                return HeroType;
            }
        }

        // Part of the name of a hero already loaded.
        UFortHeroType* Best = nullptr;
        if (!UFortHeroType::StaticClass() || WantedSimplified.empty())
        {
            return nullptr;
        }
        for (TObjectIterator<UFortHeroType> It(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)); It; ++It)
        {
            const std::wstring Name = It->GetName().ToStdString();
            if (!WantedSimplified.empty() && Simplified(Name).find(WantedSimplified) != std::wstring::npos && (!Best || Name.size() < Best->GetName().ToStdString().size()))
            {
                Best = *It;
            }
        }
        OutFound = Best ? Best->GetName().ToStdString() : std::wstring();
        return Best;
    }

    // Battle Royale's own heroes (1.8's HID_*_Athena_Commando_* under /Game/Athena, 1.7.2's HID_Commando_Athena_*) are
    // only looks: their kits are Save the World's soldier perks.
    bool IsBattleRoyaleHero(const UFortHeroType* HeroType)
    {
        const std::wstring Path = Lower(HeroType->GetPathName().ToStdString());
        return Path.find(L"/athena/") != std::wstring::npos || Lower(HeroType->GetName().ToStdString()).find(L"athena") != std::wstring::npos;
    }

    // The kits of every slot of the hero's specializations (its perks, abilities and their upgrades), as at its highest
    // level: a slot's kit replaces the one it names as removed.
    std::vector<UFortAbilityKit*> SpecializationKits(const TArray<UFortHeroSpecialization*>& Specializations)
    {
        std::vector<UFortAbilityKit*> Granted;
        std::vector<UFortAbilityKit*> Removed;
        for (UFortHeroSpecialization* Specialization : Specializations)
        {
            if (!Specialization)
            {
                continue;
            }
            for (TStructArray<FFortSpecializationSlot>* Slots : { &static_cast<TStructArray<FFortSpecializationSlot>&>(Specialization->SpecializationSlots), &static_cast<TStructArray<FFortSpecializationSlot>&>(Specialization->TacticalAbilitySlots) })
            {
                for (int32_t Index = 0; Index < Slots->Num(); ++Index)
                {
                    FFortSpecializationSlot& Slot = (*Slots)[Index];
                    const TAssetPtr<UFortAbilityKit>& GrantedKit = Slot.GrantedAbilityKit;
                    const TAssetPtr<UFortAbilityKit>& RemovedKit = Slot.RemovedAbilityKit;
                    if (UFortAbilityKit* Kit = GrantedKit.LoadSynchronous(); Kit && std::find(Granted.begin(), Granted.end(), Kit) == Granted.end())
                    {
                        Granted.push_back(Kit);
                    }
                    if (UFortAbilityKit* Kit = RemovedKit.LoadSynchronous())
                    {
                        Removed.push_back(Kit);
                    }
                }
            }
        }
        std::erase_if(Granted, [&Removed](UFortAbilityKit* Kit) { return std::find(Removed.begin(), Removed.end(), Kit) != Removed.end(); });
        return Granted;
    }

    // A hero of the type, as GetOrInitializeHero makes the loadout's (1.8 0x4347F0): its gender and specializations from
    // the type, which SetHeroType takes the character parts from, at the type's highest level, with every perk.
    UFortHero* CreateHero(UFortHeroType* HeroType)
    {
        const int32_t Level = UFortHeroType::HasMaxLevel() ? std::max(static_cast<int32_t>(HeroType->MaxLevel), 1) : 1;
        UFortItem* Item = HeroType->CreateTemporaryItemInstanceBP(1, Level);
        UFortHero* Hero = Item && UFortHero::StaticClass() ? Cast<UFortHero>(reinterpret_cast<UObject*>(Item)) : nullptr;
        if (!Hero)
        {
            UE_LOG(LogFortDisguise, Error, TEXT("CreateHero: %s made %s, not a hero"), *HeroType->GetName(), *GetNameSafe(Item));
            return nullptr;
        }

        if (UFortHero::HasGender() && UFortHeroType::HasGender() && HeroType->Gender != 0)
        {
            Hero->Gender = HeroType->Gender;
        }
        TArray<UFortHeroSpecialization*>& Specializations = Hero->Specializations;
        if (Specializations.Num() == 0)
        {
            const TArray<TAssetPtr<UFortHeroSpecialization>>& TypeSpecializations = HeroType->Specializations;
            for (const TAssetPtr<UFortHeroSpecialization>& Specialization : TypeSpecializations)
            {
                if (UFortHeroSpecialization* Loaded = Specialization.LoadSynchronous())
                {
                    Specializations.Add(Loaded);
                }
            }
        }
        TArray<UFortAbilityKit*>& Kits = Hero->SpecializationAbilityKits;
        if (Kits.Num() == 0)
        {
            for (UFortAbilityKit* Kit : SpecializationKits(Specializations))
            {
                Kits.Add(Kit);
            }
        }
        return Hero;
    }

    UClass* AbilitySystemInterfaceClass()
    {
        static UClass* Class = FindObject<UClass>(nullptr, L"/Script/GameplayAbilities.AbilitySystemInterface");
        return Class;
    }

    // UFortKismetLibrary::EquipFortAbilitySet on the player state's ability system, keeping the handle to take it off.
    bool EquipKit(AFortPlayerState* PlayerState, UFortAbilitySet* Kit, std::vector<uint8_t>& OutHandle)
    {
        UFunction* Equip = UFortKismetLibrary::FindStaticFunction_EquipFortAbilitySet();
        UScriptStruct* HandleStruct = FFortAbilitySetHandle::StaticStruct();
        UClass* InterfaceClass = AbilitySystemInterfaceClass();
        void* Interface = InterfaceClass && Natives::UObject_GetInterfaceAddress ? Natives::UObject_GetInterfaceAddress(PlayerState, InterfaceClass) : nullptr;
        if (!Equip || !HandleStruct || !Interface || !Kit)
        {
            return false;
        }

        Reflection::FParams Params(Equip, true);
        Params.Set(L"AbilitySystemInterfaceActor", FScriptInterface(PlayerState, Interface));
        Params.Set(L"AbilitySet", Kit);
        Params.Set(L"OverrideSourceObject", static_cast<UObject*>(nullptr));
        if (!Params.Call(UFortKismetLibrary::StaticClass()->GetDefaultObject()))
        {
            return false;
        }
        OutHandle.assign(HandleStruct->GetStructureSize(), 0);
        HandleStruct->InitializeStruct(OutHandle.data());
        if (const void* Returned = Params.GetAddress(L"ReturnValue"))
        {
            HandleStruct->CopyScriptStruct(OutHandle.data(), Returned);
        }
        return true;
    }

    void UnequipKit(std::vector<uint8_t>& Handle)
    {
        UScriptStruct* HandleStruct = FFortAbilitySetHandle::StaticStruct();
        if (Handle.empty() || !HandleStruct)
        {
            return;
        }
        if (UFunction* Unequip = UFortKismetLibrary::FindStaticFunction_UnequipFortAbilitySet())
        {
            Reflection::FParams Params(Unequip, true);
            if (Params.CopyStruct(L"AbilitySetHandle", Handle.data()))
            {
                Params.Call(UFortKismetLibrary::StaticClass()->GetDefaultObject());
            }
        }
        HandleStruct->DestroyStruct(Handle.data());
        Handle.clear();
    }

    // Battle Royale: the hero a player chose, worn again after each respawn, and the kits Core equipped for it.
    struct FHeroSwap
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        std::wstring HeroTypePath;
        std::vector<std::vector<uint8_t>> KitHandles;
    };
    std::vector<FHeroSwap> HeroSwaps;

    // Save the World: the hero the player had before the first "cheat hero", for "cheat hero reset".
    struct FOriginalHero
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        TWeakObjectPtr<UFortHero> Hero;
    };
    std::vector<FOriginalHero> OriginalHeroes;

    void ForgetDepartedPlayers()
    {
        std::erase_if(HeroSwaps, [](FHeroSwap& Swap)
        {
            if (Swap.Controller.IsValid())
            {
                return false;
            }
            for (std::vector<uint8_t>& Handle : Swap.KitHandles)
            {
                UnequipKit(Handle);
            }
            return true;
        });
        std::erase_if(OriginalHeroes, [](const FOriginalHero& Original) { return !Original.Controller.IsValid(); });
    }

    FHeroSwap* FindHeroSwap(const AFortPlayerController* PlayerController)
    {
        auto Found = std::find_if(HeroSwaps.begin(), HeroSwaps.end(), [PlayerController](const FHeroSwap& Swap) { return Swap.Controller.Get() == PlayerController; });
        return Found != HeroSwaps.end() ? &*Found : nullptr;
    }

    // Takes the kits of the player's last swap off and forgets it.
    void ForgetHeroSwap(const AFortPlayerController* PlayerController)
    {
        if (FHeroSwap* Swap = FindHeroSwap(PlayerController))
        {
            for (std::vector<uint8_t>& Handle : Swap->KitHandles)
            {
                UnequipKit(Handle);
            }
            HeroSwaps.erase(HeroSwaps.begin() + (Swap - HeroSwaps.data()));
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Disguise
    // ------------------------------------------------------------------------------------------------------------------

    // The engine's EMovementMode values Core sets.
    constexpr uint8_t MOVE_None = 0;
    constexpr uint8_t MOVE_Walking = 1;
    constexpr uint8_t MOVE_Falling = 3;

    // A static mesh (for a prop made of any mesh) and the components and props that show one.
    class UStaticMeshType : public UObject
    {
        DefineUnrealClass(UStaticMesh, Engine)
    };

    class UStaticMeshComponentFunctions : public UObject
    {
        DefineUnrealClass(UStaticMeshComponent, Engine)
        DefineUFunction(SetStaticMesh)

    public:
        bool SetStaticMesh(UObject* NewMesh)
        {
            Reflection::FParams Params(FindStaticFunction_SetStaticMesh());
            Params.Set(L"NewMesh", NewMesh);
            return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
        }
    };

    class UActorComponentFunctions : public UObject
    {
        DefineUnrealClass(UActorComponent, Engine)
        DefineUFunction(SetIsReplicated)

    public:
        void SetIsReplicated(bool ShouldReplicate)
        {
            Reflection::FParams Params(FindStaticFunction_SetIsReplicated());
            Params.Set(L"ShouldReplicate", ShouldReplicate);
            Params.Call(this);
        }
    };

    // A world prop (a toilet, a barrel, a tree...): a building actor whose mesh (StaticMesh, replicated) any mesh can be.
    class ABuildingPropType : public AActor
    {
        DefineUnrealClass(ABuildingProp, FortniteGame)
    };

    class ABuildingSMActorMesh : public AActor
    {
        DefineUnrealClass(ABuildingSMActor, FortniteGame)

    public:
        DefineUProperty(UObject*, StaticMesh)
        DefineUProperty(UObject*, StaticMeshComponent)

        static bool HasMesh() { return Member_StaticMesh.IsAvailable() && Member_StaticMeshComponent.IsAvailable(); }
    };

    struct FDisguise
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        // The player's own pawn: hidden (with its weapon), out of the way and invulnerable meanwhile, following the
        // disguise pawn.
        TWeakObjectPtr<APawn> Original;
        // A pawn disguise: the pawn the player plays meanwhile.
        TWeakObjectPtr<APawn> Pawn;
        // A prop disguise: the prop riding on the hidden pawn.
        TWeakObjectPtr<AActor> Prop;
        bool bProp = false;
        bool bPlayerPawn = false;
        std::wstring Name;
        std::vector<TWeakObjectPtr<AActor>> HiddenWeapons;
        FVector LastLocation;
        float LastFeet = 0.0f;
        float OriginalHalfHeight = 0.0f;
        uint8_t OriginalMovementMode = MOVE_Walking;
        uint8_t OriginalCustomMovementMode = 0;
        bool bOriginalCanBeDamaged = true;
    };
    std::vector<FDisguise> Disguises;
    bool bWatchingDisguises = false;

    FDisguise* FindDisguise(const AFortPlayerController* PlayerController)
    {
        auto Found = std::find_if(Disguises.begin(), Disguises.end(), [PlayerController](const FDisguise& Disguise) { return Disguise.Controller.Get() == PlayerController; });
        return Found != Disguises.end() ? &*Found : nullptr;
    }

    void Tell(AFortPlayerController* PlayerController, const FString& Message)
    {
        UE_LOG(LogFortDisguise, Log, TEXT("%s: %s"), *GetNameSafe(PlayerController), *Message);
        if (PlayerController)
        {
            PlayerController->ClientMessage(Message);
        }
    }

    // The weapon the hidden pawn holds is an actor of its own, which hiding the pawn leaves in the air.
    void HideWeapon(FDisguise& Disguise, APawn* Pawn)
    {
        AFortPawn* FortPawn = Pawn ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
        AActor* Weapon = FortPawn ? reinterpret_cast<AActor*>(static_cast<AFortWeapon*>(FortPawn->CurrentWeapon)) : nullptr;
        if (Weapon && !Weapon->bHidden)
        {
            Weapon->SetActorHiddenInGame(true);
            Disguise.HiddenWeapons.emplace_back(Weapon);
        }
    }

    void ShowWeapons(FDisguise& Disguise)
    {
        for (const TWeakObjectPtr<AActor>& Weapon : Disguise.HiddenWeapons)
        {
            if (AActor* Hidden = Weapon.Get())
            {
                Hidden->SetActorHiddenInGame(false);
            }
        }
        Disguise.HiddenWeapons.clear();
    }

    UCharacterMovementComponent* MovementOf(APawn* Pawn)
    {
        ACharacter* Character = Pawn ? Cast<ACharacter>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
        return Character ? static_cast<UCharacterMovementComponent*>(Character->CharacterMovement) : nullptr;
    }

    // The player's own pawn while a pawn disguise is played: hidden, without collision or movement (it follows the
    // disguise instead of falling through the floor) and out of harm's way.
    void ParkOriginal(FDisguise& Disguise, APawn* Original)
    {
        Disguise.bOriginalCanBeDamaged = Original->bCanBeDamaged;
        Original->bCanBeDamaged = false;
        FeetOf(Original, &Disguise.OriginalHalfHeight);
        HideWeapon(Disguise, Original);
        Original->SetActorHiddenInGame(true);
        Original->SetActorEnableCollision(false);
        if (UCharacterMovementComponent* Movement = MovementOf(Original))
        {
            Disguise.OriginalMovementMode = Movement->MovementMode;
            Disguise.OriginalCustomMovementMode = Movement->CustomMovementMode;
            Movement->SetMovementMode(MOVE_None);
        }
        Original->ForceNetUpdate();
    }

    void UnparkOriginal(FDisguise& Disguise, APawn* Original)
    {
        Original->SetActorEnableCollision(true);
        Original->SetActorHiddenInGame(false);
        Original->bCanBeDamaged = Disguise.bOriginalCanBeDamaged;
        if (UCharacterMovementComponent* Movement = MovementOf(Original))
        {
            // A pawn that walked falls onto whatever is under it now; one flying (cheat fly) flies on.
            const bool bWalked = Disguise.OriginalMovementMode == MOVE_Walking || Disguise.OriginalMovementMode == MOVE_None;
            Movement->SetMovementMode(bWalked ? MOVE_Falling : Disguise.OriginalMovementMode, bWalked ? 0 : Disguise.OriginalCustomMovementMode);
        }
        ShowWeapons(Disguise);
        Original->ForceNetUpdate();
    }

    // Possesses the pawn the way a disguise does: AFortPlayerController::Possess then keeps the player's hero,
    // attributes and items as they are.
    void PossessForDisguise(AFortPlayerController* PlayerController, APawn* Pawn)
    {
        UFortCheatManager::bDisguisePossess = true;
        PlayerController->Possess(Pawn);
        UFortCheatManager::bDisguisePossess = false;
    }

    // Ends a prop disguise: the prop goes, the pawn shows again.
    void EndPropDisguise(FDisguise& Disguise)
    {
        if (AActor* Prop = Disguise.Prop.Get())
        {
            Prop->K2_DestroyActor();
        }
        if (APawn* Original = Disguise.Original.Get())
        {
            Original->SetActorHiddenInGame(false);
            ShowWeapons(Disguise);
            Original->ForceNetUpdate();
        }
    }

    // Ends a pawn disguise: the player's own pawn comes back where the disguise stands and is possessed again, and the
    // disguise pawn goes. Returns false when the player's own pawn is gone (they stay in the disguise).
    bool EndPawnDisguise(FDisguise& Disguise)
    {
        AFortPlayerController* PlayerController = Disguise.Controller.Get();
        APawn* Original = Disguise.Original.Get();
        APawn* DisguisePawn = Disguise.Pawn.Get();
        if (!Original || !PlayerController)
        {
            return false;
        }

        if (DisguisePawn)
        {
            Disguise.LastLocation = ActorLocation(DisguisePawn);
            Disguise.LastFeet = FeetOf(DisguisePawn);
        }
        const FVector Back(Disguise.LastLocation.X, Disguise.LastLocation.Y, Disguise.LastFeet + Disguise.OriginalHalfHeight + 2.0f);
        Original->SetActorLocation(Back, false, true);
        PossessForDisguise(PlayerController, Original);
        if (DisguisePawn)
        {
            DisguisePawn->K2_DestroyActor();
        }
        UnparkOriginal(Disguise, Original);
        return true;
    }

    // Once a tick while anyone is disguised: the hidden pawn follows its disguise and stays hidden, and a disguise that
    // died, lost its player or was replaced (a respawn) ends without leaving anything behind. Returns false when it ended.
    bool TickDisguise(FDisguise& Disguise)
    {
        AFortPlayerController* PlayerController = Disguise.Controller.Get();
        APawn* Original = Disguise.Original.Get();
        if (!PlayerController)
        {
            // The player left: the game took their pawn (the disguise) with them, and the hidden one goes too.
            if (AActor* Prop = Disguise.Prop.Get())
            {
                Prop->K2_DestroyActor();
            }
            if (APawn* DisguisePawn = Disguise.Pawn.Get(); DisguisePawn && !DisguisePawn->Controller)
            {
                DisguisePawn->K2_DestroyActor();
            }
            if (Original && !Original->Controller)
            {
                Original->K2_DestroyActor();
            }
            return false;
        }
        APawn* Current = PlayerController->GetPawn();

        if (Disguise.bProp)
        {
            if (!Original || Current != Original || IsDeadPawn(Original))
            {
                EndPropDisguise(Disguise);
                Tell(PlayerController, FString::Printf(TEXT("You are no longer disguised as %s"), Disguise.Name.c_str()));
                return false;
            }
            if (!Disguise.Prop.IsValid())
            {
                EndPropDisguise(Disguise);
                Tell(PlayerController, FString::Printf(TEXT("Your %s is gone: you can be seen again"), Disguise.Name.c_str()));
                return false;
            }
            if (!Original->bHidden)
            {
                Original->SetActorHiddenInGame(true);
            }
            HideWeapon(Disguise, Original);
            return true;
        }

        APawn* DisguisePawn = Disguise.Pawn.Get();
        const bool bDisguiseDied = DisguisePawn && IsDeadPawn(DisguisePawn);
        if (DisguisePawn && !bDisguiseDied && Current == DisguisePawn)
        {
            Disguise.LastLocation = ActorLocation(DisguisePawn);
            Disguise.LastFeet = FeetOf(DisguisePawn);
            if (Original && FVector::DistSquared(ActorLocation(Original), Disguise.LastLocation) > 1.0f)
            {
                Original->SetActorLocation(Disguise.LastLocation, false, true);
            }
            return true;
        }

        // A player pawn's death was the player's own (the game eliminated them, knocked them out for good or respawns
        // them), and a respawn possesses a new pawn: their hidden pawn isn't coming back.
        if ((bDisguiseDied && Disguise.bPlayerPawn) || (Current && Current != DisguisePawn && Current != Original))
        {
            if (Original && !Original->Controller)
            {
                Original->K2_DestroyActor();
            }
            Tell(PlayerController, FString::Printf(TEXT("Your %s died, and you with it"), Disguise.Name.c_str()));
            return false;
        }

        // The aircraft took the player from the warmup island: it destroyed their pawn (the disguise) and they jump with
        // a new one, so the hidden one goes too.
        if (!DisguisePawn && !Current && IsInAircraftPhase(PlayerController))
        {
            if (Original && !Original->Controller)
            {
                Original->K2_DestroyActor();
            }
            Tell(PlayerController, FString::Printf(TEXT("Your %s stayed on the island: you board the aircraft as yourself"), Disguise.Name.c_str()));
            return false;
        }

        // A husk (or anything else that isn't a player) dying, or going away, only ends the disguise.
        if (EndPawnDisguise(Disguise))
        {
            Tell(PlayerController, FString::Printf(TEXT("Your %s %s: you are yourself again"), Disguise.Name.c_str(), bDisguiseDied ? TEXT("died") : TEXT("is gone")));
        }
        return false;
    }

    void WatchDisguises()
    {
        for (size_t Index = 0; Index < Disguises.size();)
        {
            if (TickDisguise(Disguises[Index]))
            {
                ++Index;
            }
            else
            {
                Disguises.erase(Disguises.begin() + static_cast<ptrdiff_t>(Index));
            }
        }
        bWatchingDisguises = !Disguises.empty() && GameThread::Enqueue(&WatchDisguises);
    }

    void StartWatchingDisguises()
    {
        if (!bWatchingDisguises)
        {
            bWatchingDisguises = GameThread::Enqueue(&WatchDisguises);
        }
    }

    // Where the game keeps the classes a disguise usually is: husks and other monsters, the player pawn classes (the
    // Ninja's...), world props and containers.
    constexpr const wchar_t* DisguiseDirectories[] = {
        L"/Game/Characters/Enemies/Husk/Blueprints/",
        L"/Game/Characters/Enemies/Smasher/Blueprints/",
        L"/Game/Characters/Enemies/Taker/Blueprints/",
        L"/Game/Characters/Enemies/Flinger/Blueprints/",
        L"/Game/Characters/Enemies/Shielder/Blueprints/",
        L"/Game/Abilities/Player/Pawns/",
        L"/Game/Athena/",
        L"/Game/Building/ActorBlueprints/Prop/",
        L"/Game/Building/ActorBlueprints/Containers/",
    };

    std::wstring WithoutClassSuffix(std::wstring Name)
    {
        if (Lower(Name).ends_with(L"_c"))
        {
            Name.resize(Name.size() - 2);
        }
        return Name;
    }

    // The actor class a disguise names: SpawnActor's lookup, a class of the usual directories before a loaded class
    // that only has the name in it.
    UClass* FindDisguiseClass(const std::wstring& Query)
    {
        UClass* Found = UFortCheatManager::FindActorClass(Query);
        if (Query.front() == L'/' || (Found && Lower(WithoutClassSuffix(Found->GetName().ToStdString())) == Lower(WithoutClassSuffix(Query))))
        {
            return Found;
        }
        for (const wchar_t* Directory : DisguiseDirectories)
        {
            if (UClass* Class = UFortCheatManager::FindActorClass(Directory + WithoutClassSuffix(Query)))
            {
                return Class;
            }
        }
        return Found;
    }

    UObject* FindStaticMesh(const std::wstring& Query)
    {
        if (!UStaticMeshType::StaticClass())
        {
            return nullptr;
        }
        return Query.front() == L'/' ? LoadQuietly<UStaticMeshType>(Query) : FindObject<UStaticMeshType>(ANY_PACKAGE, Query.c_str());
    }

    AActor* SpawnDisguiseActor(APawn* Owner, UClass* Class, const FVector& Location, float Yaw, float Scale, ESpawnActorCollisionHandlingMethod Collision)
    {
        FTransform SpawnTransform;
        SpawnTransform.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion();
        SpawnTransform.Translation = Location;
        SpawnTransform.Scale3D = FVector(Scale, Scale, Scale);
        AActor* Spawned = UGameplayStatics::BeginDeferredActorSpawnFromClass(Owner, Class, SpawnTransform, Collision);
        Spawned = Spawned ? UGameplayStatics::FinishSpawningActor(Spawned, SpawnTransform) : nullptr;
        if (Spawned && Spawned->GetRootComponent() && Scale != 1.0f)
        {
            Spawned->SetActorScale3D(SpawnTransform.Scale3D);
        }
        return Spawned;
    }
}

void UFortCheatManager::HeroSwap(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    if (Words.size() != 1)
    {
        Reply(TEXT("Usage: hero <HID or CID | reset> (\"hero HID_Ninja_007_R_T01\", \"hero CID_005_Athena_Commando_M_Default\", or its path)"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortPlayerState* PlayerState = FortPlayerStateOf(PlayerController);
    if (!PlayerController || !PlayerState)
    {
        Reply(TEXT("Hero needs a player with a player state"));
        return;
    }
    ForgetDepartedPlayers();
    const bool bBattleRoyale = AFortGameModeAthena::GetCurrent(PlayerController) != nullptr;
    const std::wstring Query = Words[0];

    if (Lower(Query) == L"reset" || Lower(Query) == L"default" || Lower(Query) == L"off")
    {
        if (bBattleRoyale)
        {
            ForgetHeroSwap(PlayerController);
            AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(PlayerController));
            UFortHero* Hero = AthenaController && AthenaController->PlaysBattleRoyaleHeroes() ? AthenaController->SetupHero() : nullptr;
            PlayerState->ForceNetUpdate();
            Reply(Hero ? FString::Printf(TEXT("You wear %s again"), *GetNameSafe(Hero->GetHeroTypeBP())) : FString(TEXT("Your own hero isn't known yet; you wear it again when you respawn")));
            return;
        }
        auto Original = std::find_if(OriginalHeroes.begin(), OriginalHeroes.end(), [PlayerController](const FOriginalHero& Entry) { return Entry.Controller.Get() == PlayerController; });
        UFortHero* Hero = Original != OriginalHeroes.end() ? Original->Hero.Get() : nullptr;
        if (!Hero || !AFortPlayerController::CanTrySetHero())
        {
            Reply(TEXT("No hero to go back to (you haven't changed hero since you joined)"));
            return;
        }
        PlayerController->TrySetHeroImplementation(Hero, false, false);
        OriginalHeroes.erase(Original);
        PlayerState->ForceNetUpdate();
        Reply(FString::Printf(TEXT("You play %s again"), *GetNameSafe(Hero->GetHeroTypeBP())));
        return;
    }

    std::wstring Found;
    UFortHeroType* HeroType = FindHeroType(Query, Found);
    if (!HeroType)
    {
        Reply(FString::Printf(TEXT("No hero or outfit called '%s' (an HID_* or CID_* name, or its path: /Game/Heroes/Ninja/ItemDefinition/HID_Ninja_007_R_T01)"), Query.c_str()));
        return;
    }
    if (!AFortPlayerState::CanSetHeroType())
    {
        Reply(TEXT("This server can't set a hero (AFortPlayerState::SetHeroType wasn't found)"));
        return;
    }
    UFortHero* Hero = CreateHero(HeroType);
    if (!Hero)
    {
        Reply(FString::Printf(TEXT("Couldn't make a hero of %s"), *HeroType->GetName()));
        return;
    }
    const FString Shown = Found.empty() || Lower(Found) == Lower(HeroType->GetName().ToStdString()) ? HeroType->GetName() : FString::Printf(TEXT("%s (%s)"), Found.c_str(), *HeroType->GetName());

    if (!bBattleRoyale)
    {
        // Save the World plays the hero: TrySetHeroImplementation sets its look and its effects, abilities and perks,
        // and Core gives its gadgets (ApplyHeroAbilityKitItems). Its pawn class (a Ninja's) comes with the next pawn.
        if (!AFortPlayerController::CanTrySetHero())
        {
            Reply(TEXT("This server can't set a hero (AFortPlayerController::TrySetHeroImplementation wasn't found)"));
            return;
        }
        AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn()));
        const bool bRemembered = std::any_of(OriginalHeroes.begin(), OriginalHeroes.end(), [PlayerController](const FOriginalHero& Entry) { return Entry.Controller.Get() == PlayerController; });
        if (!bRemembered && PlayerPawn)
        {
            const TWeakObjectPtr<UFortHero>& OwnHero = PlayerPawn->Hero;
            OriginalHeroes.push_back({ TWeakObjectPtr<AFortPlayerController>(PlayerController), OwnHero });
        }
        PlayerController->TrySetHeroImplementation(Hero, false, false);
        PlayerState->ForceNetUpdate();
        Reply(FString::Printf(TEXT("You play %s, with %d perk and ability kits; its own pawn (and its animations) comes with your next respawn. \"cheat hero reset\" goes back"),
            *Shown, static_cast<int32_t>(static_cast<TArray<UFortAbilityKit*>&>(Hero->SpecializationAbilityKits).Num())));
        return;
    }

    // Battle Royale: the player keeps their Battle Royale hero (its health, shield and backpack, #73), and wears this one:
    // SetHeroType gives the player state its character parts and gender, which replicate to every client
    // (OnRep_CharacterParts puts them on the pawn). A Save the World hero's perks and abilities go on the player state's
    // ability system as the kits of its specializations and class (Mantis Leap's double jump, ...).
    ForgetHeroSwap(PlayerController);
    PlayerState->SetHeroType(Hero);
    PlayerState->ForceNetUpdate();

    FHeroSwap Swap;
    Swap.Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    Swap.HeroTypePath = HeroType->GetPathName().ToStdString();
    int32_t Equipped = 0;
    std::wstring KitNames;
    if (!IsBattleRoyaleHero(HeroType))
    {
        std::vector<UFortAbilityKit*> Kits = SpecializationKits(Hero->Specializations);
        const TStructArray<FHeroAbilityKit>& ClassKits = HeroType->AbilityKits;
        for (int32_t Index = 0; Index < ClassKits.Num(); ++Index)
        {
            const TAssetPtr<UFortAbilityKit>& InherentKit = ClassKits[Index].InherentAbilityKit;
            if (UFortAbilityKit* Kit = InherentKit.LoadSynchronous(); Kit && std::find(Kits.begin(), Kits.end(), Kit) == Kits.end())
            {
                Kits.push_back(Kit);
            }
        }
        for (UFortAbilityKit* Kit : Kits)
        {
            std::vector<uint8_t> Handle;
            if (EquipKit(PlayerState, Kit, Handle))
            {
                Swap.KitHandles.push_back(std::move(Handle));
                KitNames += (KitNames.empty() ? L"" : L", ") + Kit->GetName().ToStdString();
                ++Equipped;
            }
        }
    }
    HeroSwaps.push_back(std::move(Swap));

    UE_LOG(LogFortDisguise, Log, TEXT("HeroSwap: %s wears %s (%s), %d kits equipped: %s"), *GetNameSafe(PlayerController), *Shown, *HeroType->GetPathName(), Equipped, KitNames.c_str());
    Reply(FString::Printf(TEXT("You wear %s%s. \"cheat hero reset\" goes back"), *Shown,
        Equipped > 0 ? *FString::Printf(TEXT(", with %d of its perk and ability kits (passives and triggered abilities; its gadgets need Save the World's gadget slots)"), Equipped) : TEXT("")));
#endif
}

void UFortCheatManager::ReapplyHeroSwap(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    FHeroSwap* Swap = PlayerController ? FindHeroSwap(PlayerController) : nullptr;
    AFortPlayerState* PlayerState = Swap ? FortPlayerStateOf(PlayerController) : nullptr;
    UFortHeroType* HeroType = PlayerState ? LoadQuietly<UFortHeroType>(Swap->HeroTypePath) : nullptr;
    UFortHero* Hero = HeroType && AFortPlayerState::CanSetHeroType() ? CreateHero(HeroType) : nullptr;
    if (Hero)
    {
        PlayerState->SetHeroType(Hero);
        PlayerState->ForceNetUpdate();
        UE_LOG(LogFortDisguise, Log, TEXT("ReapplyHeroSwap: %s wears %s again"), *GetNameSafe(PlayerController), *HeroType->GetName());
    }
#endif
}

void UFortCheatManager::Disguise(const FString& Arguments)
{
#if WITH_SERVER_CODE
    constexpr float MinScale = 0.05f;
    constexpr float MaxScale = 20.0f;
    const FString Usage = TEXT("Usage: disguise <class, path or static mesh> [scale], a scale from 0.05 to 20 (\"disguise Bathroom_ToiletBowl01\", \"disguise HuskPawn\", \"disguise PlayerPawn_Ninja\"); \"undisguise\" ends it");
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    if (Words.empty() || Words.size() > 2)
    {
        Reply(Usage);
        return;
    }
    if (Lower(Words[0]) == L"off")
    {
        Undisguise(FString());
        return;
    }
    float Scale = 1.0f;
    if (Words.size() == 2)
    {
        wchar_t* End = nullptr;
        Scale = std::wcstof(Words[1].c_str(), &End);
        if (!End || *End != L'\0' || !std::isfinite(Scale) || Scale < MinScale || Scale > MaxScale)
        {
            Reply(Usage);
            return;
        }
    }

    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController || !PlayerController->GetPawn())
    {
        Reply(TEXT("Disguise needs a pawn to disguise"));
        return;
    }

    // Find what to become first, so a name nothing matches leaves a disguise as it is.
    UClass* Class = FindDisguiseClass(Words[0]);
    UObject* Mesh = Class ? nullptr : FindStaticMesh(Words[0]);
    UClass* PropBase = Mesh ? ABuildingPropType::StaticClass() : nullptr;
    if (!Class && !Mesh)
    {
        Reply(FString::Printf(TEXT("No class or static mesh called '%s' (give its path, /Game/.../X, to load one that isn't yet)"), Words[0].c_str()));
        return;
    }
    if (Mesh && (!PropBase || !ABuildingSMActorMesh::HasMesh()))
    {
        Reply(FString::Printf(TEXT("%s is a static mesh, and this build has no building prop to show it on; give a prop class instead"), *Mesh->GetName()));
        return;
    }
    if (Class && !Class->IsChildOf(AActor::StaticClass()))
    {
        Reply(FString::Printf(TEXT("%s isn't an actor class"), *Class->GetPathName()));
        return;
    }
    const bool bPawnDisguise = Class && Class->IsChildOf(APawn::StaticClass());
    if (bPawnDisguise && !AFortPlayerController::CanPossess())
    {
        Reply(TEXT("This server can't possess another pawn (AFortPlayerController::Possess wasn't found)"));
        return;
    }

    // One disguise at a time: the last one ends first.
    if (FindDisguise(PlayerController))
    {
        Undisguise(FString());
    }
    APawn* Original = PlayerController->GetPawn();
    if (!Original || IsDeadPawn(Original))
    {
        Reply(TEXT("Disguise needs a pawn that is alive"));
        return;
    }
    const FVector Location = ActorLocation(Original);
    const float Yaw = Original->GetActorRotation().Yaw;
    const std::wstring Name = Mesh ? Mesh->GetName().ToStdString() : WithoutClassSuffix(Class->GetName().ToStdString());

    FDisguise Disguise;
    Disguise.Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    Disguise.Original = TWeakObjectPtr<APawn>(Original);
    Disguise.Name = Name;
    Disguise.LastLocation = Location;

    if (bPawnDisguise)
    {
        // The pawn takes the player's place and the player plays it, with its own movement, animations and abilities: the
        // player's controller possesses it (its AI controller lets go of it in APlayerController::Possess, and goes). The
        // player's own pawn waits hidden and follows it, so "undisguise" puts the player back where the disguise stands.
        ParkOriginal(Disguise, Original);
        Disguise.LastFeet = Location.Z - Disguise.OriginalHalfHeight;
        AActor* Spawned = SpawnDisguiseActor(Original, Class, Location, Yaw, Scale, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
        APawn* DisguisePawn = Spawned ? static_cast<APawn*>(Spawned) : nullptr;
        if (!DisguisePawn)
        {
            UnparkOriginal(Disguise, Original);
            Reply(FString::Printf(TEXT("Couldn't spawn %s (an abstract class, or one that can't be spawned here)"), *Class->GetPathName()));
            return;
        }
        AController* PreviousController = DisguisePawn->Controller;
        PossessForDisguise(PlayerController, DisguisePawn);
        if (PlayerController->GetPawn() != DisguisePawn)
        {
            PossessForDisguise(PlayerController, Original);
            DisguisePawn->K2_DestroyActor();
            UnparkOriginal(Disguise, Original);
            Reply(FString::Printf(TEXT("Couldn't take over %s"), *DisguisePawn->GetName()));
            return;
        }
        if (PreviousController && reinterpret_cast<UObject*>(PreviousController) != reinterpret_cast<UObject*>(PlayerController))
        {
            PreviousController->K2_DestroyActor();
        }
        Disguise.Pawn = TWeakObjectPtr<APawn>(DisguisePawn);
        Disguise.bPlayerPawn = AFortPlayerPawn::StaticClass() && Class->IsChildOf(AFortPlayerPawn::StaticClass());
        Disguises.push_back(std::move(Disguise));
        StartWatchingDisguises();

        const bool bAI = AFortAIPawn::StaticClass() && Class->IsChildOf(AFortAIPawn::StaticClass());
        UE_LOG(LogFortDisguise, Log, TEXT("Disguise: %s plays %s (%s), its AI controller %s"), *GetNameSafe(PlayerController), *DisguisePawn->GetName(), *Class->GetPathName(), *GetNameSafe(reinterpret_cast<UObject*>(PreviousController)));
        Reply(FString::Printf(TEXT("You are %s. %s\"cheat undisguise\" to be yourself again"), Name.c_str(),
            bAI ? TEXT("You move with its animations; its attacks are its AI's, which nothing triggers now. ")
                : Disguise.bPlayerPawn ? TEXT("Dying as it is a real death. ") : TEXT("")));
        return;
    }

    // A prop: spawned at the player's feet, without collision (shots go through to the hidden player), riding on the
    // player's pawn. It replicates its attachment (bReplicateMovement), so every client sees it follow the player.
    const FVector Feet(Location.X, Location.Y, FeetOf(Original));
    AActor* Prop = SpawnDisguiseActor(Original, Mesh ? PropBase : Class, Feet, Yaw, Scale, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Prop)
    {
        Reply(FString::Printf(TEXT("Couldn't spawn %s (an abstract class, or one that can't be spawned here)"), Mesh ? *PropBase->GetPathName() : *Class->GetPathName()));
        return;
    }
    USceneComponent* Root = Prop->GetRootComponent();
    if (!Root || Root->Mobility == EComponentMobility::Static)
    {
        Prop->K2_DestroyActor();
        Reply(FString::Printf(TEXT("%s can't move (%s), so it can't follow you; pick a prop that can (a building prop, \"disguise Bathroom_ToiletBowl01\")"), Name.c_str(), Root ? TEXT("its root is static") : TEXT("it has no root")));
        return;
    }
    if (Mesh)
    {
        // The building's StaticMesh replicates, and so does its mesh component's, once it replicates.
        ABuildingSMActorMesh* Building = Cast<ABuildingSMActorMesh>(reinterpret_cast<UObject*>(Prop));
        UObject* Component = Building ? static_cast<UObject*>(Building->StaticMeshComponent) : nullptr;
        if (Building)
        {
            Building->StaticMesh = Mesh;
        }
        if (Component && UStaticMeshComponentFunctions::StaticClass() && Component->IsA(UStaticMeshComponentFunctions::StaticClass()))
        {
            static_cast<UStaticMeshComponentFunctions*>(Component)->SetStaticMesh(Mesh);
            static_cast<UActorComponentFunctions*>(Component)->SetIsReplicated(true);
        }
    }
    Prop->SetActorEnableCollision(false);
    if (!Prop->bReplicates)
    {
        Prop->SetReplicates(true);
    }
    Prop->bReplicateMovement = true;
    Prop->AttachToActor(Original, EAttachmentRule::KeepWorld, EAttachmentRule::KeepWorld, EAttachmentRule::KeepWorld);
    Prop->ForceNetUpdate();

    Original->SetActorHiddenInGame(true);
    HideWeapon(Disguise, Original);
    Original->ForceNetUpdate();
    Disguise.Prop = TWeakObjectPtr<AActor>(Prop);
    Disguise.bProp = true;
    Disguises.push_back(std::move(Disguise));
    StartWatchingDisguises();

    UE_LOG(LogFortDisguise, Log, TEXT("Disguise: %s is the prop %s (%s), attached to %s"), *GetNameSafe(PlayerController), *Prop->GetName(), Mesh ? *Mesh->GetPathName() : *Class->GetPathName(), *Original->GetName());
    Reply(FString::Printf(TEXT("You are a %s: you are hidden and it follows you. \"cheat undisguise\" to be seen again"), Name.c_str()));
#endif
}

void UFortCheatManager::Undisguise(const FString& Arguments)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetPlayerController();
    FDisguise* Disguise = PlayerController ? FindDisguise(PlayerController) : nullptr;
    if (!Disguise)
    {
        Reply(TEXT("You aren't disguised"));
        return;
    }

    const std::wstring Name = Disguise->Name;
    bool bEnded = true;
    if (Disguise->bProp)
    {
        EndPropDisguise(*Disguise);
    }
    else
    {
        bEnded = EndPawnDisguise(*Disguise);
    }
    Disguises.erase(Disguises.begin() + (Disguise - Disguises.data()));
    Reply(bEnded ? FString::Printf(TEXT("You are no longer disguised as %s"), Name.c_str())
                 : FString::Printf(TEXT("Your own pawn is gone, so you stay %s"), Name.c_str()));
#endif
}
