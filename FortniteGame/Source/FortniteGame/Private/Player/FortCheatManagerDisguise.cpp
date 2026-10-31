#include "pch.h"
#include "Player/FortCheatManager.h"
#include "Abilities/FortAbilityKit.h"
#include "Abilities/FortAbilitySet.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Components/SceneComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FortKismetLibrary.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameModeBase.h"
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
#include <memory>
#include <optional>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortDisguise, Log, All);

// "cheat skin" ("cheat hero") and "cheat disguise" / "cheat undisguise" (#134, #136). Everything here goes through the
// game's and the engine's own functions (UFunctions through reflection, and Core's existing SetHeroType, Possess and
// ApplyHeroAbilityKitItems); nothing is hooked.

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
            for (int32_t Index = 0; Index < TypeSpecializations.Num(); ++Index)
            {
                if (UFortHeroSpecialization* Loaded = SoftElementAt(TypeSpecializations, Index).LoadSynchronous())
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

    // Every kit a hero brings (its specializations' perks and abilities, and its class's own): what "cheat disguise" gives
    // a player pawn's disguise.
    std::vector<UFortAbilityKit*> HeroKits(UFortHeroType* HeroType, UFortHero* Hero)
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
        return Kits;
    }

    // The skin a player chose, worn again after each respawn.
    struct FSkin
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        std::wstring HeroTypePath;
    };
    std::vector<FSkin> Skins;

    FSkin* FindSkin(const AFortPlayerController* PlayerController)
    {
        std::erase_if(Skins, [](const FSkin& Skin) { return !Skin.Controller.IsValid(); });
        auto Found = std::find_if(Skins.begin(), Skins.end(), [PlayerController](const FSkin& Skin) { return Skin.Controller.Get() == PlayerController; });
        return Found != Skins.end() ? &*Found : nullptr;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Disguise
    // ------------------------------------------------------------------------------------------------------------------

    // The engine's EMovementMode values Core sets.
    constexpr uint8_t MOVE_None = 0;
    constexpr uint8_t MOVE_Walking = 1;
    constexpr uint8_t MOVE_Falling = 3;

    // A static mesh (for a prop made of any mesh) and the components that show one.
    class UStaticMeshType : public UObject
    {
        DefineUnrealClass(UStaticMesh, Engine)
    };

    class UStaticMeshComponentFunctions : public UObject
    {
        DefineUnrealClass(UStaticMeshComponent, Engine)
        DefineUFunction(SetStaticMesh)

    public:
        DefineUProperty(UObject*, StaticMesh)

        bool SetStaticMesh(UObject* NewMesh)
        {
            Reflection::FParams Params(FindStaticFunction_SetStaticMesh());
            Params.Set(L"NewMesh", NewMesh);
            return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
        }
    };

    class USceneComponentFunctions : public UObject
    {
        DefineUnrealClass(USceneComponent, Engine)
        DefineUFunction(SetRelativeScale3D)
        DefineUFunction(K2_SetRelativeLocationAndRotation)

    public:
        void SetRelativeScale3D(const FVector& NewScale3D)
        {
            Reflection::FParams Params(FindStaticFunction_SetRelativeScale3D());
            Params.Set(L"NewScale3D", NewScale3D);
            Params.Call(this);
        }

        // Teleports, without a sweep: where the component sits on its parent.
        bool SetRelativeLocationAndRotation(const FVector& NewLocation, const FRotator& NewRotation)
        {
            UFunction* Function = FindStaticFunction_K2_SetRelativeLocationAndRotation();
            if (!Function)
            {
                return false;
            }
            Reflection::FParams Params(Function, true);
            Params.Set(L"NewLocation", NewLocation);
            Params.Set(L"NewRotation", NewRotation);
            Params.Set(L"bSweep", false);
            Params.Set(L"bTeleport", true);
            return Params.Call(this);
        }
    };

    class UActorComponentFunctions : public UObject
    {
        DefineUnrealClass(UActorComponent, Engine)
        DefineUFunction(SetIsReplicated)
        DefineUFunction(SetComponentTickEnabled)

    public:
        void SetIsReplicated(bool ShouldReplicate)
        {
            Reflection::FParams Params(FindStaticFunction_SetIsReplicated());
            Params.Set(L"ShouldReplicate", ShouldReplicate);
            Params.Call(this);
        }

        bool SetComponentTickEnabled(bool bEnabled)
        {
            UFunction* Function = FindStaticFunction_SetComponentTickEnabled();
            if (!Function)
            {
                return false;
            }
            Reflection::FParams Params(Function);
            Params.Set(L"bEnabled", bEnabled);
            return Params.Call(this);
        }
    };

    // UPrimitiveComponent::SetOwnerNoSee: the default pawn's mesh, which carries a prop, is hidden from its owner's view
    // (bOwnerNoSee, ADefaultPawn's constructor), and the carrier's owner is the player's pawn (#154).
    class UPrimitiveComponentFunctions : public UObject
    {
        DefineUnrealClass(UPrimitiveComponent, Engine)
        DefineUFunction(SetOwnerNoSee)

    public:
        bool SetOwnerNoSee(bool bNewOwnerNoSee)
        {
            UFunction* Function = FindStaticFunction_SetOwnerNoSee();
            if (!Function)
            {
                return false;
            }
            Reflection::FParams Params(Function);
            Params.Set(L"bNewOwnerNoSee", bNewOwnerNoSee);
            return Params.Call(this);
        }
    };

    // A character's capsule: its half height puts a puppet's feet where the player's are.
    class UCapsuleComponentFunctions : public UObject
    {
        DefineUnrealClass(UCapsuleComponent, Engine)
        DefineUFunction(GetScaledCapsuleHalfHeight)

    public:
        float GetScaledCapsuleHalfHeight()
        {
            Reflection::FParams Params(FindStaticFunction_GetScaledCapsuleHalfHeight());
            return Params.Call(this) ? Params.Take<float>(L"ReturnValue") : 0.0f;
        }
    };

    class ACharacterCapsule : public UObject
    {
        DefineUnrealClass(ACharacter, Engine)

    public:
        DefineUProperty(UObject*, CapsuleComponent)

        static bool HasCapsuleComponent() { return Member_CapsuleComponent.IsAvailable(); }
    };

    class AControllerFunctions : public UObject
    {
        DefineUnrealClass(AController, Engine)
        DefineUFunction(GetControlRotation)
        DefineUFunction(SetControlRotation)
        DefineUFunction(ClientSetRotation)

    public:
        FRotator GetControlRotation()
        {
            Reflection::FParams Params(FindStaticFunction_GetControlRotation());
            return Params.Call(this) ? Params.Take<FRotator>(L"ReturnValue") : FRotator();
        }

        // The server's control rotation, and (a client function) the client's: the way its camera looks.
        bool SetViewRotation(const FRotator& Rotation)
        {
            UFunction* SetControl = FindStaticFunction_SetControlRotation();
            UFunction* ClientSet = FindStaticFunction_ClientSetRotation();
            if (!SetControl || !ClientSet)
            {
                return false;
            }
            Reflection::FParams ControlParams(SetControl);
            ControlParams.Set(L"NewRotation", Rotation);
            ControlParams.Call(this);
            Reflection::FParams ClientParams(ClientSet, true);
            ClientParams.Set(L"NewRotation", Rotation);
            ClientParams.Set(L"bResetCamera", false);
            return ClientParams.Call(this);
        }
    };

    // The half height of the actor's capsule (a character's), else of its colliding bounds.
    float HalfHeightOf(const AActor* Actor)
    {
        UObject* AsObject = reinterpret_cast<UObject*>(const_cast<AActor*>(Actor));
        if (ACharacterCapsule::StaticClass() && ACharacterCapsule::HasCapsuleComponent() && AsObject->IsA(ACharacterCapsule::StaticClass()))
        {
            if (UObject* Capsule = static_cast<ACharacterCapsule*>(AsObject)->CapsuleComponent; Capsule && UCapsuleComponentFunctions::FindStaticFunction_GetScaledCapsuleHalfHeight())
            {
                return static_cast<UCapsuleComponentFunctions*>(Capsule)->GetScaledCapsuleHalfHeight();
            }
        }
        float HalfHeight = 0.0f;
        FeetOf(Actor, &HalfHeight);
        if (HalfHeight <= 0.0f)
        {
            // Nothing of it collides (a client turned its collision off): all of it.
            FVector Origin;
            FVector Extent;
            Actor->GetActorBounds(false, Origin, Extent);
            HalfHeight = Extent.Z;
        }
        return HalfHeight;
    }

    // The way the player looks (their camera's yaw), else the way their pawn faces.
    float FacingYaw(AController* Controller, const AActor* Pawn)
    {
        if (Controller && AControllerFunctions::FindStaticFunction_GetControlRotation())
        {
            return reinterpret_cast<AControllerFunctions*>(Controller)->GetControlRotation().Yaw;
        }
        return Pawn->GetActorRotation().Yaw;
    }

    // A building (a world prop: a toilet, a barrel, a tree...): its mesh is its StaticMesh. Its StaticMeshComponent is
    // static (ABuildingSMActor's constructor, 1.7.2 0x45A620, sets the component's Mobility to Static), so it can't be
    // attached to anything that moves: a client refuses (USceneComponent::AttachToComponent, 1.7.2 0x2058350,
    // "NoStaticToDynamicWarning"), and SetStaticMesh refuses a static component after play has begun.
    class ABuildingSMActorMesh : public AActor
    {
        DefineUnrealClass(ABuildingSMActor, FortniteGame)

    public:
        DefineUProperty(UObject*, StaticMesh)
    };

    // The engine's default pawn: a movable sphere root and a movable mesh component, replicated. It carries a prop's mesh
    // on the player's back: its mesh component replicates the mesh (StaticMesh) and its scale, its root the attachment.
    class ADefaultPawnType : public APawn
    {
        DefineUnrealClass(ADefaultPawn, Engine)

    public:
        DefineUProperty(UObject*, MeshComponent)

        static bool HasMeshComponent() { return Member_MeshComponent.IsAvailable(); }
    };

    enum class EDisguiseKind : uint8_t
    {
        // A prop riding on the hidden pawn: the actor itself, or a carrier with its mesh.
        Prop,
        // A pawn (a husk) that walks where the hidden pawn walks, with its own animations; the player keeps their pawn.
        Puppet,
        // A player pawn class the player possesses, with its hero's abilities; their own pawn waits hidden.
        Possessed,
    };

    struct FDisguise
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        EDisguiseKind Kind = EDisguiseKind::Prop;
        // The player's own pawn: hidden (with its weapon) meanwhile. While possessing, also out of the way, invulnerable
        // and following the disguise.
        TWeakObjectPtr<APawn> Original;
        // The prop, the carrier, the puppet or the possessed pawn.
        TWeakObjectPtr<AActor> Actor;
        std::wstring Name;
        std::vector<TWeakObjectPtr<AActor>> HiddenWeapons;
        // Possessed: where the disguise last stood, and the player's own pawn as it was.
        FVector LastLocation;
        float LastFeet = 0.0f;
        float OriginalHalfHeight = 0.0f;
        uint8_t OriginalMovementMode = MOVE_Walking;
        uint8_t OriginalCustomMovementMode = 0;
        bool bOriginalCanBeDamaged = true;
        // Possessed: the player's own pawn's role on their client (theirs: autonomous), simulated while it is parked.
        ENetRole OriginalRemoteRole = ROLE_AutonomousProxy;
        // Possessed: the hero kits equipped on the player state, and in Save the World the player's own hero, whose
        // gadgets come back at the end.
        std::vector<std::vector<uint8_t>> KitHandles;
        bool bGaveGadgets = false;
        TWeakObjectPtr<UFortHero> OwnHero;
        // Puppet: its half height (its capsule centre over the player's feet) and the movement mode it shows.
        float PuppetHalfHeight = 0.0f;
        uint8_t PuppetMovementMode = MOVE_Walking;
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
        AActor* Weapon = FortPawn ? static_cast<AActor*>(static_cast<AFortWeapon*>(FortPawn->CurrentWeapon)) : nullptr;
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

    // The player's own pawn while they possess a disguise: hidden, without collision or movement (it follows the
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

    // A player who possessed a disguise (or their own pawn again), and the way they looked before (#154).
    struct FKeptView
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        TWeakObjectPtr<APawn> Pawn;
        FRotator Rotation;
        ULONGLONG GiveUpAt = 0;
    };
    std::vector<FKeptView> KeptViews;
    bool bWatchingKeptViews = false;
    constexpr ULONGLONG KeepViewMs = 5000;

    // Once the client has taken the pawn (its ServerAcknowledgePossession, from ClientRestart, set AcknowledgedPawn), it
    // looks the way it did again: whatever its restart did to its rotation is behind it by then.
    void WatchKeptViews()
    {
        const ULONGLONG Now = GetTickCount64();
        std::erase_if(KeptViews, [Now](const FKeptView& Kept)
        {
            AFortPlayerController* PlayerController = Kept.Controller.Get();
            APawn* Pawn = Kept.Pawn.Get();
            if (!PlayerController || !Pawn || PlayerController->GetPawn() != Pawn)
            {
                return true;
            }
            if (static_cast<APawn*>(PlayerController->AcknowledgedPawn) != Pawn)
            {
                return Now >= Kept.GiveUpAt;
            }
            reinterpret_cast<AControllerFunctions*>(PlayerController)->SetViewRotation(Kept.Rotation);
            return true;
        });
        bWatchingKeptViews = !KeptViews.empty() && GameThread::Enqueue(&WatchKeptViews);
    }

    // Possesses the pawn the way a disguise does: AFortPlayerController::Possess then keeps the player's hero,
    // attributes and items as they are. The camera keeps looking where it looked (#154): APlayerController::Possess
    // turns the control rotation to the pawn's facing (pitch level) and the client's restart of a player pawn turned its
    // camera too, so the rotation goes back to the client now and again once it has taken the pawn.
    void PossessForDisguise(AFortPlayerController* PlayerController, APawn* Pawn)
    {
        const bool bCanKeepView = AControllerFunctions::FindStaticFunction_GetControlRotation() && AControllerFunctions::FindStaticFunction_SetControlRotation() && AControllerFunctions::FindStaticFunction_ClientSetRotation();
        const FRotator View = bCanKeepView ? reinterpret_cast<AControllerFunctions*>(PlayerController)->GetControlRotation() : FRotator();
        UFortCheatManager::bDisguisePossess = true;
        PlayerController->Possess(Pawn);
        UFortCheatManager::bDisguisePossess = false;
        if (!bCanKeepView || PlayerController->GetPawn() != Pawn)
        {
            return;
        }
        reinterpret_cast<AControllerFunctions*>(PlayerController)->SetViewRotation(View);
        std::erase_if(KeptViews, [PlayerController](const FKeptView& Kept) { return Kept.Controller.Get() == PlayerController; });
        FKeptView& Kept = KeptViews.emplace_back();
        Kept.Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
        Kept.Pawn = TWeakObjectPtr<APawn>(Pawn);
        Kept.Rotation = View;
        Kept.GiveUpAt = GetTickCount64() + KeepViewMs;
        if (!bWatchingKeptViews)
        {
            bWatchingKeptViews = GameThread::Enqueue(&WatchKeptViews);
        }
    }

    // Takes a possessed disguise's hero kits off, and in Save the World gives the player's own hero's gadgets back.
    void TakeHeroKitsOff(FDisguise& Disguise)
    {
        for (std::vector<uint8_t>& Handle : Disguise.KitHandles)
        {
            UnequipKit(Handle);
        }
        Disguise.KitHandles.clear();
        AFortPlayerController* PlayerController = Disguise.Controller.Get();
        UFortHero* OwnHero = Disguise.OwnHero.Get();
        if (PlayerController && Disguise.bGaveGadgets && OwnHero)
        {
            PlayerController->ApplyHeroAbilityKitItems(OwnHero);
        }
        Disguise.bGaveGadgets = false;
    }

    // Ends a prop or puppet disguise: the actor goes, the player's pawn shows again.
    void EndShownDisguise(FDisguise& Disguise)
    {
        if (AActor* Actor = Disguise.Actor.Get())
        {
            Actor->K2_DestroyActor();
        }
        if (APawn* Original = Disguise.Original.Get())
        {
            Original->SetActorHiddenInGame(false);
            ShowWeapons(Disguise);
            Original->ForceNetUpdate();
        }
    }

    // Ends a possessed disguise: the player's own pawn comes back where the disguise stands and is possessed again, the
    // disguise pawn and its hero's kits go. Returns false when the player's own pawn is gone (they stay in the disguise).
    bool EndPossessedDisguise(FDisguise& Disguise)
    {
        TakeHeroKitsOff(Disguise);
        AFortPlayerController* PlayerController = Disguise.Controller.Get();
        APawn* Original = Disguise.Original.Get();
        AActor* DisguisePawn = Disguise.Actor.Get();
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
        Original->RemoteRole = TEnumAsByte<ENetRole>(Disguise.OriginalRemoteRole);
        PossessForDisguise(PlayerController, Original);
        if (DisguisePawn)
        {
            DisguisePawn->K2_DestroyActor();
        }
        UnparkOriginal(Disguise, Original);
        return true;
    }

    // A puppet rides on the player's hidden pawn (#154): attached to it, its feet on the pawn's, facing the way the pawn
    // faces (the camera's way: the pawn turns to it). It used to be placed where the pawn stood every server tick and
    // sent to the other clients as a simulated character, which those clients moved on their own between the updates
    // with no collision (every client takes a disguise's off), so it went in steps and, falling after a jump, fell
    // through the floor until the next update. Attached, the server sends its attachment instead of its movement, so
    // every client carries it on the pawn, which they move smoothly with its own collision; their TickDisguiseView keeps
    // it there and gives it the pawn's velocity and movement mode, which its animation Blueprint plays.
    void MovePuppet(FDisguise& Disguise, APawn* Original, AActor* Puppet)
    {
        USceneComponent* Root = Puppet->GetRootComponent();
        if (!Root)
        {
            return;
        }
        if (static_cast<USceneComponent*>(Root->AttachParent) != Original->GetRootComponent())
        {
            Puppet->AttachToActor(Original, EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, EAttachmentRule::KeepWorld);
        }
        // The pawn's half height changes when it crouches.
        const FVector Relative(0.0f, 0.0f, Disguise.PuppetHalfHeight - HalfHeightOf(Original));
        if (FVector::DistSquared(static_cast<FVector>(Root->RelativeLocation), Relative) > 0.25f)
        {
            reinterpret_cast<USceneComponentFunctions*>(Root)->SetRelativeLocationAndRotation(Relative, FRotator());
        }

        UCharacterMovementComponent* PuppetMovement = MovementOf(static_cast<APawn*>(Puppet));
        if (!PuppetMovement)
        {
            return;
        }
        PuppetMovement->Velocity = Original->GetVelocity();
        UCharacterMovementComponent* OwnMovement = MovementOf(Original);
        const uint8_t Mode = OwnMovement && static_cast<uint8_t>(OwnMovement->MovementMode) == MOVE_Falling ? MOVE_Falling : MOVE_Walking;
        if (Mode != Disguise.PuppetMovementMode)
        {
            PuppetMovement->SetMovementMode(Mode);
            Disguise.PuppetMovementMode = Mode;
        }
    }

    // Once a tick while anyone is disguised: the hidden pawn stays hidden, a puppet follows it, a possessed disguise is
    // followed by it, and a disguise that died, lost its player or was replaced (a respawn) ends without leaving anything
    // behind. Returns false when it ended.
    bool TickDisguise(FDisguise& Disguise)
    {
        AFortPlayerController* PlayerController = Disguise.Controller.Get();
        APawn* Original = Disguise.Original.Get();
        AActor* Actor = Disguise.Actor.Get();
        if (!PlayerController)
        {
            // The player left: the game took their pawn with them, and what Core spawned goes too.
            TakeHeroKitsOff(Disguise);
            if (Actor && (Disguise.Kind != EDisguiseKind::Possessed || !static_cast<APawn*>(Actor)->Controller))
            {
                Actor->K2_DestroyActor();
            }
            if (Disguise.Kind == EDisguiseKind::Possessed && Original && !Original->Controller)
            {
                Original->K2_DestroyActor();
            }
            return false;
        }
        APawn* Current = PlayerController->GetPawn();

        if (Disguise.Kind != EDisguiseKind::Possessed)
        {
            if (!Original || Current != Original || IsDeadPawn(Original))
            {
                EndShownDisguise(Disguise);
                Tell(PlayerController, FString::Printf(TEXT("You are no longer disguised as %s"), Disguise.Name.c_str()));
                return false;
            }
            if (!Actor || (Disguise.Kind == EDisguiseKind::Puppet && IsDeadPawn(static_cast<APawn*>(Actor))))
            {
                EndShownDisguise(Disguise);
                Tell(PlayerController, FString::Printf(TEXT("Your %s is gone: you can be seen again"), Disguise.Name.c_str()));
                return false;
            }
            if (!Original->bHidden)
            {
                Original->SetActorHiddenInGame(true);
            }
            HideWeapon(Disguise, Original);
            if (Disguise.Kind == EDisguiseKind::Puppet)
            {
                MovePuppet(Disguise, Original, Actor);
            }
            return true;
        }

        APawn* DisguisePawn = static_cast<APawn*>(Actor);
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

        // A player pawn's death is the player's own (the game eliminates them, or downs and respawns them), and a respawn
        // possesses a new pawn: their hidden pawn isn't coming back.
        if (bDisguiseDied || (Current && Current != DisguisePawn && Current != Original))
        {
            TakeHeroKitsOff(Disguise);
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
            TakeHeroKitsOff(Disguise);
            if (Original && !Original->Controller)
            {
                Original->K2_DestroyActor();
            }
            Tell(PlayerController, FString::Printf(TEXT("Your %s stayed on the island: you board the aircraft as yourself"), Disguise.Name.c_str()));
            return false;
        }

        if (EndPossessedDisguise(Disguise))
        {
            Tell(PlayerController, FString::Printf(TEXT("Your %s is gone: you are yourself again"), Disguise.Name.c_str()));
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

    // The mesh an actor class shows, from its class default: a building's StaticMesh, else its root's when that is a
    // static mesh component.
    UObject* MeshOfClass(UClass* Class)
    {
        AActor* Default = static_cast<AActor*>(Class->GetDefaultObject());
        if (!Default)
        {
            return nullptr;
        }
        if (ABuildingSMActorMesh::StaticClass() && reinterpret_cast<UObject*>(Default)->IsA(ABuildingSMActorMesh::StaticClass()))
        {
            if (UObject* Mesh = static_cast<ABuildingSMActorMesh*>(Default)->StaticMesh)
            {
                return Mesh;
            }
        }
        UObject* Root = reinterpret_cast<UObject*>(Default->GetRootComponent());
        if (Root && UStaticMeshComponentFunctions::StaticClass() && Root->IsA(UStaticMeshComponentFunctions::StaticClass()))
        {
            return static_cast<UStaticMeshComponentFunctions*>(Root)->StaticMesh;
        }
        return nullptr;
    }

    // The hero whose abilities a player pawn class's disguise brings: the one the player named, else the class's usual
    // one (PlayerPawn_Ninja: a Ninja, ...).
    UFortHeroType* HeroForPawnClass(UClass* Class, const std::wstring& Named, std::wstring& OutFound)
    {
        if (!Named.empty())
        {
            return FindHeroType(Named, OutFound);
        }
        const std::wstring ClassName = Lower(Class->GetName().ToStdString());
        constexpr std::pair<const wchar_t*, const wchar_t*> ClassHeroes[] = {
            { L"ninja", L"HID_Ninja_SlashTail_UC_T01" },
            { L"constructor", L"HID_Constructor_HammerTank_UC_T01" },
            { L"outlander", L"HID_Outlander_PunchPhase_UC_T01" },
            { L"commando", L"HID_Commando_GrenadeGun_UC_T01" },
            { L"soldier", L"HID_Commando_GrenadeGun_UC_T01" },
        };
        for (const auto& [Token, Hero] : ClassHeroes)
        {
            if (ClassName.find(Token) != std::wstring::npos)
            {
                return FindHeroType(Hero, OutFound);
            }
        }
        return nullptr;
    }

    class AActorFunctions : public UObject
    {
        DefineUnrealClass(AActor, Engine)
        DefineUFunction(SetOwner)

    public:
        void SetOwner(AActor* NewOwner)
        {
            Reflection::FParams Params(FindStaticFunction_SetOwner());
            Params.Set(L"NewOwner", NewOwner);
            Params.Call(this);
        }
    };

    // The spawn gives the owner already; AActor::SetOwner when it didn't.
    void OwnByPawn(AActor* Actor, APawn* Original)
    {
        if (Actor->GetOwner() != Original && AActorFunctions::FindStaticFunction_SetOwner())
        {
            reinterpret_cast<AActorFunctions*>(Actor)->SetOwner(Original);
        }
    }

    // A puppet or a prop is owned by the player's hidden pawn (bOwnedByPawn): Owner replicates, so every client can tell
    // a disguise and whose it is (ScanDisguises, below), and the owner's connection is the player's.
    AActor* SpawnDisguiseActor(APawn* Original, UClass* Class, const FVector& Location, float Yaw, float Scale, ESpawnActorCollisionHandlingMethod Collision, bool bWithoutAI, bool bOwnedByPawn = false)
    {
        FTransform SpawnTransform;
        SpawnTransform.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion();
        SpawnTransform.Translation = Location;
        SpawnTransform.Scale3D = FVector(Scale, Scale, Scale);
        AActor* Spawned = UGameplayStatics::BeginDeferredActorSpawnFromClass(Original, Class, SpawnTransform, Collision, bOwnedByPawn ? static_cast<AActor*>(Original) : nullptr);
        // A puppet gets no AI controller: FinishSpawning gives a pawn one when AutoPossessAI says so.
        if (Spawned && bWithoutAI && reinterpret_cast<UObject*>(Spawned)->IsA(APawn::StaticClass()))
        {
            static_cast<APawn*>(Spawned)->AutoPossessAI = 0;
        }
        Spawned = Spawned ? UGameplayStatics::FinishSpawningActor(Spawned, SpawnTransform) : nullptr;
        if (Spawned && Spawned->GetRootComponent() && Scale != 1.0f)
        {
            Spawned->SetActorScale3D(SpawnTransform.Scale3D);
        }
        return Spawned;
    }

    // A carrier for a mesh: the engine's default pawn, without collision, its mesh component showing the mesh at the
    // scale and replicating both.
    AActor* SpawnMeshCarrier(APawn* Owner, UObject* Mesh, const FVector& Location, float Yaw, float Scale)
    {
        UClass* CarrierClass = ADefaultPawnType::StaticClass();
        if (!CarrierClass || !ADefaultPawnType::HasMeshComponent() || !UStaticMeshComponentFunctions::FindStaticFunction_SetStaticMesh())
        {
            return nullptr;
        }
        AActor* Carrier = SpawnDisguiseActor(Owner, CarrierClass, Location, Yaw, 1.0f, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, true, true);
        UObject* MeshComponent = Carrier ? static_cast<UObject*>(static_cast<ADefaultPawnType*>(Carrier)->MeshComponent) : nullptr;
        if (!MeshComponent || !static_cast<UStaticMeshComponentFunctions*>(MeshComponent)->SetStaticMesh(Mesh))
        {
            if (Carrier)
            {
                Carrier->K2_DestroyActor();
            }
            return nullptr;
        }
        // The default pawn shows its sphere at 35/160 of its size.
        static_cast<USceneComponentFunctions*>(MeshComponent)->SetRelativeScale3D(FVector(Scale, Scale, Scale));
        static_cast<UActorComponentFunctions*>(MeshComponent)->SetIsReplicated(true);
        return Carrier;
    }
}

void UFortCheatManager::Skin(const FString& Arguments)
{
#if WITH_SERVER_CODE
    const std::vector<std::wstring> Words = SplitWords(Arguments.ToStdString());
    if (Words.size() != 1)
    {
        Reply(TEXT("Usage: skin <HID or CID | reset> (\"skin HID_Ninja_007_R_T01\", \"skin CID_005_Athena_Commando_M_Default\", or its path)"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    AFortPlayerState* PlayerState = FortPlayerStateOf(PlayerController);
    if (!PlayerController || !PlayerState)
    {
        Reply(TEXT("Skin needs a player with a player state"));
        return;
    }
    if (!AFortPlayerState::CanSetHeroType())
    {
        Reply(TEXT("This server can't set a look (AFortPlayerState::SetHeroType wasn't found)"));
        return;
    }
    const std::wstring Query = Words[0];

    if (Lower(Query) == L"reset" || Lower(Query) == L"default" || Lower(Query) == L"off")
    {
        // The player's own look: Battle Royale's hero (SetupHero sets it again), else the hero their pawn plays.
        std::erase_if(Skins, [PlayerController](const FSkin& Skin) { return Skin.Controller.Get() == PlayerController; });
        AFortPlayerControllerAthena* AthenaController = AFortGameModeAthena::GetCurrent(PlayerController) ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(PlayerController)) : nullptr;
        UFortHero* Hero = nullptr;
        if (AthenaController && AthenaController->PlaysBattleRoyaleHeroes())
        {
            Hero = AthenaController->SetupHero();
        }
        else if (AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())))
        {
            const TWeakObjectPtr<UFortHero>& OwnHero = PlayerPawn->Hero;
            Hero = OwnHero.Get();
            if (Hero)
            {
                PlayerState->SetHeroType(Hero);
            }
        }
        PlayerState->ForceNetUpdate();
        Reply(Hero ? FString::Printf(TEXT("You wear %s again"), *GetNameSafe(Hero->GetHeroTypeBP())) : FString(TEXT("Your own look comes back when you respawn")));
        return;
    }

    std::wstring Found;
    UFortHeroType* HeroType = FindHeroType(Query, Found);
    if (!HeroType)
    {
        Reply(FString::Printf(TEXT("No hero or outfit called '%s' (an HID_* or CID_* name, or its path: /Game/Heroes/Ninja/ItemDefinition/HID_Ninja_007_R_T01)"), Query.c_str()));
        return;
    }
    UFortHero* Hero = CreateHero(HeroType);
    if (!Hero)
    {
        Reply(FString::Printf(TEXT("Couldn't make a hero of %s"), *HeroType->GetName()));
        return;
    }

    // Only the look (#136): SetHeroType gives the player state the hero's character parts and gender, which replicate to
    // every client (OnRep_CharacterParts puts them on the pawn). The player keeps playing their own hero: its health,
    // shield, perks and abilities.
    PlayerState->SetHeroType(Hero);
    PlayerState->ForceNetUpdate();
    FSkin* Skin = FindSkin(PlayerController);
    if (!Skin)
    {
        Skin = &Skins.emplace_back();
        Skin->Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    }
    Skin->HeroTypePath = HeroType->GetPathName().ToStdString();

    const FString Shown = Found.empty() || Lower(Found) == Lower(HeroType->GetName().ToStdString()) ? HeroType->GetName() : FString::Printf(TEXT("%s (%s)"), Found.c_str(), *HeroType->GetName());
    UE_LOG(LogFortDisguise, Log, TEXT("Skin: %s wears %s (%s)"), *GetNameSafe(PlayerController), *Shown, *HeroType->GetPathName());
    Reply(FString::Printf(TEXT("You wear %s. \"cheat skin reset\" goes back"), *Shown));
#endif
}

void UFortCheatManager::ReapplySkin(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    FSkin* Skin = PlayerController ? FindSkin(PlayerController) : nullptr;
    AFortPlayerState* PlayerState = Skin ? FortPlayerStateOf(PlayerController) : nullptr;
    UFortHeroType* HeroType = PlayerState ? LoadQuietly<UFortHeroType>(Skin->HeroTypePath) : nullptr;
    UFortHero* Hero = HeroType && AFortPlayerState::CanSetHeroType() ? CreateHero(HeroType) : nullptr;
    if (Hero)
    {
        PlayerState->SetHeroType(Hero);
        PlayerState->ForceNetUpdate();
        UE_LOG(LogFortDisguise, Log, TEXT("ReapplySkin: %s wears %s again"), *GetNameSafe(PlayerController), *HeroType->GetName());
    }
#endif
}

void UFortCheatManager::Disguise(const FString& Arguments)
{
#if WITH_SERVER_CODE
    constexpr float MinScale = 0.05f;
    constexpr float MaxScale = 20.0f;
    const FString Usage = TEXT("Usage: disguise <class, path or static mesh> [scale], a scale from 0.05 to 20 (\"disguise Bathroom_ToiletBowl01\", \"disguise HuskPawn\", \"disguise PlayerPawn_Ninja [HID]\"); \"undisguise\" ends it");
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
    // The second word is a scale, or (a player pawn class) the hero whose abilities it brings.
    float Scale = 1.0f;
    std::wstring NamedHero;
    if (Words.size() == 2)
    {
        wchar_t* End = nullptr;
        const float Number = std::wcstof(Words[1].c_str(), &End);
        if (End && *End == L'\0')
        {
            if (!std::isfinite(Number) || Number < MinScale || Number > MaxScale)
            {
                Reply(Usage);
                return;
            }
            Scale = Number;
        }
        else
        {
            NamedHero = Words[1];
        }
    }

    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController || !PlayerController->GetPawn())
    {
        Reply(TEXT("Disguise needs a pawn to disguise"));
        return;
    }
    // #155: the pawn played now is another player's, or this one's own is.
    if (IsInPossession(PlayerController))
    {
        Reply(TEXT("You can't disguise while you control another player's pawn or they control yours (unpossess first)"));
        return;
    }

    // Find what to become first, so a name nothing matches leaves a disguise as it is.
    UClass* Class = FindDisguiseClass(Words[0]);
    UObject* Mesh = Class ? nullptr : FindStaticMesh(Words[0]);
    if (!Class && !Mesh)
    {
        Reply(FString::Printf(TEXT("No class or static mesh called '%s' (give its path, /Game/.../X, to load one that isn't yet)"), Words[0].c_str()));
        return;
    }
    if (Class && !Class->IsChildOf(AActor::StaticClass()))
    {
        Reply(FString::Printf(TEXT("%s isn't an actor class"), *Class->GetPathName()));
        return;
    }
    const bool bPawnClass = Class && Class->IsChildOf(APawn::StaticClass());
    const bool bPlayerPawnClass = bPawnClass && AFortPlayerPawn::StaticClass() && Class->IsChildOf(AFortPlayerPawn::StaticClass());
    if (!NamedHero.empty() && !bPlayerPawnClass)
    {
        Reply(Usage);
        return;
    }
    if (bPlayerPawnClass && !AFortPlayerController::CanPossess())
    {
        Reply(TEXT("This server can't possess another pawn (AFortPlayerController::Possess wasn't found)"));
        return;
    }
    std::wstring HeroFound;
    UFortHeroType* HeroType = bPlayerPawnClass ? HeroForPawnClass(Class, NamedHero, HeroFound) : nullptr;
    if (!NamedHero.empty() && !HeroType)
    {
        Reply(FString::Printf(TEXT("No hero called '%s' to bring the abilities of"), NamedHero.c_str()));
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
    const float Feet = FeetOf(Original);
    const std::wstring Name = Mesh ? Mesh->GetName().ToStdString() : WithoutClassSuffix(Class->GetName().ToStdString());

    FDisguise Disguise;
    Disguise.Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    Disguise.Original = TWeakObjectPtr<APawn>(Original);
    Disguise.Name = Name;
    Disguise.LastLocation = Location;

    if (bPlayerPawnClass)
    {
        // A player pawn class (the Ninja's...) is played: the player possesses it, with its own input, camera and
        // animations, and its hero's perks and abilities go on the player state while it lasts. The player's own pawn
        // waits hidden and follows it, so "undisguise" puts them back where it stands.
        Disguise.Kind = EDisguiseKind::Possessed;
        ParkOriginal(Disguise, Original);
        Disguise.LastFeet = Feet;
        // Facing the way the player looks, which Possess turns the control rotation to.
        AActor* Spawned = SpawnDisguiseActor(Original, Class, Location, FacingYaw(PlayerController, Original), Scale, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn, false);
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
        // The player's own pawn, parked, is owned by the disguise (#137): every client can tell it (ScanDisguises) and
        // turn its collision off there too, where the disguise the server drags it along with bumped into it. It is
        // simulated on every client, the player's too, until Possess makes it theirs again.
        OwnByPawn(Original, DisguisePawn);
        Disguise.OriginalRemoteRole = Original->RemoteRole;
        Original->RemoteRole = TEnumAsByte<ENetRole>(ROLE_SimulatedProxy);
        Original->ForceNetUpdate();
        Disguise.Actor = TWeakObjectPtr<AActor>(DisguisePawn);

        // The hero's kits: its perks and abilities (Mantis Leap's double jump, ...) on the player state's ability system.
        // Save the World also gets its gadgets (Crescent Kick, Dragon Slash...) on the quickbar, and the player's own
        // hero's back at the end; Battle Royale's quickbars have no gadget slots.
        int32_t Equipped = 0;
        bool bGadgets = false;
        AFortPlayerState* PlayerState = FortPlayerStateOf(PlayerController);
        UFortHero* Hero = HeroType ? CreateHero(HeroType) : nullptr;
        if (Hero && PlayerState)
        {
            for (UFortAbilityKit* Kit : HeroKits(HeroType, Hero))
            {
                std::vector<uint8_t> Handle;
                if (EquipKit(PlayerState, Kit, Handle))
                {
                    Disguise.KitHandles.push_back(std::move(Handle));
                    ++Equipped;
                }
            }
            AFortPlayerPawn* OwnPawn = Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Original));
            const TWeakObjectPtr<UFortHero> OwnHero = OwnPawn ? static_cast<const TWeakObjectPtr<UFortHero>&>(OwnPawn->Hero) : TWeakObjectPtr<UFortHero>();
            if (!AFortGameModeAthena::GetCurrent(PlayerController) && OwnHero.IsValid())
            {
                PlayerController->ApplyHeroAbilityKitItems(Hero);
                Disguise.bGaveGadgets = true;
                Disguise.OwnHero = OwnHero;
                bGadgets = true;
            }
        }
        Disguises.push_back(std::move(Disguise));
        StartWatchingDisguises();

        UE_LOG(LogFortDisguise, Log, TEXT("Disguise: %s plays %s (%s) with %s's %d kits%s"), *GetNameSafe(PlayerController), *DisguisePawn->GetName(), *Class->GetPathName(),
            HeroType ? *HeroType->GetName() : TEXT("no hero"), Equipped, bGadgets ? TEXT(" and gadgets") : TEXT(""));
        Reply(FString::Printf(TEXT("You are %s%s. Dying as it is a real death. \"cheat undisguise\" to be yourself again"), Name.c_str(),
            HeroType ? *FString::Printf(TEXT(", with %s's perks and abilities (%d kits%s)"), *HeroType->GetName(), Equipped, bGadgets ? TEXT(", and its gadgets") : TEXT("; its gadgets need Save the World's gadget slots")) : TEXT("")));
        return;
    }

    if (bPawnClass)
    {
        // Any other pawn (a husk, a Smasher...) is a puppet. A player playing it through Possess can't move it: a pawn's
        // input (MoveForward, MoveRight, the look and the actions) is bound by its own SetupPlayerInputComponent, which
        // only player pawns have (AFortPlayerPawn's, 1.7.2 0x7E74D0), so a possessed husk took no input and the camera
        // turned on its own (#136). The player keeps their own pawn (hidden, still theirs: input, camera, weapons and
        // health), and the puppet, with no AI controller and no collision, stands where it stands and moves at its
        // velocity: every client sees it walk, run and fall with its own animations. It can't attack.
        //
        // The player's own client doesn't take the puppet's position from the server (#137): it arrived a round trip
        // behind the pawn the client predicts, so the puppet trailed and jumped after it, and its capsule, which the
        // client still collided with, pushed the player's pawn about. Owned by the hidden pawn, the puppet is the
        // player's autonomous proxy: the engine sends its movement (ReplicatedMovement, ReplicatedMovementMode:
        // COND_SimulatedOrPhysics and COND_SimulatedOnly, 1.7.2 0x1EC7050) only to the clients it is simulated on, and the
        // player's client sets it on their pawn every frame (FollowPawn). The server attaches it to the pawn too, so the
        // other clients carry it on the pawn as well (MovePuppet, #154).
        Disguise.Kind = EDisguiseKind::Puppet;
        AActor* Puppet = SpawnDisguiseActor(Original, Class, Location, Yaw, Scale, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, true, true);
        if (!Puppet)
        {
            Reply(FString::Printf(TEXT("Couldn't spawn %s (an abstract class, or one that can't be spawned here)"), *Class->GetPathName()));
            return;
        }
        APawn* PuppetPawn = static_cast<APawn*>(Puppet);
        if (AController* AIController = PuppetPawn->Controller)
        {
            AIController->UnPossess();
            AIController->K2_DestroyActor();
        }
        OwnByPawn(Puppet, Original);
        Disguise.PuppetHalfHeight = HalfHeightOf(Puppet);
        Puppet->SetActorEnableCollision(false);
        Puppet->bCanBeDamaged = false;
        if (UCharacterMovementComponent* Movement = MovementOf(PuppetPawn))
        {
            Movement->bRunPhysicsWithNoController = false;
            Movement->SetMovementMode(MOVE_Walking);
        }
        if (Puppet->NetUpdateFrequency < 30.0f)
        {
            Puppet->NetUpdateFrequency = 30.0f;
        }
        Puppet->RemoteRole = TEnumAsByte<ENetRole>(ROLE_AutonomousProxy);
        Original->SetActorHiddenInGame(true);
        HideWeapon(Disguise, Original);
        Original->ForceNetUpdate();
        Disguise.Actor = TWeakObjectPtr<AActor>(Puppet);
        MovePuppet(Disguise, Original, Puppet);
        Disguises.push_back(std::move(Disguise));
        StartWatchingDisguises();

        UE_LOG(LogFortDisguise, Log, TEXT("Disguise: %s walks as the puppet %s (%s)"), *GetNameSafe(PlayerController), *Puppet->GetName(), *Class->GetPathName());
        Reply(FString::Printf(TEXT("You are %s: it walks where you walk, with its animations (it can't attack). \"cheat undisguise\" to be seen again"), Name.c_str()));
        return;
    }

    // A prop rides on the player's pawn, hidden with its weapon. A class whose root can move (and so follow the player on
    // every client) is spawned as it is; a building prop (a toilet, a tree...) and a plain mesh can't (their roots are
    // static), so their mesh goes on a carrier whose root can. Without collision, so shots go through to the player.
    const FVector AtFeet(Location.X, Location.Y, Feet);
    UObject* ClassMesh = Class ? MeshOfClass(Class) : nullptr;
    AActor* Prop = nullptr;
    bool bCarried = false;
    if (Class && !ClassMesh)
    {
        Prop = SpawnDisguiseActor(Original, Class, AtFeet, Yaw, Scale, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, true, true);
        USceneComponent* Root = Prop ? Prop->GetRootComponent() : nullptr;
        if (Prop && (!Root || Root->Mobility == EComponentMobility::Static))
        {
            Prop->K2_DestroyActor();
            Reply(FString::Printf(TEXT("%s can't move (%s) and shows no mesh Core can carry; pick another (\"disguise Bathroom_ToiletBowl01\")"), Name.c_str(), Root ? TEXT("its root is static") : TEXT("it has no root")));
            return;
        }
    }
    else
    {
        Prop = SpawnMeshCarrier(Original, Mesh ? Mesh : ClassMesh, AtFeet, Yaw, Scale);
        bCarried = Prop != nullptr;
    }
    if (!Prop)
    {
        Reply(FString::Printf(TEXT("Couldn't spawn %s (%s)"), Name.c_str(), Class && !ClassMesh ? TEXT("an abstract class, or one that can't be spawned here") : TEXT("no carrier for its mesh")));
        return;
    }
    OwnByPawn(Prop, Original);
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
    Disguise.Kind = EDisguiseKind::Prop;
    Disguise.Actor = TWeakObjectPtr<AActor>(Prop);
    Disguises.push_back(std::move(Disguise));
    StartWatchingDisguises();

    UE_LOG(LogFortDisguise, Log, TEXT("Disguise: %s is the prop %s (%s%s), attached to %s"), *GetNameSafe(PlayerController), *Prop->GetName(),
        Mesh ? *Mesh->GetPathName() : *Class->GetPathName(), bCarried ? *FString::Printf(TEXT(", its mesh %s on a carrier"), *GetNameSafe(Mesh ? Mesh : ClassMesh)) : TEXT(""), *Original->GetName());
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
    if (Disguise->Kind == EDisguiseKind::Possessed)
    {
        bEnded = EndPossessedDisguise(*Disguise);
    }
    else
    {
        EndShownDisguise(*Disguise);
    }
    Disguises.erase(Disguises.begin() + (Disguise - Disguises.data()));
    Reply(bEnded ? FString::Printf(TEXT("You are no longer disguised as %s"), Name.c_str())
                 : FString::Printf(TEXT("Your own pawn is gone, so you stay %s"), Name.c_str()));
#endif
}

// ----------------------------------------------------------------------------------------------------------------------
// "cheat possessplayer" and "cheat unpossess" (#155)
// ----------------------------------------------------------------------------------------------------------------------

#if WITH_SERVER_CODE
namespace
{
    // The look a player state gives the pawn it plays (SetHeroType's): its character parts and gender.
    struct FLook
    {
        std::vector<UCustomCharacterPart*> Parts;
        uint8_t Gender = 0;
    };

    // One player (the possessor) playing another's (the owner's) pawn. The possessor's own pawn is parked the way a
    // player pawn disguise parks it (hidden, without collision or movement, invulnerable, simulated on its client), but
    // where it stood: "unpossess" puts them back there. The owner's controller lets go of its pawn and spectates it the
    // way the game spectates a teammate, so they watch what the possessor does with it. The possessed pawn can't be
    // damaged meanwhile: its death would be the possessor's (the game ends whoever controls a dying pawn), not its
    // owner's.
    struct FPossession
    {
        TWeakObjectPtr<AFortPlayerController> Possessor;
        TWeakObjectPtr<AFortPlayerController> Owner;
        TWeakObjectPtr<APawn> OwnPawn;
        TWeakObjectPtr<APawn> Possessed;
        // How the possessor's own pawn was before it was parked (ParkOriginal's and UnparkOriginal's).
        FDisguise Parking;
        ENetRole OwnRemoteRole = ROLE_AutonomousProxy;
        bool bPossessedCanBeDamaged = true;
        FString OwnerName;
        FString PossessorName;
        ULONGLONG NextViewCheck = 0;
        // The possessor's own look, given back when the possession ends: meanwhile their player state (which the pawn
        // takes) wears the owner's.
        std::optional<FLook> PossessorLook;
        bool bOwnerSpectating = false;
    };
    std::vector<FPossession> Possessions;
    bool bWatchingPossessions = false;
    constexpr ULONGLONG PossessionViewCheckMs = 1000;

    FLook LookOf(AFortPlayerState* PlayerState)
    {
        FLook Look;
        const EFortCustomPartType NumTypes = EFortCustomPartType::GetNumTypes();
        for (uint8_t PartType = 0; NumTypes != Reflection::InvalidEnumValue && PartType < NumTypes; ++PartType)
        {
            Look.Parts.push_back(PlayerState->GetCharacterPart(EFortCustomPartType{ PartType }));
        }
        Look.Gender = PlayerState->CharacterGender;
        return Look;
    }

    // The player state's parts replicate to every client, whose OnRep_CharacterParts dresses the pawn it plays; the
    // server dresses its own copy of the pawn the same way (its mesh and anim class).
    void Wear(AFortPlayerState* PlayerState, const FLook& Look)
    {
        for (size_t PartType = 0; PartType < Look.Parts.size(); ++PartType)
        {
            PlayerState->GetCharacterPart(EFortCustomPartType{ static_cast<uint8_t>(PartType) }) = Look.Parts[PartType];
        }
        PlayerState->CharacterGender = Look.Gender;
        PlayerState->OnRep_CharacterParts();
        PlayerState->ForceNetUpdate();
    }

    // A copy of a UStruct value (its own copy and destruction, so FStrings and arrays in it are kept right).
    std::shared_ptr<uint8_t> CopyStructValue(UScriptStruct* Struct, const void* Source)
    {
        if (!Struct || !Source)
        {
            return nullptr;
        }
        uint8_t* Data = new uint8_t[static_cast<size_t>(Struct->GetStructureSize())];
        Struct->InitializeStruct(Data);
        Struct->CopyScriptStruct(Data, Source);
        return std::shared_ptr<uint8_t>(Data, [Struct](uint8_t* Value)
        {
            Struct->DestroyStruct(Value);
            delete[] Value;
        });
    }

    // AController::StateName (NAME_Playing, NAME_Spectating, ...), which APlayerController's ChangeState sets.
    FName* StateNameOf(AController* Controller)
    {
        static const int32_t Offset = []
        {
            const UProperty* Property = AController::StaticClass() ? AController::StaticClass()->FindPropertyByName(FName(L"StateName", FNAME_Find)) : nullptr;
            return Property ? Property->GetOffset_ForInternal() : -1;
        }();
        return Controller && Offset >= 0 ? reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(Controller) + Offset) : nullptr;
    }

    // The client's controller goes to that state (APlayerController::ClientGotoState).
    void ClientGotoState(AFortPlayerController* PlayerController, const wchar_t* State)
    {
        Reflection::FParams Params(FindObject<UFunction>(nullptr, L"/Script/Engine.PlayerController:ClientGotoState"), true);
        if (Params.Set(L"NewState", FName(State)))
        {
            Params.Call(PlayerController);
        }
    }

    FString PlayerNameOf(AController* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(Controller->PlayerState) : nullptr;
        return PlayerState ? PlayerState->GetPlayerName() : GetNameSafe(Controller);
    }

    FPossession* FindPossession(const AFortPlayerController* PlayerController, bool bAsPossessor)
    {
        auto Found = std::find_if(Possessions.begin(), Possessions.end(), [PlayerController, bAsPossessor](const FPossession& Possession)
        {
            return (bAsPossessor ? Possession.Possessor.Get() : Possession.Owner.Get()) == PlayerController;
        });
        return Found != Possessions.end() ? &*Found : nullptr;
    }

    // The controller's view (its camera manager's view target, sent to its client), through
    // APlayerController::SetViewTargetWithBlend.
    void SetViewTarget(AFortPlayerController* PlayerController, AActor* Target)
    {
        Reflection::FParams Params(FindObject<UFunction>(nullptr, L"/Script/Engine.PlayerController:SetViewTargetWithBlend"), true);
        if (Params.Set(L"NewViewTarget", Target))
        {
            Params.Call(PlayerController);
        }
    }

    AActor* ViewTargetOf(AFortPlayerController* PlayerController)
    {
        Reflection::FParams Params(FindObject<UFunction>(nullptr, L"/Script/Engine.Controller:GetViewTarget"), true);
        return Params.Call(PlayerController) ? Params.Take<AActor*>(L"ReturnValue") : nullptr;
    }

    // The client's camera views the actor (APlayerController::ClientSetViewTarget). The server's camera manager sends it
    // only when its own view target changes; the same target again changes nothing on the client.
    void ClientSetViewTarget(AFortPlayerController* PlayerController, AActor* Target)
    {
        Reflection::FParams Params(FindObject<UFunction>(nullptr, L"/Script/Engine.PlayerController:ClientSetViewTarget"), true);
        if (Params.Set(L"A", Target))
        {
            Params.Call(PlayerController);
        }
    }

    // #155: the owner watched their pawn from where they stood. They spectate it now the way Battle Royale spectates a
    // teammate (AFortPlayerControllerAthena::SpectateOnDeath, 1.9 0x438550): the controller goes to the Spectating
    // state on the server and its client, then views the pawn (SetViewTarget and ClientSetViewTarget). A client still
    // "Playing" without a pawn kept its camera where it was. The server's state is the controller's StateName (what
    // ChangeState sets; a remote controller's BeginSpectatingState spawns nothing on the server), so the spectator
    // location its client sends is taken and the client isn't sent back to Playing; Possess puts both back to Playing.
    void SpectatePawn(AFortPlayerController* PlayerController, APawn* Pawn)
    {
        if (FName* StateName = StateNameOf(PlayerController))
        {
            *StateName = FName(L"Spectating");
        }
        ClientGotoState(PlayerController, L"Spectating");
        SetViewTarget(PlayerController, Pawn);
        ClientSetViewTarget(PlayerController, Pawn);
    }

    // A spectating owner left without a pawn (none to give back, none restarted) plays again.
    void StopSpectating(AFortPlayerController* PlayerController)
    {
        FName* StateName = StateNameOf(PlayerController);
        if (!PlayerController || PlayerController->GetPawn() || !StateName || *StateName != FName(L"Spectating"))
        {
            return;
        }
        *StateName = FName(L"Playing");
        ClientGotoState(PlayerController, L"Playing");
        SetViewTarget(PlayerController, PlayerController);
    }

    // A player left without a pawn by a possession that couldn't give theirs back gets a new one, the way the game mode
    // starts every player (not while the aircraft takes everyone: it gives them their pawn when they jump).
    void RestartWithoutPawn(AFortPlayerController* PlayerController)
    {
        UWorld* World = PlayerController && !PlayerController->GetPawn() && !IsInAircraftPhase(PlayerController) ? PlayerController->GetWorld() : nullptr;
        AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
        if (GameMode)
        {
            GameMode->RestartPlayer(PlayerController);
        }
    }

    // Ends the possession: the possessor plays their own pawn again where it waited, and the owner theirs (as it is
    // now, where the possessor left it). What is gone is dealt with: the possessed pawn without an owner goes, an owner
    // left without their pawn gets a new one, a parked pawn whose player left goes.
    void EndPossession(FPossession& Possession)
    {
        AFortPlayerController* Possessor = Possession.Possessor.Get();
        AFortPlayerController* Owner = Possession.Owner.Get();
        APawn* OwnPawn = Possession.OwnPawn.Get();
        APawn* Possessed = Possession.Possessed.Get();
        const bool bPossessedAlive = Possessed && !IsDeadPawn(Possessed);
        if (Possessed)
        {
            Possessed->bCanBeDamaged = Possession.bPossessedCanBeDamaged;
        }

        // A possessor who has a pawn of another kind now (a respawn) keeps it; their parked one goes.
        APawn* const PossessorPawn = Possessor ? Possessor->GetPawn() : nullptr;
        const bool bPossessorElsewhere = PossessorPawn && PossessorPawn != Possessed && PossessorPawn != OwnPawn;
        if (OwnPawn && Possessor && !IsDeadPawn(OwnPawn) && !bPossessorElsewhere)
        {
            OwnPawn->RemoteRole = TEnumAsByte<ENetRole>(Possession.OwnRemoteRole);
            if (PossessorPawn != OwnPawn)
            {
                PossessForDisguise(Possessor, OwnPawn);
            }
            UnparkOriginal(Possession.Parking, OwnPawn);
        }
        else
        {
            if (Possessor && Possessed && Possessor->GetPawn() == Possessed)
            {
                Possessor->UnPossess();
            }
            if (OwnPawn && (!OwnPawn->Controller || static_cast<AController*>(OwnPawn->Controller) == static_cast<AController*>(Possessor)) && PossessorPawn != OwnPawn)
            {
                OwnPawn->K2_DestroyActor();
            }
            RestartWithoutPawn(Possessor);
        }

        if (Owner && bPossessedAlive && !Owner->GetPawn())
        {
            if (AController* Holder = Possessed->Controller)
            {
                Holder->UnPossess();
            }
            PossessForDisguise(Owner, Possessed);
        }
        else
        {
            if (Possessed && !Possessed->Controller && (!Owner || Owner->GetPawn()))
            {
                Possessed->K2_DestroyActor();
            }
            RestartWithoutPawn(Owner);
        }
        // Possess put the owner back to Playing; one left without a pawn stops spectating here.
        if (Possession.bOwnerSpectating)
        {
            StopSpectating(Owner);
        }

        // The possessor's player state wears their own look again (their pawn, and any they get later, takes it).
        AFortPlayerState* PossessorState = Possession.PossessorLook ? FortPlayerStateOf(Possessor) : nullptr;
        if (PossessorState)
        {
            Wear(PossessorState, *Possession.PossessorLook);
        }
        UE_LOG(LogFortDisguise, Log, TEXT("PossessPlayer: %s gave %s's pawn %s back (own pawn %s)"), *Possession.PossessorName, *Possession.OwnerName,
            *GetNameSafe(Possessed), *GetNameSafe(OwnPawn));
    }

    // Once a tick while anyone is possessed: the owner keeps watching their pawn, and a possession whose possessor,
    // owner or pawn went (left the game, died, respawned, the aircraft) ends. Returns false when it ended.
    bool TickPossession(FPossession& Possession)
    {
        AFortPlayerController* Possessor = Possession.Possessor.Get();
        AFortPlayerController* Owner = Possession.Owner.Get();
        APawn* Possessed = Possession.Possessed.Get();
        const bool bPlaying = Possessor && Owner && Possessed && !IsDeadPawn(Possessed) && Possessor->GetPawn() == Possessed && !Owner->GetPawn();
        if (bPlaying)
        {
            Possession.Parking.LastLocation = ActorLocation(Possessed);
            const ULONGLONG Now = GetTickCount64();
            // The server's view target back on the pawn if anything moved it, and the client's told again: a client that
            // already views the pawn ignores it, one that lost it (it took the pawn before the pawn's new state reached
            // it, or a spectator input moved it) views it again.
            if (Now >= Possession.NextViewCheck)
            {
                Possession.NextViewCheck = Now + PossessionViewCheckMs;
                if (ViewTargetOf(Owner) != static_cast<AActor*>(Possessed))
                {
                    SetViewTarget(Owner, Possessed);
                }
                ClientSetViewTarget(Owner, Possessed);
            }
            return true;
        }

        const FString Why = !Possessor ? FString::Printf(TEXT("%s left"), *Possession.PossessorName)
            : !Owner ? FString::Printf(TEXT("%s left"), *Possession.OwnerName)
            : !Possessed ? FString::Printf(TEXT("%s's pawn is gone"), *Possession.OwnerName)
            : IsDeadPawn(Possessed) ? FString::Printf(TEXT("%s's pawn died"), *Possession.OwnerName)
            : FString(TEXT("one of you has another pawn now"));
        EndPossession(Possession);
        if (Possessor)
        {
            Tell(Possessor, FString::Printf(TEXT("You no longer control %s's pawn: %s"), *Possession.OwnerName, *Why));
        }
        if (Owner)
        {
            Tell(Owner, FString::Printf(TEXT("%s no longer controls your pawn: %s"), *Possession.PossessorName, *Why));
        }
        return false;
    }

    void WatchPossessions()
    {
        for (size_t Index = 0; Index < Possessions.size();)
        {
            if (TickPossession(Possessions[Index]))
            {
                ++Index;
            }
            else
            {
                Possessions.erase(Possessions.begin() + static_cast<ptrdiff_t>(Index));
            }
        }
        bWatchingPossessions = !Possessions.empty() && GameThread::Enqueue(&WatchPossessions);
    }
}
#endif

bool UFortCheatManager::IsInPossession(const AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    return PlayerController && (FindPossession(PlayerController, true) || FindPossession(PlayerController, false));
#else
    return false;
#endif
}

void UFortCheatManager::PossessPlayer(const FString& Arguments)
{
#if WITH_SERVER_CODE
    std::wstring Name;
    for (const std::wstring& Word : SplitWords(Arguments.ToStdString()))
    {
        Name += (Name.empty() ? L"" : L" ") + Word;
    }
    if (Name.size() > 2 && Name.front() == L'/' && Name.back() == L'/')
    {
        Name = Name.substr(1, Name.size() - 2);
    }
    if (Name.empty())
    {
        Reply(TEXT("Usage: possessplayer <player>, their name or its start (\"possessplayer Bob\"): you control their pawn while they watch; \"unpossess\" gives it back"));
        return;
    }
    AFortPlayerController* PlayerController = GetPlayerController();
    if (!PlayerController)
    {
        return;
    }
    if (!AFortPlayerController::CanPossess())
    {
        Reply(TEXT("This server can't possess another pawn (AFortPlayerController::Possess wasn't found)"));
        return;
    }
    if (FPossession* Possessed = FindPossession(PlayerController, false))
    {
        Reply(FString::Printf(TEXT("%s controls your pawn: you can't take another until they give it back"), *Possessed->PossessorName));
        return;
    }
    if (FindDisguise(PlayerController))
    {
        Reply(TEXT("You're disguised: undisguise first"));
        return;
    }
    FString Error;
    AFortPlayerController* Owner = FindCheatTarget(PlayerController, FString(Name.c_str()), Error);
    if (!Owner)
    {
        Reply(Error);
        return;
    }
    const FString OwnerName = PlayerNameOf(Owner);
    if (Owner == PlayerController)
    {
        Reply(TEXT("That's you"));
        return;
    }
    if (FPossession* Current = FindPossession(Owner, true); Current || FindPossession(Owner, false))
    {
        Reply(FString::Printf(TEXT("%s is in a possession already (%s); try again once it ends"), *OwnerName,
            Current ? *FString::Printf(TEXT("they control %s's pawn"), *Current->OwnerName) : TEXT("someone controls their pawn")));
        return;
    }
    if (FindDisguise(Owner))
    {
        Reply(FString::Printf(TEXT("%s is disguised: they (or \"undisguise /%s/\") have to undisguise first"), *OwnerName, *OwnerName));
        return;
    }

    // Possessing someone else first gives the last one back.
    if (FPossession* Previous = FindPossession(PlayerController, true))
    {
        const FString PreviousName = Previous->OwnerName;
        AFortPlayerController* PreviousOwner = Previous->Owner.Get();
        EndPossession(*Previous);
        Possessions.erase(Possessions.begin() + (Previous - Possessions.data()));
        if (PreviousOwner)
        {
            Tell(PreviousOwner, FString::Printf(TEXT("%s gave your pawn back"), *PlayerNameOf(PlayerController)));
        }
        Reply(FString::Printf(TEXT("You gave %s's pawn back"), *PreviousName));
    }

    APawn* OwnPawn = PlayerController->GetPawn();
    APawn* Target = Owner->GetPawn();
    if (!OwnPawn || IsDeadPawn(OwnPawn))
    {
        Reply(TEXT("You need a pawn that is alive to leave while you control another"));
        return;
    }
    if (!Target || IsDeadPawn(Target))
    {
        Reply(FString::Printf(TEXT("%s has no pawn that is alive to control (dead, or in the aircraft)"), *OwnerName));
        return;
    }

    FPossession Possession;
    Possession.Possessor = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    Possession.Owner = TWeakObjectPtr<AFortPlayerController>(Owner);
    Possession.OwnPawn = TWeakObjectPtr<APawn>(OwnPawn);
    Possession.Possessed = TWeakObjectPtr<APawn>(Target);
    Possession.OwnerName = OwnerName;
    Possession.PossessorName = PlayerNameOf(PlayerController);
    Possession.Parking.Controller = TWeakObjectPtr<AFortPlayerController>(PlayerController);
    Possession.Parking.LastLocation = ActorLocation(Target);
    Possession.bPossessedCanBeDamaged = Target->bCanBeDamaged;

    // #155: the pawn keeps its owner's look. It takes the possessor's player state, whose character parts dress it
    // (Save the World's hero and Battle Royale's outfit alike), so that player state wears the owner's parts meanwhile;
    // Battle Royale's Possess also copies the possessor's cosmetic loadout (glider, pickaxe, contrail...) onto the pawn,
    // which gets the owner's back. The items and quickbars stay the possessor's: they belong to their controller.
    AFortPlayerState* const OwnerState = FortPlayerStateOf(Owner);
    AFortPlayerState* const PossessorState = FortPlayerStateOf(PlayerController);
    const std::optional<FLook> OwnerLook = OwnerState && PossessorState ? std::optional<FLook>(LookOf(OwnerState)) : std::nullopt;
    AFortPlayerPawnAthena* const AthenaTarget = Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Target));
    const std::shared_ptr<uint8_t> OwnerLoadout = AthenaTarget && AthenaTarget->HasCustomizationLoadout()
        ? CopyStructValue(FFortAthenaLoadout::StaticStruct(), &static_cast<FFortAthenaLoadout&>(AthenaTarget->CustomizationLoadout)) : nullptr;

    ParkOriginal(Possession.Parking, OwnPawn);
    Owner->UnPossess();
    PossessForDisguise(PlayerController, Target);
    if (PlayerController->GetPawn() != Target)
    {
        // Undo: the owner takes their pawn again, the possessor theirs.
        if (!Target->Controller)
        {
            PossessForDisguise(Owner, Target);
        }
        if (PlayerController->GetPawn() != OwnPawn)
        {
            PossessForDisguise(PlayerController, OwnPawn);
        }
        UnparkOriginal(Possession.Parking, OwnPawn);
        Reply(FString::Printf(TEXT("Couldn't take over %s's pawn"), *OwnerName));
        return;
    }
    Target->bCanBeDamaged = false;
    // Parked under the possessed pawn, every client turns its collision off (ScanDisguises), and it is simulated on its
    // own client too until Possess makes it its player's again.
    OwnByPawn(OwnPawn, Target);
    Possession.OwnRemoteRole = OwnPawn->RemoteRole;
    OwnPawn->RemoteRole = TEnumAsByte<ENetRole>(ROLE_SimulatedProxy);
    OwnPawn->ForceNetUpdate();

    if (OwnerLook)
    {
        Possession.PossessorLook = LookOf(PossessorState);
        Wear(PossessorState, *OwnerLook);
    }
    if (OwnerLoadout)
    {
        FFortAthenaLoadout::StaticStruct()->CopyScriptStruct(&static_cast<FFortAthenaLoadout&>(AthenaTarget->CustomizationLoadout), OwnerLoadout.get());
        if (UFunction* OnRepLoadout = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawnAthena:OnRep_CustomizationLoadout"))
        {
            Reflection::FParams Params(OnRepLoadout);
            Params.Call(AthenaTarget);
        }
    }
    Target->ForceNetUpdate();

    SpectatePawn(Owner, Target);
    Possession.bOwnerSpectating = true;
    Possessions.push_back(std::move(Possession));
    if (!bWatchingPossessions)
    {
        bWatchingPossessions = GameThread::Enqueue(&WatchPossessions);
    }

    UE_LOG(LogFortDisguise, Log, TEXT("PossessPlayer: %s controls %s's pawn %s (own pawn %s parked)"), *GetNameSafe(PlayerController), *OwnerName, *GetNameSafe(Target), *GetNameSafe(OwnPawn));
    Tell(Owner, FString::Printf(TEXT("%s controls your pawn for now: you watch it until they give it back"), *Possessions.back().PossessorName));
    Reply(FString::Printf(TEXT("You control %s's pawn while they watch; it can't be hurt meanwhile. \"unpossess\" gives it back and you're yourself again where you stood"), *OwnerName));
#endif
}

void UFortCheatManager::Unpossess(const FString& Arguments)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = GetPlayerController();
    FPossession* Possession = PlayerController ? FindPossession(PlayerController, true) : nullptr;
    if (!Possession)
    {
        FPossession* Possessed = PlayerController ? FindPossession(PlayerController, false) : nullptr;
        Reply(Possessed ? FString::Printf(TEXT("%s controls your pawn; they give it back with \"unpossess\""), *Possessed->PossessorName)
                        : FString(TEXT("You don't control anyone's pawn")));
        return;
    }
    const FString OwnerName = Possession->OwnerName;
    const FString PossessorName = Possession->PossessorName;
    AFortPlayerController* Owner = Possession->Owner.Get();
    EndPossession(*Possession);
    Possessions.erase(Possessions.begin() + (Possession - Possessions.data()));
    if (Owner)
    {
        Tell(Owner, FString::Printf(TEXT("%s gave your pawn back"), *PossessorName));
    }
    Reply(FString::Printf(TEXT("You gave %s's pawn back and are yourself again"), *OwnerName));
#endif
}

// ----------------------------------------------------------------------------------------------------------------------
// The disguise as the clients show it (#137)
// ----------------------------------------------------------------------------------------------------------------------

#if !UE_SERVER
namespace
{
    class AActorAttachments : public UObject
    {
        DefineUnrealClass(AActor, Engine)
        DefineUFunction(GetAttachedActors)

    public:
        TArray<AActor*> GetAttachedActors()
        {
            UFunction* Function = FindStaticFunction_GetAttachedActors();
            if (!Function)
            {
                return TArray<AActor*>();
            }
            Reflection::FParams Params(Function);
            return Params.Call(this) ? Params.Take<TArray<AActor*>>(L"OutActors") : TArray<AActor*>();
        }
    };

    // The Fortnite camera: the camera modes it blends (Athena_PlayerCameraModeBase and the other third person modes,
    // Default3PCamera and the older ones), each an object of its own that keeps its offsets.
    struct FActiveFortCamera
    {
        DefineUnrealStruct(FActiveFortCamera, FortniteGame)
        DefineUProperty(UObject*, Camera)
    };

    class AFortPlayerCameraBaseStack : public UObject
    {
        DefineUnrealClass(AFortPlayerCameraBase, FortniteGame)

    public:
        DefineUStructArrayProperty(FActiveFortCamera, CameraBlendStack)

        static bool HasCameraBlendStack() { return Member_CameraBlendStack.IsAvailable(); }
    };

    struct FInterpOffsetData
    {
        DefineUnrealStruct(FInterpOffsetData, FortniteGame)
        DefineUProperty(FVector, ViewOffset)
    };

    struct FInterpOffset
    {
        DefineUnrealStruct(FInterpOffset, FortniteGame)
        DefineUStructArrayProperty(FInterpOffsetData, SamplePoints)
    };

    // UFortCameraMode_ThirdPerson: the camera sits CameraSpaceForwardDistance along the view (-280 in
    // Athena_PlayerCameraModeBase and v3_PlayerCameraModeBase: behind) from its origin, ViewTargetSpaceViewOffset (45 to
    // the right, 75 up) off the pawn.
    class UFortCameraModeThirdPerson : public UObject
    {
        DefineUnrealClass(UFortCameraMode_ThirdPerson, FortniteGame)

    public:
        DefineUProperty(float, CameraSpaceForwardDistance)
        DefineUStructProperty(FInterpOffset, ViewTargetSpaceViewOffset)
        DefineUStructProperty(FInterpOffset, ViewTargetSpaceViewOffset_Crouch)

        static bool HasOffsets() { return Member_CameraSpaceForwardDistance.IsAvailable() && Member_ViewTargetSpaceViewOffset.IsAvailable() && FInterpOffsetData::StaticStruct(); }
        // 1.1 and 1.2 have no crouching offset.
        static bool HasCrouchOffset() { return Member_ViewTargetSpaceViewOffset_Crouch.IsAvailable(); }
    };

    struct FViewOffsetData
    {
        DefineUnrealStruct(FViewOffsetData, FortniteGame)
        DefineUProperty(FVector, OffsetHigh)
        DefineUProperty(FVector, OffsetMid)
        DefineUProperty(FVector, OffsetLow)
    };

    // UFort3PCameraMode (Default3PCamera...): the camera sits ViewOffsets off its pivot, by the view's pitch (X -200 to
    // -300: behind).
    class UFort3PCameraModeOffsets : public UObject
    {
        DefineUnrealClass(UFort3PCameraMode, FortniteGame)

    public:
        DefineUStructProperty(FViewOffsetData, ViewOffsets)

        static bool HasOffsets() { return Member_ViewOffsets.IsAvailable(); }
    };

    // "cheat disguiseview": how the player sees their own disguise.
    enum class EDisguiseView : uint8_t
    {
        // The camera pulls back (and up) for a disguise bigger than the player.
        Back,
        // The disguise is hidden from its own player (everyone else still sees it).
        Hide,
        // As it is, at the usual camera distance.
        Normal,
    };
    EDisguiseView DisguiseView = EDisguiseView::Back;
    // "cheat disguiseview back <distance> [up]" (#142): how far back and up the camera pulls, in place of the distance
    // measured from the disguise's size. "cheat disguiseview back" measures it again.
    std::optional<float> ChosenBack;
    std::optional<float> ChosenUp;
    constexpr float MaxChosenBack = 5000.0f;
    constexpr float MaxChosenUp = 2000.0f;

    const wchar_t* DisguiseViewName(EDisguiseView View)
    {
        return View == EDisguiseView::Back ? L"back" : View == EDisguiseView::Hide ? L"hide" : L"normal";
    }

    // The local player's own disguise.
    struct FOwnDisguise
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<APawn> Pawn;
        // A puppet (the pawn the server makes this player's autonomous proxy) follows the pawn here; a prop is attached
        // by the server.
        bool bPuppet = false;
        bool bAttached = false;
        bool bHiddenHere = false;
        float PuppetHalfHeight = 0.0f;
        // How far the camera pulls back and up for it.
        float Back = 0.0f;
        float Up = 0.0f;
    };
    FOwnDisguise Own;

    // Every disguise this client turned the collision of off.
    std::vector<TWeakObjectPtr<AActor>> Quieted;
    // The parked pawns of players playing a player pawn disguise, whose collision comes back when they do.
    std::vector<TWeakObjectPtr<AActor>> Parked;

    // Other players' puppets (simulated here), carried on their hidden pawns (#154).
    struct FOtherPuppet
    {
        TWeakObjectPtr<AActor> Puppet;
        TWeakObjectPtr<APawn> Pawn;
        float HalfHeight = 0.0f;
        bool bMovementStopped = false;
    };
    std::vector<FOtherPuppet> OtherPuppets;

    // A camera mode Core moved back, as it was.
    struct FCameraAdjustment
    {
        TWeakObjectPtr<UObject> Mode;
        bool bThirdPerson = false;
        bool bAdjusted = false;
        float ForwardDistance = 0.0f;
        std::vector<float> OffsetZ;
        FVector High;
        FVector Mid;
        FVector Low;
    };
    std::vector<FCameraAdjustment> CameraAdjustments;
    float AppliedBack = 0.0f;
    float AppliedUp = 0.0f;

    ULONGLONG NextScan = 0;
    constexpr ULONGLONG ScanMs = 250;

    bool IsA(const void* Object, UClass* Class)
    {
        return Object && Class && reinterpret_cast<const UObject*>(Object)->IsA(Class);
    }

    template <typename FVisit>
    void ForEachSample(FInterpOffset& Offset, FVisit&& Visit)
    {
        TStructArray<FInterpOffsetData>& Samples = Offset.SamplePoints;
        for (int32_t Index = 0; Index < Samples.Num(); ++Index)
        {
            Visit(Samples[Index]);
        }
    }

    void RestoreCamera()
    {
        for (FCameraAdjustment& Adjustment : CameraAdjustments)
        {
            UObject* Mode = Adjustment.Mode.Get();
            if (!Mode || !Adjustment.bAdjusted)
            {
                continue;
            }
            if (Adjustment.bThirdPerson)
            {
                UFortCameraModeThirdPerson* ThirdPerson = static_cast<UFortCameraModeThirdPerson*>(Mode);
                ThirdPerson->CameraSpaceForwardDistance = Adjustment.ForwardDistance;
                size_t Next = 0;
                const auto Restore = [&Adjustment, &Next](FInterpOffsetData& Sample)
                {
                    if (Next < Adjustment.OffsetZ.size())
                    {
                        FVector Offset = Sample.ViewOffset;
                        Offset.Z = Adjustment.OffsetZ[Next++];
                        Sample.ViewOffset = Offset;
                    }
                };
                ForEachSample(ThirdPerson->ViewTargetSpaceViewOffset, Restore);
                if (UFortCameraModeThirdPerson::HasCrouchOffset())
                {
                    ForEachSample(ThirdPerson->ViewTargetSpaceViewOffset_Crouch, Restore);
                }
            }
            else
            {
                FViewOffsetData& Offsets = static_cast<UFort3PCameraModeOffsets*>(Mode)->ViewOffsets;
                Offsets.OffsetHigh = Adjustment.High;
                Offsets.OffsetMid = Adjustment.Mid;
                Offsets.OffsetLow = Adjustment.Low;
            }
        }
        CameraAdjustments.clear();
        AppliedBack = 0.0f;
        AppliedUp = 0.0f;
    }

    // Moves a camera mode Back further behind the player and Up higher, keeping it as it was.
    void AdjustCameraMode(UObject* Mode, float Back, float Up)
    {
        FCameraAdjustment Adjustment;
        Adjustment.Mode = TWeakObjectPtr<UObject>(Mode);
        if (IsA(Mode, UFortCameraModeThirdPerson::StaticClass()) && UFortCameraModeThirdPerson::HasOffsets())
        {
            UFortCameraModeThirdPerson* ThirdPerson = static_cast<UFortCameraModeThirdPerson*>(Mode);
            Adjustment.bThirdPerson = true;
            Adjustment.bAdjusted = true;
            Adjustment.ForwardDistance = ThirdPerson->CameraSpaceForwardDistance;
            ThirdPerson->CameraSpaceForwardDistance = Adjustment.ForwardDistance - Back;
            const auto Raise = [&Adjustment, Up](FInterpOffsetData& Sample)
            {
                FVector Offset = Sample.ViewOffset;
                Adjustment.OffsetZ.push_back(Offset.Z);
                Offset.Z += Up;
                Sample.ViewOffset = Offset;
            };
            ForEachSample(ThirdPerson->ViewTargetSpaceViewOffset, Raise);
            if (UFortCameraModeThirdPerson::HasCrouchOffset())
            {
                ForEachSample(ThirdPerson->ViewTargetSpaceViewOffset_Crouch, Raise);
            }
        }
        else if (IsA(Mode, UFort3PCameraModeOffsets::StaticClass()) && UFort3PCameraModeOffsets::HasOffsets() && FViewOffsetData::StaticStruct())
        {
            FViewOffsetData& Offsets = static_cast<UFort3PCameraModeOffsets*>(Mode)->ViewOffsets;
            Adjustment.bAdjusted = true;
            Adjustment.High = Offsets.OffsetHigh;
            Adjustment.Mid = Offsets.OffsetMid;
            Adjustment.Low = Offsets.OffsetLow;
            Offsets.OffsetHigh = FVector(Adjustment.High.X - Back, Adjustment.High.Y, Adjustment.High.Z + Up);
            Offsets.OffsetMid = FVector(Adjustment.Mid.X - Back, Adjustment.Mid.Y, Adjustment.Mid.Z + Up);
            Offsets.OffsetLow = FVector(Adjustment.Low.X - Back, Adjustment.Low.Y, Adjustment.Low.Z + Up);
        }
        // A mode with neither (the death camera...) is kept as it is, and remembered so it isn't looked at again.
        CameraAdjustments.push_back(std::move(Adjustment));
    }

    // Every camera mode the player's camera blends is moved back and up, the ones it starts blending later (aiming,
    // building...) as they come; a different distance puts them back first.
    void ApplyCamera(APlayerController* PlayerController, float Back, float Up)
    {
        if (std::fabs(Back - AppliedBack) > 1.0f || std::fabs(Up - AppliedUp) > 1.0f)
        {
            RestoreCamera();
            AppliedBack = Back;
            AppliedUp = Up;
        }
        UObject* CameraManager = reinterpret_cast<UObject*>(static_cast<APlayerCameraManager*>(PlayerController->PlayerCameraManager));
        if ((Back <= 0.0f && Up <= 0.0f) || !IsA(CameraManager, AFortPlayerCameraBaseStack::StaticClass()) || !AFortPlayerCameraBaseStack::HasCameraBlendStack() || !FActiveFortCamera::StaticStruct())
        {
            return;
        }
        TStructArray<FActiveFortCamera>& Stack = static_cast<AFortPlayerCameraBaseStack*>(CameraManager)->CameraBlendStack;
        for (int32_t Index = 0; Index < Stack.Num(); ++Index)
        {
            UObject* Mode = Stack[Index].Camera;
            const bool bSeen = std::any_of(CameraAdjustments.begin(), CameraAdjustments.end(), [Mode](const FCameraAdjustment& Adjustment) { return Adjustment.Mode.Get() == Mode; });
            if (Mode && !bSeen)
            {
                AdjustCameraMode(Mode, Back, Up);
            }
        }
    }

    // The camera pulls back by twice what the disguise is bigger than the player (its largest half size over the
    // player's half height) and up by one and a half times what it is taller, so a Smasher or a toilet at scale 5 fits
    // in the view and its top doesn't fill it. Measured again at every scan (#154): a disguise that arrives before its
    // mesh or its scale has (a character's parts come later) measured as small as its capsule, and stayed so.
    void MeasureOwnDisguise(bool bFirst)
    {
        AActor* Actor = Own.Actor.Get();
        APawn* Pawn = Own.Pawn.Get();
        if (!Actor || !Pawn)
        {
            return;
        }
        FVector Origin;
        FVector Extent;
        Actor->GetActorBounds(false, Origin, Extent);
        const float Height = std::max(Extent.Z, Own.PuppetHalfHeight);
        const float PlayerHalfHeight = std::max(HalfHeightOf(Pawn), 1.0f);
        const float Size = std::max({ Extent.X, Extent.Y, Height });
        const float Back = std::clamp(2.0f * (Size - PlayerHalfHeight), 0.0f, 3000.0f);
        const float Up = std::clamp(1.5f * (Height - PlayerHalfHeight), 0.0f, 1000.0f);
        if (!bFirst && std::fabs(Back - Own.Back) <= 10.0f && std::fabs(Up - Own.Up) <= 10.0f)
        {
            return;
        }
        Own.Back = Back;
        Own.Up = Up;
        UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: you are %s (%s, half size %.0f x %.0f x %.0f): the camera pulls back %.0f and up %.0f (\"cheat disguiseview %s\")"),
            *Actor->GetName(), Own.bPuppet ? TEXT("a puppet that follows your pawn here") : TEXT("a prop on your pawn"), Extent.X, Extent.Y, Height, Own.Back, Own.Up, DisguiseViewName(DisguiseView));
    }

    // The player's own disguise changed (or ended): the last one's view is undone, the new one's measured.
    void SetOwnDisguise(AActor* Actor, APawn* Pawn)
    {
        RestoreCamera();
        if (AActor* Previous = Own.Actor.Get(); Previous && Own.bHiddenHere)
        {
            Previous->SetActorHiddenInGame(false);
        }
        Own = FOwnDisguise();
        if (!Actor || !Pawn)
        {
            return;
        }

        Own.Actor = TWeakObjectPtr<AActor>(Actor);
        Own.Pawn = TWeakObjectPtr<APawn>(Pawn);
        const ENetRole Role = Actor->Role;
        Own.bPuppet = IsA(Actor, APawn::StaticClass()) && Role == ROLE_AutonomousProxy;
        Own.PuppetHalfHeight = Own.bPuppet ? HalfHeightOf(Actor) : 0.0f;
        if (IsA(Actor, ACharacter::StaticClass()) && !Own.bPuppet)
        {
            // A server without #137 sends this client the puppet's movement: it is left to it, as before.
            UE_LOG(LogFortDisguise, Warning, TEXT("DisguiseView: %s is role %d here, not this player's autonomous proxy: it moves with the server's updates"), *Actor->GetName(), static_cast<int32_t>(Role));
        }

        // A prop's carrier (the engine's default pawn) shows its mesh to everyone but its owner, which is this player's
        // pawn (#154): this player saw no prop.
        if (IsA(Actor, ADefaultPawnType::StaticClass()) && ADefaultPawnType::HasMeshComponent())
        {
            if (UObject* MeshComponent = static_cast<ADefaultPawnType*>(Actor)->MeshComponent)
            {
                static_cast<UPrimitiveComponentFunctions*>(MeshComponent)->SetOwnerNoSee(false);
            }
        }
        MeasureOwnDisguise(true);
    }

    // A player pawn that is hidden: a disguised player's (the server hides it while the disguise shows).
    bool IsDisguisedPawn(const AActor* Actor)
    {
        return Actor && AFortPlayerPawn::StaticClass() && IsA(Actor, AFortPlayerPawn::StaticClass()) && Actor->bHidden;
    }

    // A parked pawn: a hidden player pawn the player's player pawn disguise owns.
    bool IsParkedPawn(const AActor* Actor)
    {
        AActor* Owner = Actor ? Actor->GetOwner() : nullptr;
        return IsDisguisedPawn(Actor) && Owner && Owner != Actor && IsA(Owner, APawn::StaticClass());
    }

    // A parked pawn played again (its player undisguised) collides again here.
    void UnparkCollision(AActor* LocalPawn)
    {
        std::erase_if(Parked, [LocalPawn](const TWeakObjectPtr<AActor>& Weak)
        {
            AActor* Pawn = Weak.Get();
            if (!Pawn)
            {
                return true;
            }
            if (Pawn != LocalPawn && IsParkedPawn(Pawn))
            {
                return false;
            }
            Pawn->SetActorEnableCollision(true);
            UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: %s is played again; it collides here again"), *Pawn->GetName());
            return true;
        });
    }

    // A disguise is a puppet or a prop carrier (pawns the server spawns owned by the player's hidden pawn), or a prop
    // attached to that pawn (its weapon, hidden too, isn't one). On every client it loses its collision: the server's
    // copy has none, so the pawns walking into it here, the player's own first, were stopped and pushed back by the
    // server, and the camera, which keeps out of whatever collides, was pushed into a big one. The player's own one is
    // shown the way they chose.
    void ScanDisguises(UWorld* World, APawn* LocalPawn)
    {
        std::erase_if(Quieted, [](const TWeakObjectPtr<AActor>& Actor) { return !Actor.IsValid(); });
        AActor* OwnFound = nullptr;
        const auto Consider = [&OwnFound, LocalPawn](AActor* Actor, AActor* Owner)
        {
            const bool bKnown = std::any_of(Quieted.begin(), Quieted.end(), [Actor](const TWeakObjectPtr<AActor>& Known) { return Known.Get() == Actor; });
            if (!bKnown)
            {
                Actor->SetActorEnableCollision(false);
                Quieted.emplace_back(Actor);
                UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: %s is %s's disguise; no collision here"), *Actor->GetName(), *Owner->GetName());
            }
            if (LocalPawn && Owner == static_cast<AActor*>(LocalPawn) && (!OwnFound || IsA(Actor, APawn::StaticClass())))
            {
                OwnFound = Actor;
            }
            // Another player's puppet: carried on their pawn here (FollowOtherPuppets).
            else if (Owner != static_cast<AActor*>(LocalPawn) && IsA(Actor, ACharacter::StaticClass()) && static_cast<ENetRole>(Actor->Role) == ROLE_SimulatedProxy
                && std::none_of(OtherPuppets.begin(), OtherPuppets.end(), [Actor](const FOtherPuppet& Known) { return Known.Puppet.Get() == Actor; }))
            {
                FOtherPuppet& Other = OtherPuppets.emplace_back();
                Other.Puppet = TWeakObjectPtr<AActor>(Actor);
                Other.Pawn = TWeakObjectPtr<APawn>(static_cast<APawn*>(Owner));
                Other.HalfHeight = HalfHeightOf(Actor);
            }
        };

        const TArray<AActor*> Pawns = UGameplayStatics::GetAllActorsOfClass(World, APawn::StaticClass());
        for (int32_t Index = 0; Index < Pawns.Num(); ++Index)
        {
            AActor* Pawn = Pawns[Index];
            AActor* Owner = Pawn ? Pawn->GetOwner() : nullptr;
            // The pawn a player pawn disguise parks follows it on the server, without collision there; here it stood in
            // the disguise's way.
            if (Pawn && Pawn != static_cast<AActor*>(LocalPawn) && IsParkedPawn(Pawn))
            {
                if (std::none_of(Parked.begin(), Parked.end(), [Pawn](const TWeakObjectPtr<AActor>& Known) { return Known.Get() == Pawn; }))
                {
                    Pawn->SetActorEnableCollision(false);
                    Parked.emplace_back(Pawn);
                    UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: %s waits parked under %s, its player's disguise; no collision here"), *Pawn->GetName(), *Owner->GetName());
                }
                continue;
            }
            if (Owner && Owner != Pawn && IsDisguisedPawn(Owner))
            {
                Consider(Pawn, Owner);
            }
            if (!IsDisguisedPawn(Pawn) || !AActorAttachments::StaticClass())
            {
                continue;
            }
            const TArray<AActor*> Attached = reinterpret_cast<AActorAttachments*>(Pawn)->GetAttachedActors();
            for (int32_t AttachedIndex = 0; AttachedIndex < Attached.Num(); ++AttachedIndex)
            {
                AActor* Child = Attached[AttachedIndex];
                if (Child && Child->GetOwner() == Pawn && !Child->bHidden && !IsA(Child, APawn::StaticClass()) && !IsA(Child, AFortWeapon::StaticClass()))
                {
                    Consider(Child, Pawn);
                }
            }
        }

        if (OwnFound != Own.Actor.Get())
        {
            SetOwnDisguise(OwnFound, OwnFound ? LocalPawn : nullptr);
        }
        else if (OwnFound)
        {
            MeasureOwnDisguise(false);
        }
    }

    // Another player's puppet rides on their hidden pawn, which this client moves smoothly, with collision, as it moves
    // any other player's (#154). A server with #154 attaches it to the pawn and sends that instead of its movement; this
    // client keeps it there: its character movement, which would move it on from the last update on its own (and,
    // without collision, through the floor), stops, and it gets the pawn's velocity and movement mode for its animation
    // Blueprint. A puppet the server doesn't attach is left to the server's updates, as before.
    void FollowOtherPuppets()
    {
        std::erase_if(OtherPuppets, [](const FOtherPuppet& Other) { return !Other.Puppet.IsValid() || !Other.Pawn.IsValid(); });
        for (FOtherPuppet& Other : OtherPuppets)
        {
            AActor* Puppet = Other.Puppet.Get();
            APawn* Pawn = Other.Pawn.Get();
            USceneComponent* Root = Puppet->GetRootComponent();
            if (!Root || !Pawn->GetRootComponent() || static_cast<USceneComponent*>(Root->AttachParent) != Pawn->GetRootComponent())
            {
                continue;
            }
            UCharacterMovementComponent* PuppetMovement = MovementOf(static_cast<APawn*>(Puppet));
            if (!Other.bMovementStopped && PuppetMovement)
            {
                Other.bMovementStopped = reinterpret_cast<UActorComponentFunctions*>(PuppetMovement)->SetComponentTickEnabled(false);
                UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: %s rides on %s here"), *Puppet->GetName(), *Pawn->GetName());
            }
            const FVector Relative(0.0f, 0.0f, Other.HalfHeight - HalfHeightOf(Pawn));
            if (FVector::DistSquared(static_cast<FVector>(Root->RelativeLocation), Relative) > 0.25f)
            {
                reinterpret_cast<USceneComponentFunctions*>(Root)->SetRelativeLocationAndRotation(Relative, FRotator());
            }
            UCharacterMovementComponent* PawnMovement = MovementOf(Pawn);
            if (PuppetMovement && PawnMovement)
            {
                PuppetMovement->Velocity = PawnMovement->Velocity;
                const uint8_t Mode = static_cast<uint8_t>(PawnMovement->MovementMode) == MOVE_Falling ? MOVE_Falling : MOVE_Walking;
                if (static_cast<uint8_t>(PuppetMovement->MovementMode) != Mode)
                {
                    PuppetMovement->SetMovementMode(Mode);
                }
            }
        }
    }

    // The puppet sits on the player's pawn: attached to it here, so it moves in the same frame the pawn does, its feet
    // on the pawn's, facing the way the player looks, at the pawn's velocity and falling when it falls (the puppet's
    // animations play from those). The server sends this client none of the puppet's movement to fight it.
    void FollowPawn(APlayerController* PlayerController, APawn* Pawn, AActor* Puppet)
    {
        UCharacterMovementComponent* PuppetMovement = MovementOf(static_cast<APawn*>(Puppet));
        if (!Own.bAttached)
        {
            if (PuppetMovement)
            {
                PuppetMovement->bRunPhysicsWithNoController = false;
            }
            Puppet->AttachToActor(Pawn, EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, EAttachmentRule::KeepWorld);
            Own.bAttached = true;
        }

        const float RelativeZ = Own.PuppetHalfHeight - HalfHeightOf(Pawn);
        const float RelativeYaw = NormalizeRotatorAxis(FacingYaw(PlayerController, Pawn) - Pawn->GetActorRotation().Yaw);
        if (USceneComponent* Root = Puppet->GetRootComponent())
        {
            reinterpret_cast<USceneComponentFunctions*>(Root)->SetRelativeLocationAndRotation(FVector(0.0f, 0.0f, RelativeZ), FRotator(0.0f, RelativeYaw, 0.0f));
        }

        UCharacterMovementComponent* OwnMovement = MovementOf(Pawn);
        if (PuppetMovement && OwnMovement)
        {
            PuppetMovement->Velocity = OwnMovement->Velocity;
            const uint8_t Mode = static_cast<uint8_t>(OwnMovement->MovementMode) == MOVE_Falling ? MOVE_Falling : MOVE_Walking;
            if (static_cast<uint8_t>(PuppetMovement->MovementMode) != Mode)
            {
                PuppetMovement->SetMovementMode(Mode);
            }
        }
    }

    void ShowOwnDisguise(APlayerController* PlayerController, AActor* Actor)
    {
        const bool bHide = DisguiseView == EDisguiseView::Hide;
        if (bHide != Own.bHiddenHere)
        {
            Actor->SetActorHiddenInGame(bHide);
            Own.bHiddenHere = bHide;
        }
        if (DisguiseView == EDisguiseView::Back)
        {
            ApplyCamera(PlayerController, ChosenBack.value_or(Own.Back), ChosenUp.value_or(Own.Up));
        }
        else if (!CameraAdjustments.empty())
        {
            RestoreCamera();
        }
    }

    APlayerController* LocalPlayerController(UWorld* World)
    {
        UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
        const TArray<ULocalPlayer*>* LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
        ULocalPlayer* Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
        return Player ? static_cast<APlayerController*>(Player->PlayerController) : nullptr;
    }

    // Every frame on the game thread, before the world ticks: a scan for disguises four times a second, and the player's
    // own one followed every frame.
    void TickDisguiseView()
    {
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        APlayerController* PlayerController = LocalPlayerController(World);
        APawn* LocalPawn = PlayerController ? PlayerController->GetPawn() : nullptr;
        // Only a client of a server: its own pawn is its autonomous proxy.
        if (LocalPawn && static_cast<ENetRole>(LocalPawn->Role) != ROLE_AutonomousProxy)
        {
            LocalPawn = nullptr;
        }

        // A parked pawn played again collides again, the player's own from its first frame.
        const ULONGLONG Now = GetTickCount64();
        AActor* Played = PlayerController ? static_cast<AActor*>(PlayerController->GetPawn()) : nullptr;
        const bool bOwnParked = Played && std::any_of(Parked.begin(), Parked.end(), [Played](const TWeakObjectPtr<AActor>& Known) { return Known.Get() == Played; });
        if (!Parked.empty() && (bOwnParked || Now >= NextScan))
        {
            UnparkCollision(Played);
        }

        // Without a pawn of its own too (dead, spectating), for the other players' disguises.
        if (World && PlayerController && static_cast<ENetRole>(PlayerController->Role) == ROLE_AutonomousProxy && Now >= NextScan)
        {
            NextScan = Now + ScanMs;
            ScanDisguises(World, LocalPawn);
        }
        if (!OtherPuppets.empty())
        {
            FollowOtherPuppets();
        }

        AActor* Actor = Own.Actor.Get();
        if (Actor && (!LocalPawn || Own.Pawn.Get() != LocalPawn))
        {
            SetOwnDisguise(nullptr, nullptr);
            Actor = nullptr;
        }
        else if (!Actor && (Own.Pawn.IsValid() || !CameraAdjustments.empty()))
        {
            SetOwnDisguise(nullptr, nullptr);
        }
        if (Actor)
        {
            if (Own.bPuppet)
            {
                FollowPawn(PlayerController, LocalPawn, Actor);
            }
            ShowOwnDisguise(PlayerController, Actor);
        }
        GameThread::Enqueue(&TickDisguiseView);
    }
}
#endif

void UFortCheatManager::StartDisguiseView()
{
#if !UE_SERVER
    static bool bStarted = false;
    if (!bStarted)
    {
        bStarted = GameThread::Enqueue(&TickDisguiseView);
    }
#endif
}

bool UFortCheatManager::RunDisguiseViewCommand(APlayerController* PlayerController, const FString& Msg)
{
#if !UE_SERVER
    const std::vector<std::wstring> Words = SplitWords(Msg.ToStdString());
    if (Words.empty() || Lower(Words[0]) != L"disguiseview")
    {
        return false;
    }

    const auto Number = [](const std::wstring& Word, float Max) -> std::optional<float>
    {
        wchar_t* End = nullptr;
        const float Value = std::wcstof(Word.c_str(), &End);
        if (!End || *End != L'\0' || End == Word.c_str() || !std::isfinite(Value) || Value < 0.0f || Value > Max)
        {
            return std::nullopt;
        }
        return Value;
    };

    std::optional<EDisguiseView> Chosen;
    std::optional<float> Back;
    std::optional<float> Up;
    bool bValid = false;
    if (Words.size() == 2)
    {
        const std::wstring Wanted = Lower(Words[1]);
        if (Wanted == L"back")
        {
            Chosen = EDisguiseView::Back;
            bValid = true;
        }
        else if (Wanted == L"hide")
        {
            Chosen = EDisguiseView::Hide;
            bValid = true;
        }
        else if (Wanted == L"normal")
        {
            Chosen = EDisguiseView::Normal;
            bValid = true;
        }
    }
    else if ((Words.size() == 3 || Words.size() == 4) && Lower(Words[1]) == L"back")
    {
        Chosen = EDisguiseView::Back;
        Back = Number(Words[2], MaxChosenBack);
        Up = Words.size() == 4 ? Number(Words[3], MaxChosenUp) : ChosenUp;
        bValid = Back && (Words.size() == 3 || Up);
    }
    FString Message;
    if (!bValid)
    {
        Message = FString::Printf(TEXT("Your disguise view is \"%s\". Usage: disguiseview <back [distance] [up] | hide | normal>: the camera pulls back for a disguise bigger than you (by distance 0-%.0f and up 0-%.0f if you give them, measured from its size if not), it is hidden from you (others still see it), or neither"),
            DisguiseViewName(DisguiseView), MaxChosenBack, MaxChosenUp);
    }
    else
    {
        DisguiseView = *Chosen;
        if (DisguiseView == EDisguiseView::Back)
        {
            ChosenBack = Back;
            ChosenUp = Back ? Up : std::nullopt;
        }
        Message = DisguiseView == EDisguiseView::Back && ChosenBack ? FString::Printf(TEXT("Your camera pulls back %.0f and up %.0f%s"), *ChosenBack, ChosenUp.value_or(Own.Up), ChosenUp ? TEXT("") : TEXT(" (up measured from your disguise)"))
                : DisguiseView == EDisguiseView::Back ? FString(TEXT("Your camera pulls back for a disguise bigger than you"))
                : DisguiseView == EDisguiseView::Hide ? FString(TEXT("Your disguise is hidden from you; everyone else still sees it"))
                                                      : FString(TEXT("You see your disguise as it is, from the usual distance"));
    }
    UE_LOG(LogFortDisguise, Log, TEXT("DisguiseView: %s"), *Message);
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
