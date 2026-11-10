#include "pch.h"
#include "Athena/AI/CoreAthenaBotController.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "Abilities/FortAbilitySet.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Abilities/GameplayAbility.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingSMActor.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Building/BuildingWall.h"
#include "Configuration.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "Customization/CustomCharacterPart.h"
#include "Finder.h"
#include "GameplayCueInterface.h"
#include "GameplayTagContainer.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/AssetManager.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Heroes/FortHeroType.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Player/FortCheatManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace
{
    // An asset the bots keep a pointer to across garbage collections and matches (an outfit, emote, weapon or class):
    // rooted, so the collector never frees it under Core's caches, the map reload after a match included. Never a
    // world's object: a rooted one would keep its world alive.
    template <class T>
    T* KeepLoaded(T* Asset)
    {
        UObject* Object = reinterpret_cast<UObject*>(Asset);
        if (Object && !Object->IsRooted())
        {
            Object->AddToRoot();
        }
        return Asset;
    }

    // The engine's online interface (UOnlineEngineInterface::Get(): its one OnlineEngineInterfaceImpl object).
    UObject* OnlineEngineInterface()
    {
        static FWeakObjectPtr Cached;
        if (UObject* Interface = Cached.Get())
        {
            return Interface;
        }
        static UClass* InterfaceClass = FindObject<UClass>(nullptr, L"/Script/Engine.OnlineEngineInterface");
        for (TObjectIterator<UObject> It; It && InterfaceClass; ++It)
        {
            if ((*It)->IsA(InterfaceClass))
            {
                Cached = FWeakObjectPtr(*It);
                return *It;
            }
        }
        return nullptr;
    }

    // A player's id, which its connection brings and its login gives its player state (AGameModeBase::InitNewPlayer's
    // SetUniqueId): made the way a connection's replicated id is, by the online interface's CreateUniquePlayerId. A
    // client keeps the names of the other players by their ids (AFortPlayerState::SetPlayerName, 3.1 0xEAC480, files the
    // name under UniqueId; GetPlayerNameCustom, 0xE974E0, reads it back from there), so bots without one all shared one
    // name on clients. Id is never an account id (not hex), so match results and rewards pass bots by.
    bool AssignUniqueId(APlayerState* State, const std::wstring& Id)
    {
        FUniqueNetIdRepl& UniqueId = State->UniqueId;
        // FUniqueNetIdWrapper: its vtable, then the TSharedPtr<const FUniqueNetId> (object, reference controller).
        void** SharedPtr = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(&UniqueId) + sizeof(void*));
        UObject* Interface = SharedPtr[0] ? nullptr : OnlineEngineInterface();
        if (!Interface || !Finder::ResolveCreateUniquePlayerIdLive(*reinterpret_cast<void* const* const*>(Interface)))
        {
            return false;
        }
        void* Made[2] = {};
        const FString IdString(Id.c_str());
        // UE 4.20's Type (NAME_None: the default online subsystem, whose ids the players have); 4.19's takes Str alone and
        // leaves r9 be.
        const FName Type = NAME_None;
        CallVirtual<void**>(Interface, Natives::UOnlineEngineInterface_CreateUniquePlayerIdIndex, Made, &IdString, Type);
        if (!Made[0])
        {
            return false;
        }
        // The new id's one reference moves into the player state's.
        SharedPtr[0] = Made[0];
        SharedPtr[1] = Made[1];
        return UniqueId.IsValid();
    }

    // What a trap's trigger filter (FFortTargetFilter, 3.1 0x842D20) reads of a bot's pawn, once a match: its team
    // through the game's own team lookup (UFortKismetLibrary::GetActorTeam; friends are passed by: AFortGameStateZone's
    // 0xB880E0 compares the teams), and whether it can be damaged (the Damageable filter wants bCanBeDamaged).
    void LogTrapTargeting(AFortPlayerPawnAthena* Pawn)
    {
        static int32_t Logged = 0;
        static UFunction* GetActorTeam = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortKismetLibrary:GetActorTeam");
        static UClass* Library = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortKismetLibrary");
        if (Logged >= 2 || !Pawn || !GetActorTeam || !Library)
        {
            return;
        }
        ++Logged;
        Reflection::FParams Params(GetActorTeam, true);
        uint8_t Team = 0xFF;
        if (Params.Set(L"Actor", reinterpret_cast<AActor*>(Pawn)) && Params.Call(Library->GetDefaultObject()))
        {
            Team = Params.Take<uint8_t>(L"ReturnValue");
        }
        APlayerState* State = static_cast<APlayerState*>(Pawn->PlayerState);
        AFortPlayerStateAthena* AthenaState = State ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(State)) : nullptr;
        Log::Info(L"Battle Royale bots: {}'s team for the game (traps' filter): {}, its player state's {}, can be damaged {}", reinterpret_cast<UObject*>(Pawn)->GetName().ToStdString(), static_cast<int32_t>(Team),
            AthenaState ? static_cast<int32_t>(static_cast<uint8_t>(AthenaState->TeamIndex)) : -1, static_cast<bool>(Pawn->bCanBeDamaged));
    }

    // -CoreBotHotDrop (testing): everyone lands around the first bot's spot, one per match.
    FVector HotDrop;
    bool bHasHotDrop = false;
    // The fire cue each weapon ability sends (the abilities are the match's).
    std::map<const UObject*, FName> CueTags;

    // AFortWeaponRanged::bPersistentFireFX (its ShouldUsePersistentFireFX, 3.3 0xFE1490, reads it): the weapon's fire
    // effects loop while its fire cue is active instead of playing once per executed cue.
    bool UsesPersistentFireFX(AFortWeapon* Weapon)
    {
        static UClass* RangedClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortWeaponRanged");
        static UBoolProperty* PersistentProperty = RangedClass ? static_cast<UBoolProperty*>(RangedClass->FindPropertyByName(FName(L"bPersistentFireFX", FNAME_Find))) : nullptr;
        UObject* Object = reinterpret_cast<UObject*>(Weapon);
        return PersistentProperty && Object && Object->IsA(RangedClass) && PersistentProperty->GetPropertyValue_InContainer(Object);
    }

    // A weapon fire cue's FGameplayCueParameters as the fire ability fills them: the pawn as instigator, the weapon as
    // causer, its item definition as source, the shot's end as location.
    void FillWeaponCueParameters(uint8_t* CueParameters, AFortPlayerPawnAthena* Pawn, AFortWeapon* Weapon, const FVector& ImpactLocation)
    {
        static UScriptStruct* CueParametersStruct = FindObject<UScriptStruct>(nullptr, L"/Script/GameplayAbilities.GameplayCueParameters");
        if (!CueParameters || !CueParametersStruct || !Weapon)
        {
            return;
        }
        const auto Field = [CueParameters](const wchar_t* Name) -> uint8_t*
        {
            UProperty* Property = CueParametersStruct->FindPropertyByName(FName(Name, FNAME_Find));
            return Property ? CueParameters + Property->GetOffset_ForInternal() : nullptr;
        };
        if (uint8_t* Instigator = Field(L"Instigator"))
        {
            *reinterpret_cast<TWeakObjectPtr<UObject>*>(Instigator) = reinterpret_cast<UObject*>(Pawn);
        }
        if (uint8_t* Causer = Field(L"EffectCauser"))
        {
            *reinterpret_cast<TWeakObjectPtr<UObject>*>(Causer) = reinterpret_cast<UObject*>(Weapon);
        }
        if (uint8_t* Source = Field(L"SourceObject"))
        {
            *reinterpret_cast<TWeakObjectPtr<UObject>*>(Source) = reinterpret_cast<UObject*>(static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData));
        }
        if (uint8_t* Location = Field(L"Location"))
        {
            *reinterpret_cast<FVector*>(Location) = ImpactLocation;
        }
    }

    // EMovementMode: walking on the ground.
    constexpr uint8_t MoveWalking = 1;
    // EFortMovementStyle.
    constexpr uint8_t StyleRunning = 0;
    constexpr uint8_t StyleSprinting = 3;
    // EPathFollowingStatus::Idle.
    constexpr uint8_t PathIdle = 0;
    // A goal farther than this is reached a leg at a time (Core's 🟡: legs end inside the 40 m of navmesh tiles built
    // around each bot); the next leg is asked for once the bot is this close to its leg's end; a path ending this close
    // to its goal (plus the goal's acceptance) leaves a last straight step.
    constexpr float LegGoalDistance = 5000.0f;
    constexpr float NextLegDistance = 1000.0f;
    constexpr float LastStepDistance = 250.0f;

    // Whether the game's own fire works for a weapon (1), doesn't (0), or isn't known yet (absent): learnt from the
    // first shot with it, for every bot.
    std::map<UFortWeaponItemDefinition*, int32_t> WeaponFireWorks;

    // UMovementComponent::GetMaxSpeed (BlueprintPure): what the pawn's movement walks at now (Fortnite's movement
    // component reads the pawn's movement set for its movement style).
    float MaxSpeedOf(UCharacterMovementComponent* Movement)
    {
        static UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/Engine.MovementComponent:GetMaxSpeed");
        Reflection::FParams Params(Function, true);
        return Function && Movement && Params.Call(reinterpret_cast<UObject*>(Movement)) ? Params.Take<float>(L"ReturnValue") : -1.0f;
    }

    // A trace from Start to End hits something solid that isn't a pawn (the bot's own pawn is ignored).
    bool IsBlocked(AFortPlayerPawnAthena* Pawn, const FVector& Start, const FVector& End, UObject** OutHitActor = nullptr)
    {
        FVector Hit;
        UObject* HitActor = nullptr;
        UObject* HitComponent = nullptr;
        if (!UKismetSystemLibrary::LineTraceSingle(Pawn, Start, End, 0, false, true, Hit, HitActor, HitComponent))
        {
            return false;
        }
        if (OutHitActor)
        {
            *OutHitActor = HitActor;
        }
        return !(HitActor && Cast<APawn>(HitActor));
    }

    // The trace type (ETraceTypeQuery) of the game's weapon trace channel: the custom channel FCC_Trace_Weapon in
    // UCollisionProfile's DefaultChannelResponses (DefaultEngine.ini). The engine maps trace types to channels in channel
    // order, Visibility and Camera first (UCollisionProfile::LoadProfileConfig). Player-built pieces are what weapons hit
    // on that channel; a Visibility trace alone let the bots' shots through walls (the "wallbang" report on #233).
    // 0xFF when the build has no such channel.
    uint8_t WeaponTraceType()
    {
        static int32_t Cached = -1;
        if (Cached >= 0)
        {
            return static_cast<uint8_t>(Cached);
        }
        Cached = 0xFF;
        UClass* ProfileClass = FindObject<UClass>(nullptr, L"/Script/Engine.CollisionProfile");
        UObject* Profile = ProfileClass ? ProfileClass->GetDefaultObject() : nullptr;
        UProperty* Responses = ProfileClass ? ProfileClass->FindPropertyByName(FName(L"DefaultChannelResponses", FNAME_Find)) : nullptr;
        UScriptStruct* SetupStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.CustomChannelSetup");
        UProperty* ChannelProperty = SetupStruct ? SetupStruct->FindPropertyByName(FName(L"Channel", FNAME_Find)) : nullptr;
        UProperty* NameProperty = SetupStruct ? SetupStruct->FindPropertyByName(FName(L"Name", FNAME_Find)) : nullptr;
        UBoolProperty* TraceProperty = SetupStruct ? static_cast<UBoolProperty*>(SetupStruct->FindPropertyByName(FName(L"bTraceType", FNAME_Find))) : nullptr;
        if (!Profile || !Responses || !ChannelProperty || !NameProperty || !TraceProperty)
        {
            Log::Warning(L"Battle Royale bots: the collision profile's channels weren't found: bot shots trace the Visibility channel only");
            return static_cast<uint8_t>(Cached);
        }
        struct FRawArray
        {
            uint8_t* Data;
            int32_t Num;
            int32_t Max;
        };
        const FRawArray& Array = *reinterpret_cast<const FRawArray*>(reinterpret_cast<uint8_t*>(Profile) + Responses->GetOffset_ForInternal());
        const int32_t Size = SetupStruct->GetStructureSize();
        std::vector<std::pair<uint8_t, std::wstring>> TraceChannels;
        std::wstring Listed;
        for (int32_t Index = 0; Index < Array.Num; ++Index)
        {
            const uint8_t* Entry = Array.Data + static_cast<ptrdiff_t>(Index) * Size;
            const uint8_t Channel = *(Entry + ChannelProperty->GetOffset_ForInternal());
            const std::wstring Name = reinterpret_cast<const FName*>(Entry + NameProperty->GetOffset_ForInternal())->ToStdString();
            const bool bTrace = TraceProperty->GetPropertyValue_InContainer(Entry);
            Listed += std::format(L"{}{} {}{}", Listed.empty() ? L"" : L", ", Channel, Name, bTrace ? L" (trace)" : L"");
            if (bTrace)
            {
                TraceChannels.emplace_back(Channel, Name);
            }
        }
        std::sort(TraceChannels.begin(), TraceChannels.end());
        for (size_t Index = 0; Index < TraceChannels.size(); ++Index)
        {
            std::wstring Lower = TraceChannels[Index].second;
            std::transform(Lower.begin(), Lower.end(), Lower.begin(), ::towlower);
            // 3.1: FCC_Trace_Weapon (FCC_Trace_WeaponCapsule and FCC_Trace_WeaponMulti are the game's wider sweeps).
            if (Lower == L"fcc_trace_weapon" || Lower == L"weapon")
            {
                Cached = static_cast<int32_t>(2 + Index);
                Log::Info(L"Battle Royale bots: bot shots trace Visibility and {} (channel {}, trace type {}); the custom channels: {}", TraceChannels[Index].second, TraceChannels[Index].first, Cached, Listed);
                return static_cast<uint8_t>(Cached);
            }
        }
        Log::Warning(L"Battle Royale bots: no weapon trace channel among {}: bot shots trace the Visibility channel only", Listed);
        return static_cast<uint8_t>(Cached);
    }

    // Whether a shot (or a look) from Start reaches Target at End: the first blocking hit on the Visibility channel and on
    // the weapon channel is Target, or nothing. OutBlocker is what stopped it.
    bool HasClearShot(AFortPlayerPawnAthena* Pawn, const FVector& Start, const FVector& End, const AFortPawn* Target, UObject** OutBlocker = nullptr, FVector* OutImpact = nullptr)
    {
        const uint8_t WeaponChannel = WeaponTraceType();
        for (const uint8_t Channel : { static_cast<uint8_t>(0), WeaponChannel })
        {
            if (Channel == 0xFF)
            {
                continue;
            }
            FVector Hit;
            UObject* HitActor = nullptr;
            UObject* HitComponent = nullptr;
            if (UKismetSystemLibrary::LineTraceSingle(Pawn, Start, End, Channel, false, true, Hit, HitActor, HitComponent) && HitActor != reinterpret_cast<const UObject*>(Target))
            {
                if (OutBlocker)
                {
                    *OutBlocker = HitActor;
                }
                if (OutImpact)
                {
                    *OutImpact = Hit;
                }
                return false;
            }
        }
        return true;
    }

    // A float of a struct (a data table row) by its property's name.
    float* FloatOf(UStruct* Struct, void* Data, const wchar_t* Name)
    {
        UProperty* Property = Struct && Data ? Struct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == 4 && Property->GetClass()->GetName().ToStdString() == L"FloatProperty"
            ? reinterpret_cast<float*>(static_cast<uint8_t*>(Data) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // An attribute of an attribute set by its name.
    FGameplayAttributeData* AttributeOf(UObject* Set, const wchar_t* Name)
    {
        UProperty* Property = Set ? Set->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property ? reinterpret_cast<FGameplayAttributeData*>(reinterpret_cast<uint8_t*>(Set) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // The pawn's movement attributes from its hero's stats row (FFortPlayerPawnStats: the hero's LegacyStatHandle,
    // CharacterAttributesPlayer.PlayerLoadout_AthenaPlayer for Battle Royale's heroes), where they are still 0. A
    // player's are initialised from it when their hero is applied; nothing does that for an AI controller's pawn, so its
    // movement set's speeds stay 0, UFortMovementComp_Character::GetMaxSpeed (3.0 0xCAD090) returns 0 and the pawn
    // creeps at the engine's analog minimum (~105 u/s: phase 1's speed).
    int32_t InitializeMovementAttributes(AFortPlayerPawnAthena* Pawn, UObject* Hero)
    {
        UProperty* HandleProperty = Hero ? Hero->GetClass()->FindPropertyByName(FName(L"LegacyStatHandle", FNAME_Find)) : nullptr;
        FDataTableRowHandle* Handle = HandleProperty ? reinterpret_cast<FDataTableRowHandle*>(reinterpret_cast<uint8_t*>(Hero) + HandleProperty->GetOffset_ForInternal()) : nullptr;
        UDataTable* Table = Handle ? static_cast<UDataTable*>(Handle->DataTable) : nullptr;
        FName RowName = Handle ? static_cast<FName>(Handle->RowName) : FName();
        if (!Table)
        {
            static UDataTable* PlayerTable = KeepLoaded(LoadObject<UDataTable>(nullptr, L"/Game/Characters/Player/DataTables/CharacterAttributesPlayer.CharacterAttributesPlayer"));
            Table = PlayerTable;
            RowName = FName(L"PlayerLoadout_AthenaPlayer");
        }
        uint8_t* Row = Table ? Table->FindRowUnchecked(RowName) : nullptr;
        UScriptStruct* RowStruct = Row ? static_cast<UScriptStruct*>(Table->RowStruct) : nullptr;
        if (!Row || !RowStruct)
        {
            return 0;
        }
        int32_t Written = 0;
        const auto Init = [&](UObject* Set, const wchar_t* Attribute, const wchar_t* Stat)
        {
            FGameplayAttributeData* Data = AttributeOf(Set, Attribute);
            const float* Value = FloatOf(RowStruct, Row, Stat);
            if (Data && Value && *Value > 0.0f && Data->CurrentValue == 0.0f)
            {
                Data->BaseValue = *Value;
                Data->CurrentValue = *Value;
                ++Written;
            }
        };
        UObject* MovementSet = reinterpret_cast<UObject*>(static_cast<UFortMovementSet*>(Pawn->MovementSet));
        Init(MovementSet, L"WalkSpeed", L"SpeedWalk");
        Init(MovementSet, L"RunSpeed", L"SpeedRun");
        Init(MovementSet, L"SprintSpeed", L"SpeedSprint");
        Init(MovementSet, L"CrouchedRunSpeed", L"SpeedCrouchedRun");
        Init(MovementSet, L"CrouchedSprintSpeed", L"SpeedCrouchedSprint");
        Init(MovementSet, L"BackwardSpeedMultiplier", L"SpeedBackwardsMultiplier");
        if (FGameplayAttributeData* Multiplier = AttributeOf(MovementSet, L"SpeedMultiplier"); Multiplier && Multiplier->CurrentValue == 0.0f)
        {
            Multiplier->BaseValue = 1.0f;
            Multiplier->CurrentValue = 1.0f;
            ++Written;
        }
        UProperty* AdvancedProperty = reinterpret_cast<UObject*>(Pawn)->GetClass()->FindPropertyByName(FName(L"AdvancedMovementSet", FNAME_Find));
        UObject* Advanced = AdvancedProperty ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Pawn) + AdvancedProperty->GetOffset_ForInternal()) : nullptr;
        for (const wchar_t* Name : { L"GroundFriction", L"BrakingDecelerationWalking", L"MaxAcceleration", L"BrakingFrictionFactor", L"JumpZVelocity", L"MinAnalogWalkSpeed" })
        {
            Init(Advanced, Name, Name);
        }
        return Written;
    }

    FVector RotateYaw(const FVector& Direction, float Degrees)
    {
        const float Radians = Degrees * (3.1415926535897932f / 180.0f);
        const float C = std::cos(Radians);
        const float S = std::sin(Radians);
        return FVector(Direction.X * C - Direction.Y * S, Direction.X * S + Direction.Y * C, 0.0f);
    }

    FVector LocationOf(const AActor* Actor)
    {
        return Actor ? Actor->GetTransform().GetLocation() : FVector();
    }

    float Dist2D(const FVector& A, const FVector& B)
    {
        return std::sqrt((A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y));
    }

    FVector Direction2D(const FVector& From, const FVector& To)
    {
        const float Length = Dist2D(From, To);
        return Length > 1.0f ? FVector((To.X - From.X) / Length, (To.Y - From.Y) / Length, 0.0f) : FVector();
    }

    float RandomRange(float Min, float Max)
    {
        return Min + (Max - Min) * FMath::FRand();
    }

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    // The outfits bots wear: Battle Royale's default characters (8.30 reads the bot's from its customization loadout).
    UAthenaCharacterItemDefinition* RandomCharacter()
    {
        // The build's own outfits: what the asset manager lists as the primary asset type "AthenaCharacter" (the
        // locker's list), CID_*_Athena_Commando_*; loaded when a bot is dressed in one. The default outfits when the
        // list is empty.
        static std::vector<std::wstring> OutfitPaths;
        static bool bListed = false;
        if (!bListed)
        {
            bListed = true;
            UAssetManager* AssetManager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
            TArray<FAssetData> AssetDataList;
            if (AssetManager && AssetManager->GetPrimaryAssetDataList(FName(L"AthenaCharacter"), AssetDataList))
            {
                for (int32_t Index = 0; Index < AssetDataList.Num(); ++Index)
                {
                    const std::wstring Name = AssetDataList[Index].AssetName.ToString().ToStdString();
                    if (Name.starts_with(L"CID_") && Name.find(L"_Athena_Commando_") != std::wstring::npos)
                    {
                        OutfitPaths.push_back(AssetDataList[Index].ObjectPath.ToString().ToStdString());
                    }
                }
            }
            Log::Info(L"Battle Royale bots: {} outfits for the bots to wear", OutfitPaths.size());
        }
        for (int32_t Attempt = 0; Attempt < 3 && !OutfitPaths.empty(); ++Attempt)
        {
            const std::wstring& Path = OutfitPaths[static_cast<size_t>(FMath::Rand()) % OutfitPaths.size()];
            if (UAthenaCharacterItemDefinition* Character = LoadObject<UAthenaCharacterItemDefinition>(nullptr, Path.c_str()); Character && static_cast<UFortHeroType*>(Character->HeroDefinition))
            {
                return Character;
            }
        }
        static std::vector<UAthenaCharacterItemDefinition*> Characters;
        static bool bLoaded = false;
        if (!bLoaded)
        {
            bLoaded = true;
            for (const wchar_t* Path : { L"/Game/Athena/Items/Cosmetics/Characters/CID_001_Athena_Commando_F_Default.CID_001_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_002_Athena_Commando_F_Default.CID_002_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_003_Athena_Commando_F_Default.CID_003_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_004_Athena_Commando_F_Default.CID_004_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_005_Athena_Commando_M_Default.CID_005_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_006_Athena_Commando_M_Default.CID_006_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_007_Athena_Commando_M_Default.CID_007_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_008_Athena_Commando_M_Default.CID_008_Athena_Commando_M_Default" })
            {
                if (UAthenaCharacterItemDefinition* Character = KeepLoaded(LoadObject<UAthenaCharacterItemDefinition>(nullptr, Path)))
                {
                    Characters.push_back(Character);
                }
            }
            Log::Info(L"Battle Royale bots: {} default outfits for the bots", Characters.size());
        }
        return Characters.empty() ? nullptr : Characters[static_cast<size_t>(FMath::Rand()) % Characters.size()];
    }

    // A random cosmetic of the build's own: what the asset manager lists as the primary asset type AssetType (the
    // locker's lists: AthenaPickaxe, AthenaGlider, AthenaBackpack, AthenaSkyDiveContrail), loaded and kept. Null when
    // the build lists none.
    UObject* RandomCosmetic(const wchar_t* AssetType)
    {
        static std::map<std::wstring, std::vector<std::wstring>> Lists;
        auto [Found, bNew] = Lists.try_emplace(AssetType);
        std::vector<std::wstring>& Paths = Found->second;
        if (bNew)
        {
            UAssetManager* AssetManager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
            TArray<FAssetData> AssetDataList;
            if (AssetManager && AssetManager->GetPrimaryAssetDataList(FName(AssetType), AssetDataList))
            {
                for (int32_t Index = 0; Index < AssetDataList.Num(); ++Index)
                {
                    // Not the winners' umbrellas: a player only has one after a Victory Royale.
                    std::wstring Path = AssetDataList[Index].ObjectPath.ToString().ToStdString();
                    if (Path.find(L"Umbrella") == std::wstring::npos)
                    {
                        Paths.push_back(std::move(Path));
                    }
                }
            }
            Log::Info(L"Battle Royale bots: {} {} for the bots", Paths.size(), AssetType);
        }
        for (int32_t Attempt = 0; Attempt < 3 && !Paths.empty(); ++Attempt)
        {
            if (UObject* Cosmetic = LoadObject<UObject>(nullptr, Paths[static_cast<size_t>(FMath::Rand()) % Paths.size()].c_str()))
            {
                return KeepLoaded(Cosmetic);
            }
        }
        return nullptr;
    }

    UAthenaGliderItemDefinition* DefaultGlider()
    {
        static UAthenaGliderItemDefinition* Glider = KeepLoaded(LoadObject<UAthenaGliderItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Gliders/DefaultGlider.DefaultGlider"));
        return Glider;
    }

    UAthenaPickaxeItemDefinition* DefaultPickaxe()
    {
        static UAthenaPickaxeItemDefinition* Pickaxe = KeepLoaded(LoadObject<UAthenaPickaxeItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Pickaxes/DefaultPickaxe.DefaultPickaxe"));
        return Pickaxe;
    }

    // The players' pawn class (PlayerPawn_Athena_C, 8.30's UFortServerBotManagerAthena::BotPlayerPawnClass). The game
    // mode gives an AI controller the engine's DefaultPawn (GetDefaultPawnClassForController only answers players).
    UClass* BotPawnClass(AFortGameModeAthena* GameMode, AAIController* AIController)
    {
        static UClass* Cached = nullptr;
        if (Cached)
        {
            return Cached;
        }
        UClass* AthenaPawn = AFortPlayerPawnAthena::StaticClass();
        for (TObjectIterator<UClass> It; It && AthenaPawn; ++It)
        {
            UClass* Class = *It;
            if (Class->IsChildOf(AthenaPawn) && Class->GetName().ToStdString() == L"PlayerPawn_Athena_C")
            {
                Cached = Class;
                break;
            }
        }
        if (!Cached)
        {
            UClass* Fallback = GameMode->GetDefaultPawnClassForController(AIController);
            Cached = Fallback && AthenaPawn && Fallback->IsChildOf(AthenaPawn) ? Fallback : nullptr;
        }
        KeepLoaded(Cached);
        Log::Info(L"Battle Royale bots: the bots' pawn class is {}", NameOf(Cached));
        return Cached;
    }

    // A number from the weapon's stats row (FFortBaseWeaponStats / FFortRangedWeaponStats), by its property's name.
    bool ReadStat(UFortWeaponItemDefinition* Definition, const wchar_t* Name, float& OutValue)
    {
        const FDataTableRowHandle& StatHandle = Definition->WeaponStatHandle;
        UDataTable* Table = StatHandle.DataTable;
        uint8_t* Row = Table ? Table->FindRowUnchecked(StatHandle.RowName) : nullptr;
        UScriptStruct* RowStruct = Row ? static_cast<UScriptStruct*>(Table->RowStruct) : nullptr;
        UProperty* Property = RowStruct ? RowStruct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property || Property->ElementSize != 4)
        {
            return false;
        }
        const uint8_t* Value = Row + Property->GetOffset_ForInternal();
        const bool bInt = Property->GetClass()->GetName().ToStdString() == L"IntProperty";
        OutValue = bInt ? static_cast<float>(*reinterpret_cast<const int32_t*>(Value)) : *reinterpret_cast<const float*>(Value);
        return true;
    }

    // A one-byte value (an enum) of the weapon's stats row, or -1.
    int32_t ReadByteStat(UFortWeaponItemDefinition* Definition, const wchar_t* Name)
    {
        const FDataTableRowHandle& StatHandle = Definition->WeaponStatHandle;
        UDataTable* Table = StatHandle.DataTable;
        uint8_t* Row = Table ? Table->FindRowUnchecked(StatHandle.RowName) : nullptr;
        UScriptStruct* RowStruct = Row ? static_cast<UScriptStruct*>(Table->RowStruct) : nullptr;
        UProperty* Property = RowStruct ? RowStruct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == 1 ? static_cast<int32_t>(Row[Property->GetOffset_ForInternal()]) : -1;
    }

    // EFortWeaponReloadType::ReloadIndividualBullets.
    constexpr int32_t ReloadIndividualBullets = 1;

    ECoreAthenaBotWeaponType WeaponTypeOf(UFortWeaponItemDefinition* Definition, int32_t Pellets)
    {
        const std::wstring Name = Definition ? Definition->GetName().ToStdString() : std::wstring();
        const auto Has = [&Name](const wchar_t* Part) { return Name.find(Part) != std::wstring::npos; };
        if (Has(L"Shotgun") || Pellets > 1)
        {
            return ECoreAthenaBotWeaponType::Shotgun;
        }
        if (Has(L"Sniper"))
        {
            return ECoreAthenaBotWeaponType::Sniper;
        }
        if (Has(L"Assault"))
        {
            return ECoreAthenaBotWeaponType::AssaultRifle;
        }
        if (Has(L"Pistol_AutoHeavy") || Has(L"Pistol_Scavenger") || Has(L"SMG"))
        {
            return ECoreAthenaBotWeaponType::SMG;
        }
        if (Has(L"Pistol"))
        {
            return ECoreAthenaBotWeaponType::Pistol;
        }
        if (Has(L"Launcher"))
        {
            return ECoreAthenaBotWeaponType::Launcher;
        }
        return ECoreAthenaBotWeaponType::Other;
    }

    const wchar_t* WeaponTypeName(ECoreAthenaBotWeaponType Type)
    {
        switch (Type)
        {
        case ECoreAthenaBotWeaponType::Pistol: return L"pistol";
        case ECoreAthenaBotWeaponType::SMG: return L"SMG";
        case ECoreAthenaBotWeaponType::AssaultRifle: return L"assault rifle";
        case ECoreAthenaBotWeaponType::Shotgun: return L"shotgun";
        case ECoreAthenaBotWeaponType::Sniper: return L"sniper";
        case ECoreAthenaBotWeaponType::Launcher: return L"launcher";
        default: return L"gun";
        }
    }

    // Core's EquipMaxDistance per kind 🟡 (8.30's values are in its bot blueprints, which don't ship).
    float EquipMaxDistanceOf(ECoreAthenaBotWeaponType Type)
    {
        switch (Type)
        {
        case ECoreAthenaBotWeaponType::Shotgun: return 1200.0f;
        case ECoreAthenaBotWeaponType::SMG: return 2500.0f;
        case ECoreAthenaBotWeaponType::Pistol: return 3500.0f;
        case ECoreAthenaBotWeaponType::Launcher: return 4000.0f;
        case ECoreAthenaBotWeaponType::AssaultRifle: return 6000.0f;
        case ECoreAthenaBotWeaponType::Sniper: return 100000.0f;
        default: return 3000.0f;
        }
    }

    // How much a bot wants a kind of gun at a range band (0 close, under 10 m; 1 mid, under 35 m; 2 far) 🟡.
    float RangePreference(ECoreAthenaBotWeaponType Type, int32_t Band)
    {
        static const float Table[7][3] = {
            { 1.0f, 1.0f, 1.0f },  // Other
            { 3.0f, 3.5f, 3.0f },  // Pistol
            { 5.0f, 5.0f, 2.5f },  // SMG
            { 4.0f, 6.0f, 5.0f },  // AssaultRifle
            { 6.0f, 2.0f, 0.5f },  // Shotgun
            { 1.0f, 2.5f, 6.0f },  // Sniper
            { 0.5f, 2.0f, 2.0f },  // Launcher
        };
        return Table[static_cast<size_t>(Type)][static_cast<size_t>(std::clamp(Band, 0, 2))];
    }

    // How much a bot wants a kind of gun in its inventory at all (it carries one of each kind).
    float TypePriority(ECoreAthenaBotWeaponType Type)
    {
        switch (Type)
        {
        case ECoreAthenaBotWeaponType::AssaultRifle:
        case ECoreAthenaBotWeaponType::Shotgun: return 5.0f;
        case ECoreAthenaBotWeaponType::SMG: return 4.0f;
        case ECoreAthenaBotWeaponType::Sniper: return 3.0f;
        case ECoreAthenaBotWeaponType::Pistol: return 2.0f;
        default: return 1.0f;
        }
    }

    float DamagePerSecond(const FCoreAthenaBotWeapon& Weapon)
    {
        return Weapon.Damage * static_cast<float>(Weapon.Pellets) * std::max(Weapon.FiringRate, 0.1f);
    }

    // EFortRarity's Battle Royale names (Handmade, Ordinary, Sturdy, Quality, Fine: common to legendary).
    const wchar_t* RarityName(uint8_t Rarity)
    {
        static const wchar_t* const Names[] = { L"common", L"uncommon", L"rare", L"epic", L"legendary", L"mythic" };
        return Rarity < 6 ? Names[Rarity] : L"exotic";
    }

    // Whether Found is better than Held, a gun of the same kind (the community list: bots swap for higher rarity): a
    // rarer one unless it is clearly weaker, else one of the same rarity with a tenth more damage a second.
    bool IsBetterWeapon(const FCoreAthenaBotWeapon& Found, const FCoreAthenaBotWeapon& Held)
    {
        if (Found.Rarity != Held.Rarity)
        {
            return Found.Rarity > Held.Rarity && DamagePerSecond(Found) > DamagePerSecond(Held) * 0.85f;
        }
        return DamagePerSecond(Found) > DamagePerSecond(Held) * 1.1f;
    }

    const wchar_t* GoalName(ECoreAthenaBotGoal Goal)
    {
        switch (Goal)
        {
        case ECoreAthenaBotGoal::Loot: return L"loot";
        case ECoreAthenaBotGoal::Search: return L"search";
        case ECoreAthenaBotGoal::Storm: return L"storm";
        case ECoreAthenaBotGoal::Regroup: return L"regroup";
        case ECoreAthenaBotGoal::Harvest: return L"harvest";
        case ECoreAthenaBotGoal::Hunt: return L"hunt";
        case ECoreAthenaBotGoal::TravelToPOI: return L"travel";
        case ECoreAthenaBotGoal::Wander: return L"wander";
        default: return L"idle";
        }
    }

    // The bots' shots this match and how many hit, by the kind of gun and the range band (RangePreference's: under 10 m,
    // under 35 m, farther), logged every 100 shots, so the aim's tuning can be checked against a player's hit rate.
    struct FBotShotStats
    {
        int32_t Shots[7][3] = {};
        int32_t Hits[7][3] = {};
        int32_t Total = 0;
    };
    FBotShotStats ShotStats;

    void RecordShot(ECoreAthenaBotWeaponType Type, float Distance, bool bHit)
    {
        const size_t Kind = static_cast<size_t>(Type);
        const size_t Band = Distance < 1000.0f ? 0 : Distance < 3500.0f ? 1 : 2;
        ++ShotStats.Shots[Kind][Band];
        ShotStats.Hits[Kind][Band] += bHit ? 1 : 0;
        if (++ShotStats.Total % 100 != 0)
        {
            return;
        }
        std::wstring Rates;
        static const wchar_t* const BandNames[] = { L"close", L"mid", L"far" };
        for (size_t KindIndex = 0; KindIndex < 7; ++KindIndex)
        {
            for (size_t BandIndex = 0; BandIndex < 3; ++BandIndex)
            {
                if (const int32_t Shots = ShotStats.Shots[KindIndex][BandIndex]; Shots > 0)
                {
                    Rates += std::format(L"{}{} {} {:.0f}% ({}/{})", Rates.empty() ? L"" : L", ", WeaponTypeName(static_cast<ECoreAthenaBotWeaponType>(KindIndex)), BandNames[BandIndex],
                        100.0f * static_cast<float>(ShotStats.Hits[KindIndex][BandIndex]) / static_cast<float>(Shots), ShotStats.Hits[KindIndex][BandIndex], Shots);
                }
            }
        }
        Log::Info(L"Battle Royale bots: hit rates after {} shots: {}", ShotStats.Total, Rates);
    }

    // Battle Royale's epic rarity (EFortRarity::Quality): a bot with guns below it still searches chests for better ones.
    constexpr uint8_t EpicRarity = 3;

    // How often a bot takes on another bot it sees and that isn't after it: the admin panel's "Bot vs bot fight chance"
    // (30% by default), rolled again every 30 s per pair; doubled once the last few bots are left, so a match still ends.
    constexpr int32_t LateGameBotCount = 10;
    constexpr float BotEngageRollPeriod = 30.0f;

    // Giving a knocked (DBNO) enemy time before thirsting it (CoreV2 #233), so a teammate can revive: the base hold-off
    // after the knock; the longer hold-off while a squadmate is right there (a revive could be underway); how close a
    // standing squadmate counts as "right there"; and how far one can be and still be treated as a possible reviver
    // (beyond this, or with none, the downed enemy is fair game). The downed enemy stays deprioritised behind standing
    // ones the whole time, so the hold-off only matters when nothing else is worth shooting.
    constexpr float ThirstGrace = 8.0f;
    constexpr float ThirstReviveGrace = 20.0f;
    constexpr float ThirstReviveRange = 1500.0f;
    constexpr float ThirstNoReviverRange = 8000.0f;
    // How long a bot remembers an enemy that went out of sight (CoreV2 #349): 8.30's perception keeps a sight
    // stimulus until its max age, and the bot aims at and closes in on its last known position meanwhile (21.40's
    // GetLastKnownPosition). Core's 🟡: 5 s.
    constexpr float TargetMemoryAge = 5.0f;
    // A bot heads for a player within this distance (Core's 🟡: 100 m), at most this many bots for one player.
    constexpr float HuntRange = 10000.0f;
    constexpr int32_t MaxHuntersPerPlayer = 3;

    // A bot carries up to this many guns (a player's five quickbar slots, two kept for healing items).
    constexpr size_t MaxWeapons = 3;
    constexpr size_t MaxConsumableKinds = 2;

    void ReadWeaponStats(FCoreAthenaBotWeapon& Weapon)
    {
        float Value = 0.0f;
        if (ReadStat(Weapon.WeaponDefinition, L"DmgPB", Value) && Value > 0.0f)
        {
            Weapon.Damage = Value;
        }
        if (ReadStat(Weapon.WeaponDefinition, L"BulletsPerCartridge", Value) && Value >= 1.0f)
        {
            Weapon.Pellets = static_cast<int32_t>(Value);
        }
        if (ReadStat(Weapon.WeaponDefinition, L"FiringRate", Value) && Value > 0.0f)
        {
            Weapon.FiringRate = Value;
        }
        if (ReadStat(Weapon.WeaponDefinition, L"ClipSize", Value) && Value >= 1.0f)
        {
            Weapon.ClipSize = static_cast<int32_t>(Value);
        }
        if (ReadStat(Weapon.WeaponDefinition, L"ReloadTime", Value) && Value > 0.0f)
        {
            Weapon.ReloadTime = Value;
        }
        Weapon.bReloadIndividualBullets = ReadByteStat(Weapon.WeaponDefinition, L"ReloadType") == ReloadIndividualBullets;
        Weapon.Type = WeaponTypeOf(Weapon.WeaponDefinition, Weapon.Pellets);
        Weapon.EquipMaxDistance = EquipMaxDistanceOf(Weapon.Type);
    }

    // A number (an int or a float property) of an object, by its name, or Default.
    float NumberOf(const UObject* Object, const wchar_t* Name, float Default)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property || Property->ElementSize != 4)
        {
            return Default;
        }
        const uint8_t* Value = reinterpret_cast<const uint8_t*>(Object) + Property->GetOffset_ForInternal();
        const std::wstring Type = Property->GetClass()->GetName().ToStdString();
        return Type == L"IntProperty" ? static_cast<float>(*reinterpret_cast<const int32_t*>(Value)) : Type == L"FloatProperty" ? *reinterpret_cast<const float*>(Value) : Default;
    }

    // An object (a montage) of Object by its property's name, or of a struct property of it (Name.Inner), and a float
    // beside it in that struct.
    UObject* ObjectOf(const UObject* Object, const wchar_t* Name, const wchar_t* Inner = nullptr, float* OutRate = nullptr, const wchar_t* RateName = nullptr)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property)
        {
            return nullptr;
        }
        const uint8_t* Value = reinterpret_cast<const uint8_t*>(Object) + Property->GetOffset_ForInternal();
        if (Inner)
        {
            UScriptStruct* Struct = Property->GetClass()->GetName().ToStdString() == L"StructProperty" ? static_cast<UStructProperty*>(Property)->Struct : nullptr;
            UProperty* InnerProperty = Struct ? Struct->FindPropertyByName(FName(Inner, FNAME_Find)) : nullptr;
            UProperty* RateProperty = Struct && RateName ? Struct->FindPropertyByName(FName(RateName, FNAME_Find)) : nullptr;
            if (!InnerProperty)
            {
                return nullptr;
            }
            if (OutRate && RateProperty && RateProperty->ElementSize == 4)
            {
                *OutRate = *reinterpret_cast<const float*>(Value + RateProperty->GetOffset_ForInternal());
            }
            Value += InnerProperty->GetOffset_ForInternal();
        }
        return *reinterpret_cast<UObject* const*>(Value);
    }

    int32_t BulletsPerClip(AFortWeapon* Weapon)
    {
        UFunction* Function = Weapon ? AFortWeapon::FindStaticFunction_GetBulletsPerClip() : nullptr;
        Reflection::FParams Params(Function, true);
        return Function && Params.Call(Weapon) ? Params.Take<int32_t>(L"ReturnValue") : 0;
    }

    // A player's abilities from the game data's GenericPlayerAbilitySet (GAS_DefaultPlayer), which their hero's abilities
    // bring (UFortGlobals::ApplyHeroEffectsAndAbilities), that a bot needs too:
    // - the boogie bomb's dance (GA_DanceGrenade_Stun): the grenade activates it by its tag
    //   (Gameplay.Action.Player.DanceStunned, TryActivateAbilitiesByTag) on the pawns it hits;
    // - Battle Royale's DBNO (GAB_AthenaDBNO, on Event.Damage.DiedAthena) and getting up (GAB_AthenaDBNORevive, on
    //   Event.RevivedAthena): when a player's hit knocks a bot, the game downs it itself (AFortPlayerPawnAthena's DBNO
    //   handling sets bIsDBNO) and sends the event, and without the ability the pawn sat at 0 health, with no DBNO
    //   health and no bleed (#248).
    // The others of the set (death, interaction) are Core's for a bot.
    const std::vector<UClass*>& PlayerAbilityClasses()
    {
        static std::vector<UClass*> Found;
        static bool bSearched = false;
        if (bSearched)
        {
            return Found;
        }
        bSearched = true;
        UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
        UFortAbilitySet* Set = GameData ? GameData->GenericPlayerAbilitySet.LoadSynchronous() : nullptr;
        if (Set)
        {
            const TArray<UClass*>& Abilities = Set->GameplayAbilities;
            for (UClass* Ability : Abilities)
            {
                const std::wstring Name = Ability ? Ability->GetName().ToStdString() : std::wstring();
                if (Name.find(L"DanceGrenade_Stun") != std::wstring::npos || Name == L"GAB_AthenaDBNO_C" || Name == L"GAB_AthenaDBNORevive_C")
                {
                    Found.push_back(KeepLoaded(Ability));
                }
            }
        }
        // The DBNO abilities by their paths when the set hasn't them.
        for (const wchar_t* Path : { L"/Game/Abilities/NPC/Generic/GAB_AthenaDBNO.GAB_AthenaDBNO_C", L"/Game/Abilities/NPC/Generic/GAB_AthenaDBNORevive.GAB_AthenaDBNORevive_C" })
        {
            UClass* Ability = LoadObject<UClass>(nullptr, Path);
            if (Ability && Ability->IsChildOf(UGameplayAbility::StaticClass()) && std::find(Found.begin(), Found.end(), Ability) == Found.end())
            {
                Found.push_back(KeepLoaded(Ability));
            }
        }
        std::wstring Names;
        for (UClass* Ability : Found)
        {
            Names += (Names.empty() ? L"" : L", ") + NameOf(Ability);
        }
        Log::Info(L"Battle Royale bots: a player's abilities the bots get: {}", Names.empty() ? std::wstring(L"none") : Names);
        return Found;
    }

    // One of those, by a part of its name (DanceGrenade_Stun, GAB_AthenaDBNO_C).
    UClass* PlayerAbilityClass(const wchar_t* Name)
    {
        for (UClass* Ability : PlayerAbilityClasses())
        {
            if (Ability->GetName().ToStdString().find(Name) != std::wstring::npos)
            {
                return Ability;
            }
        }
        return nullptr;
    }

    UClass* DanceStunAbilityClass()
    {
        static UClass* Found = PlayerAbilityClass(L"DanceGrenade_Stun");
        return Found;
    }

    void GiveAbility(UAbilitySystemComponent* AbilitySystem, UClass* AbilityClass)
    {
        if (!AbilitySystem || !AbilityClass || AbilitySystem->FindAbilitySpecFromClass(AbilityClass))
        {
            return;
        }
        UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
        std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
        SpecStruct->InitializeStruct(SpecStorage.data());
        FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
        FGameplayAbilitySpecHandle NewHandle;
        NewHandle.GenerateNewHandle();
        Spec.Handle = NewHandle;
        Spec.Ability = static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject());
        Spec.Level = 1;
        Spec.InputID = INDEX_NONE;
        AbilitySystem->GiveAbility(Spec);
        SpecStruct->DestroyStruct(SpecStorage.data());
    }

    void GivePlayerAbilities(UAbilitySystemComponent* AbilitySystem)
    {
        for (UClass* AbilityClass : PlayerAbilityClasses())
        {
            GiveAbility(AbilitySystem, AbilityClass);
        }
    }

    // The hit in a damage effect's context, as a weapon ability's targeting adds it: a blocking FHitResult from the
    // shooter to the target (UAbilitySystemBlueprintLibrary::EffectContextAddHitResult) and the context's origin
    // (EffectContextSetOrigin). The victim's damage cue (AFortPawn::GameplayCue_Damage) gets its instigator and hit from
    // the context; without them a player hit by a bot saw no hit direction (#233).
    void AddHitToContext(void* ContextHandle, UObject* TargetActor, const FVector& From)
    {
        static UFunction* AddHitResult = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:EffectContextAddHitResult");
        static UFunction* SetOrigin = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:EffectContextSetOrigin");
        static UClass* Library = FindObject<UClass>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary");
        UObject* LibraryDefault = Library ? Library->GetDefaultObject() : nullptr;
        UProperty* HitProperty = AddHitResult ? AddHitResult->FindPropertyByName(FName(L"HitResult", FNAME_Find)) : nullptr;
        if (!LibraryDefault || !HitProperty || !TargetActor)
        {
            return;
        }
        const FVector To = LocationOf(reinterpret_cast<AActor*>(TargetActor));
        FVector Normal = From - To;
        const float Length = Normal.Size();
        Normal = Length > 1.0f ? Normal * (1.0f / Length) : FVector(0.0f, 0.0f, 1.0f);
        Reflection::FParams Hit(AddHitResult, true);
        if (Hit.CopyStruct(L"EffectContext", ContextHandle))
        {
            UScriptStruct* HitStruct = static_cast<UStructProperty*>(HitProperty)->Struct;
            uint8_t* Data = static_cast<uint8_t*>(Hit.GetAddress(L"HitResult"));
            const auto Member = [&](const wchar_t* Name) -> UProperty* { return HitStruct->FindPropertyByName(FName(Name, FNAME_Find)); };
            const auto SetVector = [&](const wchar_t* Name, const FVector& Value)
            {
                if (UProperty* Property = Member(Name))
                {
                    *reinterpret_cast<FVector*>(Data + Property->GetOffset_ForInternal()) = Value;
                }
            };
            if (UBoolProperty* Blocking = static_cast<UBoolProperty*>(Member(L"bBlockingHit")))
            {
                Blocking->SetPropertyValue(Data + Blocking->GetOffset_ForInternal(), true);
            }
            SetVector(L"Location", To);
            SetVector(L"ImpactPoint", To);
            SetVector(L"Normal", Normal);
            SetVector(L"ImpactNormal", Normal);
            SetVector(L"TraceStart", From);
            SetVector(L"TraceEnd", To);
            if (UProperty* Distance = Member(L"Distance"))
            {
                *reinterpret_cast<float*>(Data + Distance->GetOffset_ForInternal()) = Length;
            }
            if (UProperty* Actor = Member(L"Actor"))
            {
                *reinterpret_cast<FWeakObjectPtr*>(Data + Actor->GetOffset_ForInternal()) = TargetActor;
            }
            Hit.Set(L"bReset", true);
            Hit.Call(LibraryDefault);
        }
        if (SetOrigin)
        {
            Reflection::FParams Origin(SetOrigin, true);
            if (Origin.CopyStruct(L"EffectContext", ContextHandle) && Origin.Set(L"Origin", From))
            {
                Origin.Call(LibraryDefault);
            }
        }
        static int32_t Logged = 0;
        static UFunction* GetInstigator = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:EffectContextGetInstigatorActor");
        static UFunction* HasHitResult = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:EffectContextHasHitResult");
        if (Logged < 2 && GetInstigator && HasHitResult)
        {
            ++Logged;
            Reflection::FParams Instigator(GetInstigator, true);
            Reflection::FParams HasHit(HasHitResult, true);
            const bool bInstigator = Instigator.CopyStruct(L"EffectContext", ContextHandle) && Instigator.Call(LibraryDefault);
            const bool bHasHit = HasHit.CopyStruct(L"EffectContext", ContextHandle) && HasHit.Call(LibraryDefault) && HasHit.Take<bool>(L"ReturnValue");
            Log::Info(L"Battle Royale bots: a bot's hit carries instigator {} and {} hit result, from ({:.0f}, {:.0f}, {:.0f})", bInstigator ? NameOf(reinterpret_cast<UObject*>(Instigator.Take<AActor*>(L"ReturnValue"))) : std::wstring(L"none"),
                bHasHit ? L"a" : L"no", From.X, From.Y, From.Z);
        }
    }

    // Who killed the pawn: the newest of its damagers, through the weapon to its owner, as the death report names the
    // killer (8.30's bots learn it from the pawn's death delegate, which Core can't bind).
    AFortPlayerStateAthena* KillerOf(AFortPawn* Pawn)
    {
        TStructArray<FDamagerInfo>& Damagers = Pawn->Damagers;
        for (int32_t Index = Damagers.Num() - 1; Index >= 0; --Index)
        {
            AActor* Causer = Damagers[Index].DamageCauser;
            if (AController* CauserController = Causer ? Cast<AController>(reinterpret_cast<UObject*>(Causer)) : nullptr)
            {
                // Not its own controller (the storm's damage names the victim's).
                AFortPlayerStateAthena* State = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(CauserController->PlayerState)));
                if (State && reinterpret_cast<UObject*>(State) != reinterpret_cast<UObject*>(static_cast<APlayerState*>(Pawn->PlayerState)))
                {
                    return State;
                }
                continue;
            }
            AActor* Owner = Causer;
            for (int32_t Depth = 0; Owner && Depth < 3 && !Cast<APawn>(reinterpret_cast<UObject*>(Owner)); ++Depth)
            {
                Owner = Owner->GetOwner();
            }
            APawn* Killer = Owner ? Cast<APawn>(reinterpret_cast<UObject*>(Owner)) : nullptr;
            if (Killer && Killer != reinterpret_cast<APawn*>(Pawn))
            {
                return Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Killer->PlayerState)));
            }
        }
        return nullptr;
    }
}

FCoreAthenaBotController::FCoreAthenaBotController(FCoreAthenaBotManager* InManager, AAIController* InController, int32_t InBotIndex, uint8_t InTeam)
    : Manager(InManager)
    , Controller(InController)
    , BotIndex(InBotIndex)
    , Team(InTeam)
{
}

void FCoreAthenaBotController::ResetForNewMatch()
{
    HotDrop = FVector();
    bHasHotDrop = false;
    CueTags.clear();
    WeaponFireWorks.clear();
    ShotStats = FBotShotStats();
}

AAIController* FCoreAthenaBotController::GetController() const
{
    return Controller.Get();
}

AFortPlayerPawnAthena* FCoreAthenaBotController::GetPawn() const
{
    return PlayerBotPawn.Get();
}

AFortPlayerStateAthena* FCoreAthenaBotController::GetPlayerState() const
{
    AAIController* AIController = Controller.Get();
    return AIController ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(AIController->PlayerState))) : nullptr;
}

void FCoreAthenaBotController::AssignDisplayName(const std::wstring& Name)
{
    // 21.40's AFortAthenaAIBotController::AssignDisplayName. The player state is the game's own: it's a bot
    // (APlayerState::bIsABot) on its team, and its name shows in the kill feed and above its head.
    BotName = Name;
    AFortPlayerStateAthena* State = GetPlayerState();
    if (!State)
    {
        return;
    }
    // The ids first, as AGameSession::RegisterPlayer gives a joining player's state its PlayerId (GetNextPlayerID) and
    // its UniqueId: a client files the name under the UniqueId, and lists a squadmate on its squad bar only with a
    // PlayerId (3.5.1's OnRep_SquadId, 0xA429D0: the bots on a player's team were missing there, CoreV2 #264).
    if (APlayerState::HasPlayerId() && State->PlayerId == 0
        && Finder::ResolveNextPlayerIDLive(reinterpret_cast<const uint8_t*>(&State->PlayerId) - reinterpret_cast<const uint8_t*>(State)))
    {
        State->PlayerId = (*Natives::AGameSession_NextPlayerID)++;
    }
    static int32_t LoggedIds = 0;
    const bool bHasId = AssignUniqueId(State, std::format(L"CoreBot{}{}", BotIndex, Name));
    if (LoggedIds++ < 2 || !bHasId)
    {
        Log::Info(L"Battle Royale bots: {}'s id: {} (PlayerId {})", Name, State->UniqueId.ToString().ToStdString(), APlayerState::HasPlayerId() ? static_cast<int32_t>(State->PlayerId) : -1);
    }
    State->PlayerName = FString(Name.c_str());
    if (APlayerState::HasPlayerNamePrivate())
    {
        // From 1.9 the game keeps the name obfuscated in PlayerNamePrivate (APlayerState::SetPlayerName) and
        // GetPlayerName (3.0 0x2259860, the player state's custom name) gives it back by shifting each character up by
        // ((Len % 4 * 3 % 8 + 1 + Index) * 3 % 8), so the stored name is shifted down by as much (a plain one comes back
        // garbled: phase 1's kill feed showed "Hlr|fv" for Clover). Checked against GetPlayerName, plain if that differs.
        State->PlayerNamePrivate = FString(Name.c_str());
        if (State->GetPlayerName().ToStdString() != Name)
        {
            std::wstring Encoded = Name;
            const int32_t Length = static_cast<int32_t>(Encoded.size());
            for (int32_t Index = 0; Index < Length; ++Index)
            {
                Encoded[static_cast<size_t>(Index)] = static_cast<wchar_t>(Encoded[static_cast<size_t>(Index)] - (Length % 4 * 3 % 8 + 1 + Index) * 3 % 8);
            }
            State->PlayerNamePrivate = FString(Encoded.c_str());
            if (State->GetPlayerName().ToStdString() != Name)
            {
                Log::Warning(L"Battle Royale bots: {}'s name reads back as {}", Name, State->GetPlayerName().ToStdString());
                State->PlayerNamePrivate = FString(Name.c_str());
            }
        }
    }
    // Its team is the manager's to give, as the game mode gives a player theirs (FCoreAthenaBotManager::PlaceBotOnTeam).
    State->bIsABot = true;
    State->ForceNetUpdate();
}

void FCoreAthenaBotController::InitializeSkills(int32_t Level)
{
    // 21.40's AFortAthenaAIBotController::InitializeSkills.
    AIBotSkills = FCoreAthenaBotManager::SkillsForLevel(Level);
}

bool FCoreAthenaBotController::SpawnPawnAt(const FVector& Location, float Yaw, bool bSkydive)
{
#if WITH_SERVER_CODE
    AAIController* AIController = Controller.Get();
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    UClass* PawnClass = AIController && World ? BotPawnClass(GameMode, AIController) : nullptr;
    if (!PawnClass)
    {
        Log::Error(L"Battle Royale bots: no pawn class for {}", BotName);
        return false;
    }
    FTransform SpawnTransform;
    const float HalfYaw = Yaw * (3.1415926535897932f / 360.0f);
    SpawnTransform.Rotation.Z = std::sin(HalfYaw);
    SpawnTransform.Rotation.W = std::cos(HalfYaw);
    SpawnTransform.SetLocation(Location);
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AFortPlayerPawnAthena* Pawn = Cast<AFortPlayerPawnAthena>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
    if (!Pawn)
    {
        Log::Error(L"Battle Royale bots: couldn't spawn {}'s {}", BotName, PawnClass->GetName().ToStdString());
        return false;
    }

    // The bot's look (8.30: its customization loadout): a default outfit, glider and pickaxe, on the pawn (its loadout
    // replicates: OnRep_CustomizationLoadout) and the player state (the outfit's hero gives it its character parts).
    UObject* HeroObject = nullptr;
    // The outfit's own parts, as SetHeroType gives them to the player state.
    std::vector<UCustomCharacterPart*> HeroParts;
    if (Pawn->HasCustomizationLoadout())
    {
        FFortAthenaLoadout& Loadout = Pawn->CustomizationLoadout;
        // A whole locker's loadout, as a player brings: an outfit and a random glider, pickaxe, contrail and back bling
        // of the build's, chosen once for the match.
        UClass* GliderClass = UAthenaGliderItemDefinition::StaticClass();
        UClass* PickaxeClass = UAthenaPickaxeItemDefinition::StaticClass();
        if (!bLoadoutChosen)
        {
            bLoadoutChosen = true;
            LoadoutCharacter = reinterpret_cast<UObject*>(RandomCharacter());
            UObject* Glider = FFortAthenaLoadout::HasGlider() ? RandomCosmetic(L"AthenaGlider") : nullptr;
            LoadoutGlider = Glider && GliderClass && Glider->IsA(GliderClass) ? Glider : reinterpret_cast<UObject*>(DefaultGlider());
            UObject* RandomPickaxe = RandomCosmetic(L"AthenaPickaxe");
            UAthenaPickaxeItemDefinition* Pickaxe = RandomPickaxe && PickaxeClass && RandomPickaxe->IsA(PickaxeClass) ? reinterpret_cast<UAthenaPickaxeItemDefinition*>(RandomPickaxe) : nullptr;
            LoadoutPickaxe = Pickaxe && static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) ? Pickaxe : nullptr;
            LoadoutContrail = FFortAthenaLoadout::HasSkyDiveContrail() ? RandomCosmetic(L"AthenaSkyDiveContrail") : nullptr;
            BackpackDefinition = FFortAthenaLoadout::HasBackpack() && FMath::FRand() < 0.6f ? RandomCosmetic(L"AthenaBackpack") : nullptr;
        }
        UAthenaCharacterItemDefinition* Character = reinterpret_cast<UAthenaCharacterItemDefinition*>(LoadoutCharacter);
        HeroObject = Character ? reinterpret_cast<UObject*>(static_cast<UFortHeroType*>(Character->HeroDefinition)) : nullptr;
        Loadout.Character = Character;
        if (FFortAthenaLoadout::HasGlider())
        {
            Loadout.Glider = reinterpret_cast<UAthenaGliderItemDefinition*>(LoadoutGlider);
        }
        Loadout.Pickaxe = LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe();
        if (FFortAthenaLoadout::HasSkyDiveContrail())
        {
            Loadout.SkyDiveContrail = LoadoutContrail;
        }
        if (FFortAthenaLoadout::HasBackpack())
        {
            Loadout.Backpack = BackpackDefinition;
        }
        AFortPlayerStateAthena* State = GetPlayerState();
        UFortHeroType* HeroType = Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr;
        if (State && HeroType && AFortPlayerState::CanSetHeroType())
        {
            if (UFortHero* Hero = UFortCheatManager::CreateLookHero(HeroType))
            {
                State->SetHeroType(Hero);
                const EFortCustomPartType NumTypes = EFortCustomPartType::GetNumTypes();
                for (uint8_t PartType = 0; NumTypes != Reflection::InvalidEnumValue && PartType < NumTypes; ++PartType)
                {
                    HeroParts.push_back(State->GetCharacterPart(EFortCustomPartType{ PartType }));
                }
            }
        }
    }

    AIController->Possess(Pawn);
    PlayerBotPawn = Pawn;
    MovementStyle = 0xFF;
    bPathFollowing = false;
    bRegisteredInvoker = false;
    bLoggedMovement = false;
    bWeaponFireActive = false;
    HealingItem = nullptr;
    ReloadIndex = -1;
    ReloadUntil = 0.0f;
    LastHealthShield = -1.0f;
    LastHurtTime = -100.0f;
    // A player pawn takes its ability system (its player state's) in OnRep_PlayerState (AFortPlayerPawn's, 3.0
    // 0xDF4E10), which on the server only AFortPlayerController's possession runs: an AI controller's doesn't, and a
    // weapon equipped without one crashes giving its abilities.
    if (!static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent))
    {
        Pawn->OnRep_PlayerState();
    }
    if (!static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent))
    {
        Log::Error(L"Battle Royale bots: {}'s pawn has no ability system, it can't hold weapons", BotName);
        AIController->UnPossess();
        Pawn->K2_DestroyActor();
        PlayerBotPawn = nullptr;
        return false;
    }
    ApplyStartingHealth();
    // The warmup island hurts nobody: AFortGameModeAthena's restart of a player in the warmup turns the new pawn's
    // bCanBeDamaged off (3.1 0x9B3FD7, GamePhase == Warmup), so hits show their numbers and take nothing. The pawn the
    // bot leaves the bus with is a new one, which can be damaged.
    if (!bSkydive && State == ECoreAthenaBotState::Warmup)
    {
        Pawn->bCanBeDamaged = false;
        // A player arriving on the island has no spawn animation here: the bot stands a moment instead, as if it were
        // loading in (TickWarmup).
        bWarmupArrivalHold = true;
    }
    const int32_t MovementAttributes = InitializeMovementAttributes(Pawn, HeroObject);
    static bool bLoggedAttributes = false;
    if (!bLoggedAttributes)
    {
        bLoggedAttributes = true;
        UFortMovementSet* Set = Pawn->MovementSet;
        Log::Info(L"Battle Royale bots: {} movement attributes from the hero's stats row (walk {:.0f}, run {:.0f}, sprint {:.0f})", MovementAttributes,
            Set ? static_cast<float>(Set->WalkSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->RunSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->SprintSpeed.CurrentValue) : -1.0f);
    }
    // The outfit's parts on the player state and the pawn, as a player's hero gets them (Core runs the stripped
    // AFortPlayerStateAthena override for players in TrySetHeroImplementation; a bot's controller isn't a player's):
    // without them clients drew the bots without their outfit.
    if (AFortPlayerStateAthena* State = GetPlayerState(); State && AFortPlayerState::CanApplyCharacterCustomization() && Pawn->HasCustomizationLoadout())
    {
        State->ApplyCharacterCustomization(Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)));
        // Its hero lookup is a player controller's (GetHero): for a bot's it fails ("Failed initialization, using
        // default parts") and every bot wore F_Med_Head1 and F_Med_Soldier_01. The outfit's parts go back on, and the
        // back bling's, then to the pawn and every client (OnRep_CharacterParts).
        bool bChanged = false;
        for (size_t PartType = 0; PartType < HeroParts.size(); ++PartType)
        {
            UCustomCharacterPart*& Part = State->GetCharacterPart(EFortCustomPartType{ static_cast<uint8_t>(PartType) });
            if (HeroParts[PartType] && Part != HeroParts[PartType])
            {
                Part = HeroParts[PartType];
                bChanged = true;
            }
        }
        static UProperty* BackpackPartsProperty = nullptr;
        if (BackpackDefinition && !BackpackPartsProperty)
        {
            BackpackPartsProperty = BackpackDefinition->GetClass()->FindPropertyByName(FName(L"CharacterParts", FNAME_Find));
        }
        if (BackpackDefinition && BackpackPartsProperty && EFortCustomPartType::GetBackpack() < EFortCustomPartType::GetNumTypes())
        {
            const TArray<UCustomCharacterPart*>& BackpackParts = *reinterpret_cast<const TArray<UCustomCharacterPart*>*>(reinterpret_cast<const uint8_t*>(BackpackDefinition) + BackpackPartsProperty->GetOffset_ForInternal());
            for (UCustomCharacterPart* Part : BackpackParts)
            {
                if (Part && static_cast<EFortCustomPartType>(Part->CharacterPartType) == EFortCustomPartType::GetBackpack())
                {
                    State->GetCharacterPart(EFortCustomPartType::GetBackpack()) = Part;
                    bChanged = true;
                    break;
                }
            }
        }
        if (bChanged)
        {
            State->OnRep_CharacterParts();
            State->ForceNetUpdate();
        }
        if (BotIndex < 2)
        {
            const auto PartName = [State](EFortCustomPartType Type) { UCustomCharacterPart* Part = Type < EFortCustomPartType::GetNumTypes() ? State->GetCharacterPart(Type) : nullptr; return Part ? reinterpret_cast<UObject*>(Part)->GetName().ToStdString() : std::wstring(L"none"); };
            Log::Info(L"Battle Royale bots: {} wears {}, {}, back bling {} (pickaxe {}, glider {})", BotName, PartName(EFortCustomPartType::GetHead()), PartName(EFortCustomPartType::GetBody()),
                PartName(EFortCustomPartType::GetBackpack()), NameOf(reinterpret_cast<UObject*>(LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe())),
                Pawn->HasCustomizationLoadout() && FFortAthenaLoadout::HasGlider() ? NameOf(reinterpret_cast<UObject*>(static_cast<UAthenaGliderItemDefinition*>(Pawn->CustomizationLoadout.Glider))) : std::wstring(L"-"));
        }
    }
    GivePlayerAbilities(static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent));
    LogTrapTargeting(Pawn);
    // A large team game's pawns are relevant to their whole team, as a player's new pawn is (AFortGameModeAthena's
    // FinishRestartPlayer, 3.2 0x9B6310: bIsLargeTeamGame and not 50v50): the bot shows on its teammates' map wherever
    // it is, not only near them. 3.5.1's (0x9E43D0) reads the game state's bIsLargeTeamGame and its playlist's
    // MaxTeamSize instead: "cmp [GameState + bIsLargeTeamGame], 0; ...; cmp [CurrentPlaylistData + MaxTeamSize], 20; jle".
    if (AFortPlayerPawnAthena::HasTeamMembersAlwaysRelevant() && AFortGameModeAthena::HasLargeTeamGame())
    {
        Pawn->bTeamMembersAlwaysRelevant = GameMode->bIsLargeTeamGame && !GameMode->bFiftyFiftySquadPlay;
    }
    else if (AFortPlayerPawnAthena::HasTeamMembersAlwaysRelevant() && AFortGameStateAthena::HasLargeTeamGame() && AFortGameStateAthena::HasCurrentPlaylistData() && UFortPlaylist::HasMaxTeamSize())
    {
        AFortGameStateAthena* GameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState()));
        UFortPlaylistAthena* Playlist = GameState ? static_cast<UFortPlaylistAthena*>(GameState->CurrentPlaylistData) : nullptr;
        Pawn->bTeamMembersAlwaysRelevant = GameState && GameState->bIsLargeTeamGame && Playlist && Playlist->MaxTeamSize <= 20;
    }
    bTargeting = false;
    bWasMoving = false;
    bEmoting = false;
    bDanceStunned = false;
    // Path following moves the pawn with acceleration, as a player's input does (UNavMovementComponent::
    // bUseAccelerationForPaths, off by default: UPathFollowingComponent then calls RequestDirectMove, which sets the
    // velocity and leaves the movement's Acceleration at 0). Clients animate another player's pawn from the acceleration
    // the server packs for them (AFortPlayerPawn::AccelerationPack, 3.3 0xE71850, read back by OnRep_AccelerationPack,
    // 0xE6CAA0), so a bot on a path ran in its idle pose and slid across the ground (CoreV2 #252); a bot steering with
    // AddMovementInput animated.
    if (UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement; Movement && UCharacterMovementComponent::HasUseAccelerationForPaths())
    {
        Movement->bUseAccelerationForPaths = true;
    }
    Pawn->ForceNetUpdate();

    // The harvesting tool first (Core-held, 8.30's weapon list keeps the bot's weapons on its controller).
    bPickaxeEquipped = false;
    EquippedIndex = -1;
    EquippedWeapon = nullptr;
    // A skydiving player holds nothing (the pickaxe's equip sound was heard as the bots left the bus): it comes out on
    // landing.
    if (!bSkydive)
    {
        UAthenaPickaxeItemDefinition* Pickaxe = (LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe());
        if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
        {
            EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
            bPickaxeEquipped = EquippedWeapon.Get() != nullptr;
        }
        if (!WeaponList.empty())
        {
            EquipWeapon(BestWeaponIndex());
        }
    }

    if (bSkydive && Natives::AFortPlayerPawnAthena_BeginSkydiving)
    {
        Natives::AFortPlayerPawnAthena_BeginSkydiving(Pawn, true);
    }
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::ApplyStartingHealth()
{
    // The admin panel's starting health and shield (8.30 has no bot health field: its bot pawn class's default).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FCoreAthenaBotSettings& Settings = Manager->GetSettings();
    if (!Pawn)
    {
        return;
    }
    Pawn->SetMaxHealth(100.0f);
    Pawn->ForceMaxHealth(100.0f);
    Pawn->SetHealth(Settings.StartingHealth);
    Pawn->ForceMaxShield(100.0f);
    Pawn->SetShield(Settings.StartingShield);
}

void FCoreAthenaBotController::EnterAircraft(AFortAthenaAircraft* InAircraft, float InJumpTime, float InLatestJumpTime)
{
    BoardedAircraft = FWeakObjectPtr(reinterpret_cast<UObject*>(InAircraft));
    // The bus has left: like a player's, the warmup pawn goes (StartAircraftPhase destroys theirs) and the bot rides it.
    StopFiring();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        if (AAIController* AIController = Controller.Get())
        {
            AIController->UnPossess();
        }
        Pawn->K2_DestroyActor();
    }
    PlayerBotPawn = nullptr;
    // The warmup's pickups don't come along (ResetToStartingItems takes a player's).
    ClearInventory();
    SetTargeting(false);
    CurrentTarget = nullptr;
    WarmupRespawnTime = -1.0f;
    bWarmupGliding = false;
    if (WarmupDeaths > 0 || WarmupGlides > 0 || WarmupBrawls > 0)
    {
        Log::Info(L"Battle Royale bots: {} leaves the warmup island ({} new pawns there, {} glides, {} fights)", BotName, WarmupDeaths, WarmupGlides, WarmupBrawls);
    }
    bHasLandingSpot = false;
    bFollowingDrop = false;
    State = ECoreAthenaBotState::InAircraft;
    JumpTime = InJumpTime;
    LatestJumpTime = InLatestJumpTime;
    ThankBusDriverTime = AFortPlayerStateAthena::HasThankedBusDriver() ? -2.0f : -1.0f;
}

void FCoreAthenaBotController::Destroy()
{
    OnEndSpectating();
    StopFiring();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        Pawn->K2_DestroyActor();
    }
    if (AAIController* AIController = Controller.Get())
    {
        // Out of its team's AFortTeamInfo first (PlaceBotOnTeam put it in TeamMembers, as a player's controller is),
        // which keeps no controller that's gone: the game's RemoveFromTeam, as a player changing teams leaves theirs
        // (AttemptChangeTeamInfoForController). It also has the team's AFortTeamPrivateInfo drop the bot's entries:
        // taking the controller out of TeamMembers by hand left them, and a bot sent away from a player's squad stayed
        // on the squad bar and as a teammate marker where it had stood (CoreV2 #330). By hand where it isn't found.
        AFortPlayerStateAthena* PlayerState = GetPlayerState();
        AFortTeamInfo* TeamInfo = PlayerState ? static_cast<AFortTeamInfo*>(PlayerState->PlayerTeam) : nullptr;
        if (TeamInfo && Finder::ResolveTeamRemoveLive())
        {
            Natives::AFortTeamInfo_RemoveFromTeam(TeamInfo, reinterpret_cast<AController*>(AIController));
        }
        if (TeamInfo && AFortTeamInfo::HasTeamMembers())
        {
            TArray<AController*>& Members = TeamInfo->GetMember_TeamMembers();
            for (int32_t Index = Members.Num() - 1; Index >= 0; --Index)
            {
                if (Members[Index] == reinterpret_cast<AController*>(AIController))
                {
                    Members.RemoveAt(Index);
                }
            }
        }
        AIController->K2_DestroyActor();
    }
    PlayerBotPawn = nullptr;
    State = ECoreAthenaBotState::Dead;
}

void FCoreAthenaBotController::ClearInventory()
{
    WeaponList.clear();
    Ammo.clear();
    Consumables.clear();
    Materials[0] = Materials[1] = Materials[2] = 0;
    HarvestTarget = nullptr;
    HealingItem = nullptr;
    LootTarget = nullptr;
    ContainerTarget = nullptr;
    EquippedIndex = -1;
    EquippedWeapon = nullptr;
    bPickaxeEquipped = false;
}

void FCoreAthenaBotController::TickWarmup(float Now, float DeltaSeconds)
{
#if WITH_SERVER_CODE
    // Nobody can be hurt on the warmup island (the pawn spawns with bCanBeDamaged off, SpawnPawnAt), so a bot never
    // dies there; a pawn that is gone all the same (out of the world) is put back at a warmup spot.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Pawn->IsPendingKill() || Pawn->bIsDying || WarmupRespawnTime >= 0.0f)
    {
        RespawnInWarmup(Now);
        return;
    }
    if (Pawn->bCanBeDamaged)
    {
        Pawn->bCanBeDamaged = false;
    }
    LastKnownPosition = LocationOf(Pawn);

    // Falling from a height: the glider.
    if (WarmupGlide(Now))
    {
        return;
    }

    // Hurt by someone: the bot fights back (a player who shoots a bot there gets shot at).
    const float HealthShield = Pawn->GetHealth() + Pawn->GetShield();
    if (LastHealthShield >= 0.0f && HealthShield < LastHealthShield - 0.5f)
    {
        LastHurtTime = Now;
        if (AFortPlayerStateAthena* Attacker = KillerOf(reinterpret_cast<AFortPawn*>(Pawn)); Attacker && Attacker != GetPlayerState())
        {
            LastAttacker = Attacker;
            LastAttackerTime = Now;
        }
    }
    LastHealthShield = HealthShield;
    if (IsDanceStunned())
    {
        StopFiring();
        StopMoving();
        return;
    }
    if (Now - LastHurtTime < 1.0f && WarmupActivity != EWarmupActivity::Brawl)
    {
        ChooseWarmupActivity(Now, true);
    }
    else if (bWarmupArrivalHold)
    {
        // Just arrived: standing still for a few seconds, looking about a little (the Idle activity: no walking, no
        // shooting, no building). Being hurt above still starts a fight.
        bWarmupArrivalHold = false;
        WarmupActivity = EWarmupActivity::Idle;
        WarmupActivityUntil = Now + RandomRange(3.0f, 8.0f);
        NextLookAroundTime = Now + RandomRange(0.8f, 2.0f);
    }
    else if (Now >= WarmupActivityUntil)
    {
        ChooseWarmupActivity(Now, false);
    }

    switch (WarmupActivity)
    {
    case EWarmupActivity::Idle:
        // Standing about, looking around now and then.
        StopMoving();
        if (Now >= NextLookAroundTime)
        {
            NextLookAroundTime = Now + RandomRange(1.5f, 4.5f);
            const float Angle = RandomRange(0.0f, 6.2831853f);
            FaceTowards(FVector(LastKnownPosition.X + std::cos(Angle) * 1000.0f, LastKnownPosition.Y + std::sin(Angle) * 1000.0f, LastKnownPosition.Z + RandomRange(-150.0f, 150.0f)), Now, 2.5f);
        }
        break;
    case EWarmupActivity::Wander:
        // A short walk, then standing again.
        Wander(Now);
        if (bHasWanderGoal && Dist2D(LastKnownPosition, WanderGoal) < 250.0f)
        {
            WarmupActivityUntil = Now;
        }
        break;
    case EWarmupActivity::Emote:
        if (Now >= EmoteUntil)
        {
            StopEmote();
            WarmupActivityUntil = Now;
        }
        return;
    case EWarmupActivity::Brawl:
    {
        // A fight: a weapon from the island's floor if one lies close, then the nearest enemy close by (a bot or a
        // player), the ground game's own fighting.
        if (Now >= NextPerceptionTime)
        {
            NextPerceptionTime = Now + 0.25f;
            UpdateCurrentTarget(Now);
            AFortPawn* Target = CurrentTarget.Get();
            if (Target && (Dist2D(LocationOf(Target), LastKnownPosition) > 3500.0f || std::fabs(LocationOf(Target).Z - LastKnownPosition.Z) > 800.0f))
            {
                CurrentTarget = nullptr;
            }
        }
        ManageWeapon(Now);
        CheckWeaponFireResult(Now);
        if (!RangeAttack(Now))
        {
            StopFiring();
            SetTargeting(false);
            if (!(!HasUsableWeapon() && Loot(Now, 2500.0f)))
            {
                AFortPawn* Target = CurrentTarget.Get();
                if (Target && HasUsableWeapon())
                {
                    MoveTowards(LocationOf(Target), Now, 600.0f);
                }
                else if (!Loot(Now, 1200.0f))
                {
                    Wander(Now);
                }
            }
        }
        break;
    }
    }
    TryToUnstuck(Now);
    UpdateFacing(Now, DeltaSeconds);
#endif
}

void FCoreAthenaBotController::ChooseWarmupActivity(float Now, bool bFightBack)
{
#if WITH_SERVER_CODE
    // What players did on the warmup island: mostly standing about and walking a little, a dance, and every so often a
    // fight with the island's weapons (more often a bot that was hit fights back).
    StopEmote();
    if (WarmupActivity == EWarmupActivity::Brawl)
    {
        StopFiring();
        SetTargeting(false);
        CurrentTarget = nullptr;
    }
    const EWarmupActivity Previous = WarmupActivity;
    const float Roll = FMath::FRand();
    EWarmupActivity Next = bFightBack ? EWarmupActivity::Brawl
        : Roll < 0.45f ? EWarmupActivity::Idle : Roll < 0.75f ? EWarmupActivity::Wander : Roll < 0.88f ? EWarmupActivity::Emote : EWarmupActivity::Brawl;
    // Never two walks or two dances in a row: a stand between them.
    if (!bFightBack && Next == Previous && Next != EWarmupActivity::Idle)
    {
        Next = EWarmupActivity::Idle;
    }
    float Duration = Next == EWarmupActivity::Idle ? RandomRange(4.0f, 12.0f) : Next == EWarmupActivity::Wander ? RandomRange(4.0f, 9.0f) : RandomRange(20.0f, 45.0f);
    if (Next == EWarmupActivity::Emote)
    {
        StopMoving();
        const float EmoteLength = RandomRange(4.0f, 8.0f);
        if (PlayEmote(Now, EmoteLength))
        {
            Duration = EmoteLength + 0.5f;
        }
        else
        {
            Next = EWarmupActivity::Idle;
            Duration = RandomRange(4.0f, 10.0f);
        }
    }
    if (Next == EWarmupActivity::Wander)
    {
        bHasWanderGoal = false;
    }
    if (Next == EWarmupActivity::Brawl)
    {
        ++WarmupBrawls;
        if (WarmupBrawls <= 3)
        {
            Log::Info(L"Battle Royale bots: {} {} on the warmup island", BotName, bFightBack ? L"fights back" : L"looks for a fight");
        }
    }
    WarmupActivity = Next;
    WarmupActivityUntil = Now + Duration;
    NextLookAroundTime = Now + RandomRange(0.5f, 2.0f);
#endif
}

bool FCoreAthenaBotController::WarmupGlide(float Now)
{
#if WITH_SERVER_CODE
    // A player falling off the island's cliffs (or launched) opens their glider: the skydive's movement begins
    // (AFortPlayerPawnAthena::BeginSkydiving, not from the bus) and opens the parachute below its forced-deploy height
    // (SkydivingForceDeployParachuteWorldZ). The glider is the bot's loadout's.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UCharacterMovementComponent* Movement = Pawn ? static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement : nullptr;
    if (!Movement)
    {
        return false;
    }
    if (Pawn->bIsSkydiving || Pawn->bIsParachuteOpen)
    {
        if (!bWarmupGliding)
        {
            bWarmupGliding = true;
            Log::Info(L"Battle Royale bots: {} glides on the warmup island ({}, {:.0f} u/s down)", BotName, Pawn->bIsParachuteOpen ? L"glider open" : L"skydiving",
                -static_cast<float>(static_cast<FVector>(Movement->Velocity).Z));
        }
        StopFiring();
        StopEmote();
        return true;
    }
    if (bWarmupGliding)
    {
        bWarmupGliding = false;
        LastHealthShield = -1.0f;
        Log::Info(L"Battle Royale bots: {} lands from its glide at ({:.0f}, {:.0f}, {:.0f})", BotName, LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z);
    }
    constexpr uint8_t MoveFalling = 3;
    if (Movement->MovementMode != MoveFalling || static_cast<FVector>(Movement->Velocity).Z > -400.0f || Now < NextGlideCheck || !Natives::AFortPlayerPawnAthena_BeginSkydiving)
    {
        return false;
    }
    NextGlideCheck = Now + 0.2f;
    // High enough: nothing below for 12 m (a jump off a step or a ramp lands first).
    const FVector Feet(LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z - 90.0f);
    if (IsBlocked(Pawn, Feet, FVector(Feet.X, Feet.Y, Feet.Z - 1200.0f)))
    {
        return false;
    }
    StopEmote();
    StopFiring();
    StopMoving();
    Natives::AFortPlayerPawnAthena_BeginSkydiving(Pawn, false);
    Pawn->ForceNetUpdate();
    ++WarmupGlides;
    Log::Info(L"Battle Royale bots: {} falls off the warmup island's heights and opens its glider (skydiving {}, glider {})", BotName,
        Pawn->bIsSkydiving ? L"yes" : L"no", Pawn->bIsParachuteOpen ? L"open" : L"closed");
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::RespawnInWarmup(float Now)
{
#if WITH_SERVER_CODE
    // The warmup pawn is gone (fell out of the world): a new one a few seconds later at a warmup spot, with nothing but
    // the pickaxe, as the game restarts a player there (a new pawn at a FortPlayerStartWarmup).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (WarmupRespawnTime < 0.0f)
    {
        WarmupRespawnTime = Now + 3.0f;
        ++WarmupDeaths;
        StopFiring();
        StopEmote();
        StopMoving();
        CurrentTarget = nullptr;
        LootTarget = nullptr;
        Log::Info(L"Battle Royale bots: {}'s warmup pawn is gone, a new one in a few seconds", BotName);
        return;
    }
    if (Now < WarmupRespawnTime)
    {
        return;
    }
    FVector Around = LastKnownPosition;
    Manager->GetWarmupSpot(Around);
    FVector Spot;
    if (!Manager->FindGroundNear(Around, 600.0f, Spot))
    {
        WarmupRespawnTime = Now + 1.0f;
        return;
    }
    WarmupRespawnTime = -1.0f;
    if (Pawn)
    {
        if (AAIController* AIController = Controller.Get())
        {
            AIController->UnPossess();
        }
        if (!Pawn->IsPendingKill())
        {
            Pawn->K2_DestroyActor();
        }
    }
    PlayerBotPawn = nullptr;
    ClearInventory();
    bWarmupGliding = false;
    LastHealthShield = -1.0f;
    LastHurtTime = -100.0f;
    LastAttacker = nullptr;
    WarmupActivity = EWarmupActivity::Idle;
    WarmupActivityUntil = Now + RandomRange(2.0f, 5.0f);
    if (SpawnPawnAt(FVector(Spot.X, Spot.Y, Spot.Z + 100.0f), RandomRange(0.0f, 360.0f), false))
    {
        Log::Info(L"Battle Royale bots: {} is back on the warmup island at ({:.0f}, {:.0f}, {:.0f})", BotName, Spot.X, Spot.Y, Spot.Z);
    }
#endif
}

void FCoreAthenaBotController::AddKill()
{
    ++Kills;
    bDanceOnKill = State == ECoreAthenaBotState::Landed && FMath::FRand() < 0.5f;
    if (AFortPlayerStateAthena* PlayerState = GetPlayerState())
    {
        PlayerState->SetKillScore(Kills);
    }
}

namespace
{
    bool HasDBNOAbilityActive(AFortPlayerPawnAthena* Pawn);
}

void FCoreAthenaBotController::Tick(float Now, float DeltaSeconds)
{
#if WITH_SERVER_CODE
    LastTickTime = Now;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    // A held fire cue whose trigger time ran out, whatever the bot went on to do.
    if (!FireCueTag.IsNone() && Now >= FireCueUntil)
    {
        RemoveWeaponFireCue();
    }
    switch (State)
    {
    case ECoreAthenaBotState::Dead:
        if (IsRespawnPending())
        {
            RespawnPlayerAfterDeath(Now);
            return;
        }
        TickSpectating(Now);
        return;
    case ECoreAthenaBotState::InAircraft:
        JumpOffBus(Now);
        return;
    case ECoreAthenaBotState::Warmup:
        TickWarmup(Now, DeltaSeconds);
        return;
    default:
        break;
    }

    // Skydiving or on the ground: the pawn's death takes the bot out of the match.
    // A downed pawn isn't dead at 0 health (AFortPawn::IsDead counts it as dead): the knock leaves it there until its DBNO
    // ability heals it, and a bleed-out is Downed's to end.
    bool bDead = !Pawn || Pawn->IsPendingKill() || Pawn->bIsDying;
    if (!bDead && !Pawn->bIsDBNO && Now >= NextDeathCheck)
    {
        NextDeathCheck = Now + 0.5f;
        bDead = Pawn->IsDead();
    }
    if (bDead)
    {
        AFortPlayerStateAthena* BotKiller = Pawn ? Manager->LastBotAttackerOf(Pawn) : nullptr;
        AFortPlayerStateAthena* Killer = Pawn ? KillerOf(Pawn) : nullptr;
        // The pawn may be gone already (or its damagers cleared): whoever hurt it last, within 15 s.
        if (!Killer && LastAttacker.Get() && Now - LastAttackerTime < 15.0f)
        {
            Killer = LastAttacker.Get();
        }
        // Bled out while down: the one who knocked it is its eliminator.
        Manager->RemoveFromAliveBots(this, Killer ? Killer : BotKiller ? BotKiller : Downer.Get());
        return;
    }
    if (State == ECoreAthenaBotState::Skydiving)
    {
        FreeFalling(Now);
        return;
    }

    LastKnownPosition = LocationOf(Pawn);
    if (!bRegisteredInvoker && Manager->HasNavigation())
    {
        // 8.30's bots are their own navigation invokers: the navmesh is generated around them.
        bRegisteredInvoker = true;
        UAthenaNavSystem::RegisterNavigationInvoker(reinterpret_cast<AActor*>(Pawn));
    }
    // Hurt (by an enemy: the storm's damage outside the circle doesn't count) puts the bot in a fight for a while.
    {
        FVector Center;
        float Radius = 0.0f;
        const bool bOutside = Manager->GetCurrentSafeZone(Center, Radius) && Dist2D(LastKnownPosition, Center) > Radius;
        const float HealthShield = Pawn->GetHealth() + Pawn->GetShield();
        if (LastHealthShield >= 0.0f && HealthShield < LastHealthShield - 0.5f && !bOutside)
        {
            LastHurtTime = Now;
        }
        if (LastHealthShield >= 0.0f && HealthShield < LastHealthShield - 0.5f)
        {
            if (AFortPlayerStateAthena* Attacker = KillerOf(reinterpret_cast<AFortPawn*>(Pawn)); Attacker && Attacker != GetPlayerState())
            {
                LastAttacker = Attacker;
                LastAttackerTime = Now;
                // Shot in the open: sometimes a wall (and a ramp) goes up towards the shooter (DefensiveBuilding); shot by
                // an enemy it hadn't seen, sometimes a box (Bunker's unknown threat).
                AFortPawn* AttackerPawn = reinterpret_cast<AFortPawn*>(Attacker->GetCurrentPawn());
                if (!bOutside && !Pawn->bIsDBNO && !DefensiveBuilding(Now, AttackerPawn) && AttackerPawn && AttackerPawn != CurrentTarget.Get() && AttackerPawn != LastSeenTarget.Get())
                {
                    Bunker(Now, AttackerPawn, true);
                }
            }
        }
        LastHealthShield = HealthShield;
    }
    // Down (squads): crawling to a teammate, bleeding out, or eliminated with the team.
    if (Pawn->bIsDBNO)
    {
        if (DBNOStartedAt <= 0.0f)
        {
            // Downed by the game (a player's hit: AFortPlayerPawnAthena's DBNO handling), not by Core's EnterDBNO.
            AFortPlayerStateAthena* Knocker = KillerOf(reinterpret_cast<AFortPawn*>(Pawn));
            Knocker = Knocker && Knocker != GetPlayerState() ? Knocker : LastAttacker.Get();
            OnEnteredDBNO(Knocker, Now, false);
            Log::Info(L"Battle Royale bots: {} is down (DBNO, the game's){}, {:.0f} health, the DBNO ability {}", BotName,
                Knocker ? std::format(L", knocked by {}", Knocker->GetPlayerName().ToStdString()) : std::wstring(), Pawn->GetHealth(), HasDBNOAbilityActive(Pawn) ? L"active" : L"not active yet");
        }
        Downed(Now);
        UpdateFacing(Now, DeltaSeconds);
        TakeStormDamage(Now);
        return;
    }
    if (DBNOStartedAt > 0.0f)
    {
        // Up again without a bot's revive: a player teammate revived it (a bot on a player's team).
        Log::Info(L"Battle Royale bots: {} is back up (revived by a player), {:.0f} health", BotName, Pawn->GetHealth());
        OnRevived();
    }
    // A boogie bomb's dance: the bot neither moves nor shoots until it ends (the ability's own effect stops the pawn).
    if (const bool bStunned = IsDanceStunned(); bStunned || bDanceStunned)
    {
        if (bStunned != bDanceStunned)
        {
            bDanceStunned = bStunned;
            Log::Info(L"Battle Royale bots: {} {} a boogie bomb's dance", BotName, bStunned ? L"is caught in" : L"is free of");
        }
        if (bStunned)
        {
            StopFiring();
            StopMoving();
            CancelHeal();
            TakeStormDamage(Now);
            return;
        }
    }
    if (Now >= NextPerceptionTime)
    {
        // Spread over the frames by the bot's index, so twenty bots in the last circle don't all trace in one frame.
        NextPerceptionTime = Now + 0.25f + (NextPerceptionTime <= 0.0f ? static_cast<float>(BotIndex % 8) * 0.033f : 0.0f);
        UpdateCurrentTarget(Now);
    }
    if (HealingItem && IsInCombat(Now))
    {
        CancelHeal();
    }
    if (bDanceOnKill && !IsInCombat(Now) && !HealingItem)
    {
        bDanceOnKill = false;
        PlayEmote(Now, 3.0f);
    }
    if (Now < EmoteUntil)
    {
        if (!IsInCombat(Now))
        {
            return;
        }
    }
    StopEmote();
    if (HarvestTarget.Get() && IsInCombat(Now))
    {
        StopHarvesting();
    }
    // A revive under way stops first if an enemy turned up (ReviveTeammate lets go of it then), so the bot fights with its
    // weapons in hand. Mid-revive its hands are on the interaction: no reload, no weapon change (CoreV2 #287).
    if (IsReviving() && (IsUnderThreat(Now) || (!ReviveTarget && !RevivePlayer.Get())))
    {
        ReviveTeammate(Now);
        if (IsReviving())
        {
            EndReviveInteraction();
            ReviveDoneAt = 0.0f;
        }
    }
    if (!HealingItem && !HarvestTarget.Get() && !IsReviving())
    {
        ManageWeapon(Now);
        CheckWeaponFireResult(Now);
    }
    const wchar_t* Intent = L"attack";
    const auto DecisionStarted = std::chrono::steady_clock::now();
    // The storm closing in: a heal is put off until the bot is in (it stood still in the storm for a medkit's 10 s).
    bHeadingForSafeZone = ShouldHeadForSafeZone(Now);
    if (bHeadingForSafeZone && HealingItem)
    {
        CancelHeal();
    }
    // Nor a build or a box to hide in (a ramp out of where it is stuck is its way to the circle).
    if (bHeadingForSafeZone)
    {
        if (!BuildQueue.bToUnstick)
        {
            ClearBuildingQueue();
        }
        HideEndTime = 0.0f;
    }
    // Mid-revive, the bot keeps at it unless an enemy is close or shooting at it.
    if (ReviveDoneAt > 0.0f && (ReviveTarget || RevivePlayer.Get()) && !IsUnderThreat(Now) && ReviveTeammate(Now))
    {
        StopFiring();
        SetTargeting(false);
        Intent = L"revive";
    }
    // Building: it stands and holds fire until the last piece is up.
    else if (TickBuildingQueue(Now))
    {
        StopFiring();
        SetTargeting(false);
        Intent = L"build";
    }
    else if (HealingItem || !RangeAttack(Now))
    {
        StopFiring();
        SetTargeting(false);
        // Healing first when safe, then loot close by (a bot without a gun arms itself before it heads for the circle: a
        // landing spot far from the first circle otherwise sent it off empty-handed). In the storm, or with the storm
        // about to catch it (ShouldHeadForSafeZone), it runs before anything else. Then the goal it set out on, until it
        // is done (ChooseGoal: it turned back and forth between two of them, re-choosing each frame), and when it is,
        // the next one.
        const bool bArmed = HasUsableWeapon();
        // A bot stuck against a tree, rock or wall breaks through it first (TryToUnstuck chose it).
        if (bHarvestToUnstick && Harvest(Now))
        {
            Intent = L"harvest";
        }
        else if (bHeadingForSafeZone && Storm(Now))
        {
            Intent = L"storm";
            SetGoal(ECoreAthenaBotGoal::Storm, Now);
        }
        else if (ReviveTeammate(Now))
        {
            Intent = L"revive";
        }
        else if (Heal(Now))
        {
            Intent = L"heal";
        }
        // In its box after an unseen enemy's shots (Bunker's HideEndTime), with nobody in sight to fight.
        else if (Now < HideEndTime)
        {
            StopMoving();
            Intent = L"bunker";
        }
        // A squad's player marked the map: the bot goes there (an unarmed one arms itself first).
        else if (bHasMapMarker && !bArmed && Loot(Now, 4000.0f))
        {
            Intent = L"loot";
        }
        else if (FollowMapMarker(Now))
        {
            Intent = L"marker";
        }
        // What lies within 15 m is taken on the way, whatever the goal (it stays the goal).
        else if (Loot(Now, 1500.0f) || (!bArmed && Loot(Now, 4000.0f)))
        {
            Intent = L"loot";
        }
        else if (SearchContainer(Now, bArmed ? 1500.0f : 5000.0f))
        {
            Intent = L"search";
        }
        // A bot that just lost sight of the enemy it was fighting goes where it last saw it for a moment, instead of
        // turning away to its loot and back when it shows again.
        else if (bArmed && LastSeenTarget.Get() && !LastSeenTarget.Get()->bIsDying && Now - LastTargetInSightTime < 3.0f && Dist2D(LastKnownPosition, LastSeenTargetLocation) > 300.0f)
        {
            Intent = L"chase";
            MoveTowards(LastSeenTargetLocation, Now, 250.0f);
        }
        else
        {
            Intent = ChooseGoal(Now);
        }
        // A break-through Loot or SearchContainer started just now (BreakDownFromRoof: the bot is on a roof over its
        // loot) is kept for the next frame's Harvest. Stopped here, it was started again the next frame, every frame:
        // the bot stood on the roof for minutes, logging the break-through each frame (#319).
        const bool bBreakThroughStarted = bHarvestToUnstick && HarvestTargetTime == Now;
        if (HarvestTarget.Get() && std::wstring_view(Intent) != L"harvest" && !bBreakThroughStarted)
        {
            StopHarvesting();
        }
    }
    // A slow decision (a hitch in the summary) is named, with what the bot was doing.
    if (const double DecisionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - DecisionStarted).count(); DecisionMs > 50.0)
    {
        static int32_t LoggedSlow = 0;
        if (LoggedSlow++ < 30)
        {
            Log::Info(L"Battle Royale bots: {}'s decision took {:.0f} ms ({}, goal {}, {:.0f} m from where it is going)", BotName, DecisionMs, Intent, GoalName(Goal), bHasMoveGoal ? Dist2D(LastKnownPosition, MoveGoal) / 100.0f : 0.0f);
        }
    }
    TryToUnstuck(Now);
    UpdateFacing(Now, DeltaSeconds);
    TakeStormDamage(Now);
    if (!bLoggedMovement && LandedTime > 0.0f && Now - LandedTime > 6.0f)
    {
        bLoggedMovement = true;
        LogMovementDiagnostics();
    }

    if (Now >= NextLogTime)
    {
        NextLogTime = Now + 20.0f;
        AFortPawn* Target = CurrentTarget.Get();
        UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
        const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
        Log::Info(L"Battle Royale bots: {} at ({:.0f}, {:.0f}, {:.0f}) {}s, speed {:.0f} (max {:.0f}), mode {}, style {}, {}: {:.0f} health, {:.0f} shield, {} weapons, holding {} ({} loaded), target {}{}, {} shots ({} the game's), {} searched, {} jumps, {} doors, {} reloads, {} weapon changes, {} heals ({} healing items), {} swings, {}/{}/{} materials, {} shots stopped by walls, {} goal changes, {} POIs reached, {} hunts, {} bots let be", BotName,
            LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z, Intent, Velocity.Size(), MaxSpeedOf(Movement), Movement ? static_cast<int32_t>(Movement->MovementMode) : -1, static_cast<int32_t>(Pawn->CurrentMovementStyle),
            bPathFollowing ? std::format(L"on a path ({} paths, {} failed)", PathRequests, PathFailures) : std::format(L"steering ({} paths, {} failed, {})", PathRequests, PathFailures, SteerReason), Pawn->GetHealth(), Pawn->GetShield(),
            WeaponList.size(), EquippedWeapon.Get() ? NameOf(static_cast<UFortWeaponItemDefinition*>(EquippedWeapon.Get()->WeaponData)) : std::wstring(L"nothing"),
            EquippedWeapon.Get() ? static_cast<int32_t>(EquippedWeapon.Get()->AmmoCount) : 0, Target ? NameOf(Target) : std::wstring(L"none"), bHasVisualLineOfSightOnTarget ? L" (in sight)" : L"", ShotsFired, NativeShots, ContainersSearched, Jumps, DoorsOpened, Reloads, WeaponSwitches, Heals, Consumables.size(), Swings, Materials[0], Materials[1], Materials[2], BlockedShots, GoalChanges, POIsVisited, Hunts, BotsIgnored);
    }
#endif
}

void FCoreAthenaBotController::JumpOffBus(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_JumpOffBus: at the bot's moment along the path (EnterAircraft picked it), or
    // when the bus is about to leave the drop zone, the bot leaves the aircraft where it is, skydiving from the bus as a
    // player does (ExitAircraft's AFortPlayerPawnAthena::BeginSkydiving).
    if (ThankBusDriverTime == -2.0f)
    {
        // About a third of the bots thank the bus driver, as players do: at some moment between the bus leaving and
        // their jump.
        const float Before = std::min(JumpTime, LatestJumpTime) - 0.5f;
        ThankBusDriverTime = FMath::FRand() < 0.35f && Before > Now + 1.0f ? Now + 1.0f + (Before - Now - 1.0f) * FMath::FRand() : -1.0f;
    }
    if (ThankBusDriverTime >= 0.0f && Now >= ThankBusDriverTime)
    {
        ThankBusDriverTime = -1.0f;
        ThankBusDriver();
    }
    if (Now < JumpTime && Now < LatestJumpTime)
    {
        return;
    }
    ThankBusDriverTime = -1.0f;
    AActor* Aircraft = GetAircraft();
    const FVector BusLocation = Aircraft ? LocationOf(Aircraft) : LastKnownPosition + FVector(0.0f, 0.0f, 15000.0f);
    const FVector Start(BusLocation.X + RandomRange(-300.0f, 300.0f), BusLocation.Y + RandomRange(-300.0f, 300.0f), BusLocation.Z - 500.0f);

    // Where to land: loot near the bus's path, or a spot on the ground below it (21.40's _FreeFalling picks a POI).
    const std::vector<FCoreAthenaBotLoot>& LootList = Manager->GetLoot();
    std::vector<FVector> Near;
    const bool bHotDrop = Configuration::Get().bBotHotDrop && bHasHotDrop;
    for (const FCoreAthenaBotLoot& Entry : LootList)
    {
        // The landing spot the manager chose (a named location or a cluster of chests): a weapon in it, else its middle.
        const bool bInSpot = bHasLandingSpot && Dist2D(Entry.Location, LandingSpot) < LandingRadius;
        if (Entry.bWeapon && (bHasLandingSpot ? bInSpot : bHotDrop ? Dist2D(Entry.Location, HotDrop) < 8000.0f : Dist2D(Entry.Location, Start) < 20000.0f))
        {
            Near.push_back(Entry.Location);
        }
    }
    if (Near.empty() && bHasLandingSpot)
    {
        DropTarget = LandingSpot;
    }
    else if (!Near.empty())
    {
        DropTarget = Near[static_cast<size_t>(FMath::Rand()) % Near.size()];
        if (Configuration::Get().bBotHotDrop && !bHasHotDrop)
        {
            HotDrop = DropTarget;
            bHasHotDrop = true;
        }
    }
    else if (!Manager->FindGroundNear(Start, 15000.0f, DropTarget))
    {
        DropTarget = FVector(Start.X, Start.Y, 0.0f);
    }
    if (!SpawnPawnAt(Start, RandomRange(0.0f, 360.0f), true))
    {
        JumpTime = Now + 1.0f;
        LatestJumpTime = std::max(LatestJumpTime, Now + 1.0f);
        return;
    }
    State = ECoreAthenaBotState::Skydiving;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    Log::Info(L"Battle Royale bots: {} jumps from the bus at ({:.0f}, {:.0f}, {:.0f}) towards ({:.0f}, {:.0f}) {:.0f} m away (skydiving {})", BotName, Start.X, Start.Y, Start.Z,
        DropTarget.X, DropTarget.Y, Dist2D(Start, DropTarget) / 100.0f, Pawn && Pawn->bIsSkydiving ? L"yes" : L"no");
    NextLogTime = Now + 3.0f;
    LandedTime = 0.0f;
#endif
}

void FCoreAthenaBotController::ThankBusDriver()
{
#if WITH_SERVER_CODE
    // AFortPlayerControllerAthena::ServerThankBusDriver_Implementation (5.30 0xC289A0): the controller's PlayerState, when
    // it is an AFortPlayerStateAthena, gets bThankedBusDriver. Replicated, it shows in every client's feed
    // (OnRep_ThankedBusDriver). The bot's controller is an AAIController, so the RPC itself can't run on it.
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (!AFortPlayerStateAthena::HasThankedBusDriver() || !PlayerState || PlayerState->bThankedBusDriver)
    {
        return;
    }
    PlayerState->bThankedBusDriver = true;
    Log::Info(L"Battle Royale bots: {} thanks the bus driver", BotName);
#endif
}

void FCoreAthenaBotController::FreeFalling(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling: steer towards the landing spot while skydiving and under the
    // parachute, until the pawn walks.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    LastKnownPosition = Location;
    const FVector Direction = Direction2D(Location, DropTarget);
    const float Distance = Dist2D(Location, DropTarget);
    // A player dives at their spot. The skydive's fast fall is its active air control (UFortMovementComp_CharacterAthena's
    // SkydivingControlParamsActive: terminal velocity 6000 and lateral speed 2400 against the passive 1200 and 1200 on
    // 3.1, rows Default.SkydivingControlActive.*), which the movement uses (3.1 0x9B6340, 0x9C0110) while the pawn's
    // input acceleration points down (its Z below -0.1) and along the controller's view (the dot of the two directions
    // above 0.4). A player's input follows the camera, pitched down. A bot looks at its spot (AAIController keeps a
    // focal point's pitch at 0) and its input is that heading tilted down; above the spot, mostly down with a little of
    // the heading, which keeps the dot over 0.4.
    const float Height = Location.Z - DropTarget.Z;
    const bool bWantsDive = Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen;
    if (Distance > 300.0f)
    {
        DiveHeading = Direction;
    }
    if (AAIController* AIController = Controller.Get(); AIController && (bWantsDive != bDiving || Now >= NextShotTime))
    {
        NextShotTime = Now + 1.0f;
        AIController->SetFocalPoint(FVector(Location.X + DiveHeading.X * 1000.0f, Location.Y + DiveHeading.Y * 1000.0f, Location.Z));
        if (bWantsDive && !bDiving)
        {
            Log::Info(L"Battle Royale bots: {} dives at {:.0f} m up, {:.0f} m from its spot", BotName, Height / 100.0f, Distance / 100.0f);
        }
        bDiving = bWantsDive;
    }
    if (bWantsDive && DiveHeading.SizeSquared() > 0.01f)
    {
        FVector Dive = Distance > 300.0f ? FVector(DiveHeading.X * 0.8f, DiveHeading.Y * 0.8f, -0.6f) : FVector(DiveHeading.X * 0.5f, DiveHeading.Y * 0.5f, -0.87f);
        Dive = Dive * (1.0f / Dive.Size());
        Pawn->AddMovementInput(Dive, 1.0f, true);
    }
    else if (Distance > 300.0f)
    {
        Pawn->AddMovementInput(Direction, 1.0f, true);
    }

    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    const bool bWalking = Movement && Movement->MovementMode == MoveWalking;
    if (Now >= NextLogTime)
    {
        NextLogTime = Now + 5.0f;
        AAIController* AIController = Controller.Get();
        Log::Info(L"Battle Royale bots: {} falls at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot, {:.0f} u/s down, view pitch {:.0f} (skydiving {}, parachute {}, movement {})", BotName, Location.X, Location.Y, Location.Z,
            Distance / 100.0f, Movement ? -static_cast<float>(static_cast<FVector>(Movement->Velocity).Z) : 0.0f, AIController ? static_cast<FRotator>(AIController->ControlRotation).Pitch : 0.0f,
            Pawn->bIsSkydiving ? L"yes" : L"no", Pawn->bIsParachuteOpen ? L"open" : L"closed", Movement ? static_cast<int32_t>(Movement->MovementMode) : -1);
    }
    if (bWalking && !Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen)
    {
        if (LandedTime <= 0.0f)
        {
            LandedTime = Now;
        }
        if (Now - LandedTime > 0.5f)
        {
            State = ECoreAthenaBotState::Landed;
            NextLogTime = Now + 5.0f;
            bDiving = false;
            bHasFacingYaw = false;
            if (!EquippedWeapon.Get())
            {
                EquipPickaxe();
                if (!WeaponList.empty())
                {
                    EquipWeapon(BestWeaponIndex());
                }
            }
            Pawn->ForceNetUpdate();
            Log::Info(L"Battle Royale bots: {} lands at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot", BotName, Location.X, Location.Y, Location.Z, Distance / 100.0f);
        }
    }
    else
    {
        LandedTime = 0.0f;
    }
#endif
}

void FCoreAthenaBotController::UpdateCurrentTarget(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's perception (OnPerceptionSensed, an EAlertLevel state machine over sight, hearing and damage), cheaply: the
    // nearest enemies in sight range, the nearest one in line of sight first; one that hurt the bot is its target too.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    LastKnownPosition = Location;
    struct FCandidate
    {
        AFortPawn* Pawn;
        float Distance;
        bool bDowned;
    };
    std::vector<FCandidate> Candidates;
    bool bHasStandingCandidate = false;
    AFortPawn* Previous = CurrentTarget.Get();
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (!Other || reinterpret_cast<AFortPlayerPawnAthena*>(Other) == Pawn || Other->bIsDying || Other->IsPendingKill() || Manager->TeamOfPawn(Other) == Team)
        {
            continue;
        }
        const float Distance = (LocationOf(Other) - Location).Size();
        if (Distance > AIBotSkills.SightRange)
        {
            continue;
        }
        // Players come first: after the bus, a bot is passed by unless it is after this one (or the roll says fight), and
        // sorted 15 m farther than a player (the warmup island's brawls are anyone's).
        FCoreAthenaBotController* OtherBot = State != ECoreAthenaBotState::Warmup ? Manager->FindBot(reinterpret_cast<UObject*>(Other)) : nullptr;
        if (OtherBot && Other != Previous && !WillEngageBot(OtherBot, Now))
        {
            ++BotsIgnored;
            continue;
        }
        // A downed enemy is finished once nobody standing is closer (sorted 20 m farther). The enemy it is fighting
        // stays its target unless another is much closer (it turned from one to the other and back).
        const bool bDowned = Other->bIsDBNO;
        bHasStandingCandidate = bHasStandingCandidate || !bDowned;
        float Sorted = Distance + (bDowned ? 2000.0f : 0.0f) + (OtherBot ? 1500.0f : 0.0f);
        Sorted *= Other == Previous ? 0.7f : 1.0f;
        Candidates.push_back({ Other, Sorted, bDowned });
    }
    // Don't thirst a knocked enemy right away: while a standing enemy is also in reach, drop a downed one the bot should
    // still be giving time (so a teammate can revive it), and fight the standing enemy (a reviver among them) instead. A
    // lone downed enemy, or one past its grace with no reviver, stays, so it is still finished eventually (CoreV2 #233).
    if (bHasStandingCandidate)
    {
        std::erase_if(Candidates, [this, Now](const FCandidate& Candidate) { return Candidate.bDowned && ShouldDeferThirst(Candidate.Pawn, Now); });
    }
    std::sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

    // Damage: the newest damager is an enemy pawn within twice the sight range (a bot only while it is still hurting
    // this one: one that shot it a minute ago isn't hunted down across the map).
    AFortPlayerStateAthena* Attacker = KillerOf(Pawn);
    AFortPawn* AttackerPawn = nullptr;
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (Attacker && Other && reinterpret_cast<UObject*>(static_cast<APlayerState*>(Other->PlayerState)) == reinterpret_cast<UObject*>(Attacker) && Manager->TeamOfPawn(Other) != Team
            && (Now - LastHurtTime < 10.0f || !Manager->FindBot(reinterpret_cast<UObject*>(Other))))
        {
            AttackerPawn = Other;
        }
    }

    AFortPawn* NewTarget = nullptr;
    bool bInSight = false;
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    for (size_t Index = 0; Index < Candidates.size() && Index < 3 && !NewTarget; ++Index)
    {
        const FVector TargetLocation = LocationOf(Candidates[Index].Pawn);
        if (HasClearShot(Pawn, Eye, FVector(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 40.0f), Candidates[Index].Pawn))
        {
            NewTarget = Candidates[Index].Pawn;
            bInSight = true;
        }
    }
    if (!NewTarget && AttackerPawn && (LocationOf(AttackerPawn) - Location).Size() <= AIBotSkills.SightRange * 2.0f)
    {
        // Whoever hurt the bot is its target, but it only shoots once it sees them (it used to fire back through walls).
        const FVector AttackerLocation = LocationOf(AttackerPawn);
        NewTarget = AttackerPawn;
        bInSight = HasClearShot(Pawn, Eye, FVector(AttackerLocation.X, AttackerLocation.Y, AttackerLocation.Z + 40.0f), AttackerPawn);
    }

    // Out of sight behind the builds it just put up around itself, the last target isn't forgotten (RangeAttack breaks in).
    if (!NewTarget && Previous && !Previous->bIsDying && Now - LastTargetInSightTime < 15.0f && (LocationOf(Previous) - Location).Size() < 3000.0f && IsTargetBoxedIn(Previous))
    {
        NewTarget = Previous;
    }
    // Nor is one that just ducked behind cover: the bot remembers it for TargetMemoryAge and RangeAttack aims at where
    // it last saw it (it forgot the enemy the moment the line of sight broke, CoreV2 #349). A downed one it is giving
    // time to (ShouldDeferThirst) is let go, as above.
    if (!NewTarget && Previous && Previous == LastSeenTarget.Get() && !Previous->bIsDying && !Previous->IsPendingKill() && Now - LastTargetInSightTime < TargetMemoryAge
        && (LocationOf(Previous) - Location).Size() <= AIBotSkills.SightRange && (!Previous->bIsDBNO || !ShouldDeferThirst(Previous, Now)))
    {
        NewTarget = Previous;
    }

    if (NewTarget != CurrentTarget.Get())
    {
        CurrentTarget = NewTarget;
        TargetAcquiredTime = Now;
        static int32_t LoggedSpots = 0;
        if (NewTarget && LoggedSpots++ < 100)
        {
            Log::Info(L"Battle Royale bots: {} spots {} {:.0f} m away", BotName, NameOf(reinterpret_cast<AFortPlayerStateAthena*>(static_cast<APlayerState*>(NewTarget->PlayerState)) ?
                reinterpret_cast<UObject*>(static_cast<APlayerState*>(NewTarget->PlayerState)) : reinterpret_cast<UObject*>(NewTarget)), (LocationOf(NewTarget) - Location).Size() / 100.0f);
        }
    }
    bHasVisualLineOfSightOnTarget = bInSight;
    if (NewTarget && bInSight)
    {
        LastSeenTarget = NewTarget;
        LastSeenTargetLocation = LocationOf(NewTarget);
    }
    CurrentAlertLevel = NewTarget ? ECoreAlertLevel::Threatened : Attacker ? ECoreAlertLevel::Alerted : ECoreAlertLevel::Unaware;
#endif
}

bool FCoreAthenaBotController::WillEngageBot(FCoreAthenaBotController* Other, float Now) const
{
    // Whoever it is fighting or going for it is fought back (its shots hurt this bot, or will).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Other || (Pawn && Other->GetCurrentTarget() == reinterpret_cast<AFortPawn*>(Pawn)))
    {
        return true;
    }
    // Otherwise a roll per pair of bots, the same for 30 s (so the bot doesn't change its mind each look).
    const uint32_t Period = static_cast<uint32_t>(std::max(Now, 0.0f) / BotEngageRollPeriod);
    uint32_t Hash = static_cast<uint32_t>(BotIndex) * 73856093u ^ static_cast<uint32_t>(Other->GetBotIndex()) * 19349663u ^ Period * 83492791u;
    Hash ^= Hash >> 13;
    Hash *= 0x5bd1e995u;
    Hash ^= Hash >> 15;
    const int32_t BaseChance = std::clamp(Manager->GetSettings().BotFightChance, 0, 100);
    const int32_t Chance = Manager->GetAliveBotCount() <= LateGameBotCount ? std::min(BaseChance * 2, 100) : BaseChance;
    return static_cast<int32_t>(Hash % 100u) < Chance;
}

bool FCoreAthenaBotController::ShouldDeferThirst(AFortPawn* Downed, float Now) const
{
#if WITH_SERVER_CODE
    // Late game (few players left): finish knocked enemies promptly so the match still ends.
    if (Manager->PlayersLeft() <= LateGameBotCount)
    {
        return false;
    }
    // No squadmate close enough to revive it (none at all, or the nearest too far to make it in time): nothing to wait
    // for, so it is fair game (it is still sorted behind standing enemies, so a lone downed one is finished in its time).
    const float NearestMate = Manager->NearestStandingSquadmateDistance(Downed);
    if (NearestMate >= ThirstNoReviverRange)
    {
        return false;
    }
    // A squadmate right on the body could be reviving (or about to): keep sparing the downed and fight the reviver, as a
    // player would. Otherwise hold off for a few seconds so a revive can begin.
    const float Grace = NearestMate <= ThirstReviveRange ? ThirstReviveGrace : ThirstGrace;
    return Manager->DBNODownedElapsed(Downed, Now) < Grace;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::HasAmmoFor(const FCoreAthenaBotWeapon& Weapon) const
{
    const auto Found = Ammo.find(Weapon.AmmoDefinition);
    return Weapon.LoadedAmmo > 0 || (Found != Ammo.end() && Found->second > 0);
}

bool FCoreAthenaBotController::HasUsableWeapon() const
{
    return std::any_of(WeaponList.begin(), WeaponList.end(), [this](const FCoreAthenaBotWeapon& Weapon) { return HasAmmoFor(Weapon); });
}

int32_t FCoreAthenaBotController::BestWeaponIndex() const
{
    // Out of a fight: the weapon for a mid-range fight, the likeliest next one.
    const int32_t Index = WeaponIndexForRange(2000.0f);
    return Index >= 0 ? Index : WeaponList.empty() ? -1 : 0;
}

int32_t FCoreAthenaBotController::WeaponIndexForRange(float Distance) const
{
    const int32_t Band = Distance < 1000.0f ? 0 : Distance < 3500.0f ? 1 : 2;
    int32_t Best = -1;
    float BestScore = -1.0f;
    for (size_t Index = 0; Index < WeaponList.size(); ++Index)
    {
        const FCoreAthenaBotWeapon& Weapon = WeaponList[Index];
        if (!HasAmmoFor(Weapon))
        {
            continue;
        }
        float Score = RangePreference(Weapon.Type, Band) + std::min(DamagePerSecond(Weapon) / 200.0f, 1.0f);
        Score += Weapon.LoadedAmmo > 0 ? 1.0f : 0.0f;
        Score -= Distance > Weapon.EquipMaxDistance ? 3.0f : 0.0f;
        if (Score > BestScore)
        {
            BestScore = Score;
            Best = static_cast<int32_t>(Index);
        }
    }
    return Best;
}

int32_t FCoreAthenaBotController::WeaponSlotFor(UFortWeaponItemDefinition* Definition) const
{
    // One gun of each kind, the better one kept (IsBetterWeapon: a higher rarity, else more damage a second from the stats
    // row); with every slot full, a gun of a more wanted kind (assault rifle and shotgun, then SMG, sniper, pistol)
    // replaces the least wanted one.
    if (!Definition)
    {
        return -2;
    }
    const FCoreAthenaBotWeapon& Found = GetWeaponInfo(Definition);
    for (size_t Index = 0; Index < WeaponList.size(); ++Index)
    {
        const FCoreAthenaBotWeapon& Held = WeaponList[Index];
        if (Held.WeaponDefinition == Definition)
        {
            return -2;
        }
        if (Held.Type == Found.Type)
        {
            return IsBetterWeapon(Found, Held) ? static_cast<int32_t>(Index) : -2;
        }
    }
    if (WeaponList.size() < MaxWeapons)
    {
        return -1;
    }
    int32_t Worst = -2;
    float WorstPriority = TypePriority(Found.Type);
    for (size_t Index = 0; Index < WeaponList.size(); ++Index)
    {
        const float Priority = TypePriority(WeaponList[Index].Type);
        if (Priority < WorstPriority)
        {
            WorstPriority = Priority;
            Worst = static_cast<int32_t>(Index);
        }
    }
    return Worst;
}

const FCoreAthenaBotWeapon& FCoreAthenaBotController::GetWeaponInfo(UFortWeaponItemDefinition* Definition)
{
    static std::map<UFortWeaponItemDefinition*, FCoreAthenaBotWeapon> Cache;
    auto [Found, bNew] = Cache.try_emplace(Definition);
    if (bNew && Definition)
    {
        FCoreAthenaBotWeapon& Weapon = Found->second;
        Weapon.WeaponDefinition = KeepLoaded(Definition);
        Weapon.AmmoDefinition = KeepLoaded(static_cast<TAssetPtr<UFortWorldItemDefinition>&>(Definition->AmmoData).LoadSynchronous());
        ReadWeaponStats(Weapon);
        Weapon.Rarity = UFortItemDefinition::HasRarity() ? Definition->GetRarity() : 0;
        Log::Info(L"Battle Royale bots: {}: {} {} ({:.0f} damage x{}, {:.2f} shots/s, {} a magazine, {:.2f} s reload{}, equipped within {:.0f} m)", NameOf(Definition), RarityName(Weapon.Rarity), WeaponTypeName(Weapon.Type),
            Weapon.Damage, Weapon.Pellets, Weapon.FiringRate, Weapon.ClipSize, Weapon.ReloadTime, Weapon.bReloadIndividualBullets ? L" by the shell" : L"", std::min(Weapon.EquipMaxDistance, 99999.0f) / 100.0f);
    }
    return Found->second;
}

const FCoreAthenaBotConsumable* FCoreAthenaBotController::GetConsumableInfo(UFortItemDefinition* Definition)
{
    // A consumable is a ranged weapon that is its own ammo (Athena_Bandage's AmmoData is Athena_Bandage); what it gives
    // and how long it takes are its fire ability's blueprint defaults (the game's ability can't run for a bot: it isn't
    // an inventory owner, the same refusal as its guns).
    static std::map<UFortItemDefinition*, std::unique_ptr<FCoreAthenaBotConsumable>> Cache;
    auto [Found, bNew] = Cache.try_emplace(Definition);
    if (!bNew)
    {
        return Found->second.get();
    }
    KeepLoaded(Definition);
    UFortWeaponItemDefinition* Weapon = Definition ? Cast<UFortWeaponItemDefinition>(Definition) : nullptr;
    if (!Weapon || static_cast<TAssetPtr<UFortWorldItemDefinition>&>(Weapon->AmmoData).LoadSynchronous() != reinterpret_cast<UFortWorldItemDefinition*>(Weapon))
    {
        return nullptr;
    }
    UProperty* AbilityProperty = reinterpret_cast<UObject*>(Weapon)->GetClass()->FindPropertyByName(FName(L"PrimaryFireAbility", FNAME_Find));
    UClass* AbilityClass = AbilityProperty ? reinterpret_cast<TAssetSubclassOf<UObject>*>(reinterpret_cast<uint8_t*>(Weapon) + AbilityProperty->GetOffset_ForInternal())->LoadSynchronous() : nullptr;
    const UObject* Ability = AbilityClass ? AbilityClass->GetDefaultObject() : nullptr;
    if (!Ability)
    {
        return nullptr;
    }
    auto Info = std::make_unique<FCoreAthenaBotConsumable>();
    Info->ItemDefinition = Weapon;
    Info->Health = NumberOf(Ability, L"HealingAmount", 0.0f);
    Info->Health = Info->Health > 0.0f ? Info->Health : NumberOf(Ability, L"MaxHealingAmount", 0.0f);
    Info->HealthCap = NumberOf(Ability, L"HealingCap", 100.0f);
    Info->Shield = NumberOf(Ability, L"ShieldingAmount", 0.0f);
    Info->ShieldCap = NumberOf(Ability, L"SmallShieldCap", 100.0f);
    Info->UseTime = std::max(NumberOf(Ability, L"TriggerDuration", 2.0f), 0.5f);
    // The Chug Jug's ability gives its shield in its graph ("Gives full health and shield", Athena_SuperMedkit).
    if (AbilityClass->GetName().ToStdString().find(L"SuperMedkit") != std::wstring::npos)
    {
        Info->Shield = 100.0f;
    }
    Info->MaxStackSize = std::max(static_cast<int32_t>(Definition->MaxStackSize), 1);
    // The Slurp Juice (GA_Athena_PurpleStuff, HealingAmount 1) heals over time through its own effect: not one of these.
    if (Info->Health < 5.0f && Info->Shield < 5.0f)
    {
        return nullptr;
    }
    Log::Info(L"Battle Royale bots: {} heals {:.0f} (to {:.0f}) and shields {:.0f} (to {:.0f}) in {:.1f} s, {} a stack", NameOf(Definition), Info->Health, Info->HealthCap, Info->Shield, Info->ShieldCap, Info->UseTime, Info->MaxStackSize);
    Found->second = std::move(Info);
    return Found->second.get();
}

bool FCoreAthenaBotController::WantsPickup(UFortItemDefinition* Definition, bool bWeapon, bool bConsumable)
{
    if (bWeapon)
    {
        return WeaponSlotFor(Cast<UFortWeaponItemDefinition>(Definition)) != -2;
    }
    if (bConsumable)
    {
        const FCoreAthenaBotConsumable* Info = GetConsumableInfo(Definition);
        const auto Held = Consumables.find(Definition);
        return Info && (Held != Consumables.end() ? Held->second < Info->MaxStackSize : Consumables.size() < MaxConsumableKinds);
    }
    // Materials (wood, stone, metal on the floor: a chest's or an eliminated player's) up to the stack a player holds
    // (CoreV2 #252: searched houses kept their floor loot).
    if (const int32_t Kind = MaterialKindOf(Definition); Kind >= 0)
    {
        return Materials[Kind] < std::max(static_cast<int32_t>(Definition->MaxStackSize), 1);
    }
    // Ammo for its guns, a reserve of 120; with a free weapon slot, a little of the other kinds too, for the gun it
    // finds next (a player sweeps an ammo box's spill whole).
    const auto Reserve = Ammo.find(Definition);
    const int32_t Held = Reserve != Ammo.end() ? Reserve->second : 0;
    const bool bForItsGuns = std::any_of(WeaponList.begin(), WeaponList.end(), [Definition](const FCoreAthenaBotWeapon& Weapon) { return Weapon.AmmoDefinition == Definition; });
    return Cast<UFortAmmoItemDefinition>(Definition) && (bForItsGuns ? Held < 120 : WeaponList.size() < MaxWeapons && Held < 30);
}

int32_t FCoreAthenaBotController::MaterialKindOf(const UFortItemDefinition* Definition)
{
    // Asked for every floor pickup a bot looks over: the game data's three items are looked up once.
    static const UFortItemDefinition* Resources[3] = {};
    if (!Resources[0])
    {
        if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
        {
            Resources[0] = GameData->WoodItemDefinition;
            Resources[1] = GameData->StoneItemDefinition;
            Resources[2] = GameData->MetalItemDefinition;
        }
    }
    if (!Definition)
    {
        return -1;
    }
    for (int32_t Kind = 0; Kind < 3; ++Kind)
    {
        if (Resources[Kind] && Definition == Resources[Kind])
        {
            return Kind;
        }
    }
    return -1;
}

void FCoreAthenaBotController::EquipWeapon(int32_t Index)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Index < 0 || Index >= static_cast<int32_t>(WeaponList.size()))
    {
        return;
    }
    StopFiring();
    CancelReload();
    FCoreAthenaBotWeapon& Entry = WeaponList[static_cast<size_t>(Index)];
    AFortWeapon* Weapon = Pawn->EquipWeaponDefinition(Entry.WeaponDefinition, Entry.ItemGuid);
    if (!Weapon)
    {
        Log::Warning(L"Battle Royale bots: {} couldn't equip {}", BotName, NameOf(Entry.WeaponDefinition));
        return;
    }
    if (Entry.ClipSize <= 0)
    {
        Entry.ClipSize = std::max(BulletsPerClip(Weapon), 1);
    }
    Weapon->AmmoCount = Entry.LoadedAmmo;
    EquippedWeapon = Weapon;
    EquippedIndex = Index;
    bPickaxeEquipped = false;
    // A player can't fire a gun until it's out (its equip animation, about half a second 🟡).
    EquipReadyTime = LastTickTime + 0.5f;
#endif
}

bool FCoreAthenaBotController::IsUnderThreat(float Now) const
{
    AFortPawn* Target = CurrentTarget.Get();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    return Now - LastHurtTime < 2.0f || (Target && Pawn && bHasVisualLineOfSightOnTarget && (LocationOf(Target) - LocationOf(Pawn)).Size() < 2000.0f);
}

bool FCoreAthenaBotController::IsInCombat(float Now) const
{
    return (CurrentTarget.Get() && bHasVisualLineOfSightOnTarget) || Now - LastHurtTime < 4.0f;
}

void FCoreAthenaBotController::StartReload(float Now)
{
    if (EquippedIndex < 0 || EquippedIndex >= static_cast<int32_t>(WeaponList.size()))
    {
        return;
    }
    const FCoreAthenaBotWeapon& Entry = WeaponList[static_cast<size_t>(EquippedIndex)];
    const int32_t Missing = Entry.ClipSize - Entry.LoadedAmmo;
    float Time = Entry.ReloadTime;
    if (Entry.bReloadIndividualBullets && Entry.ClipSize > 0)
    {
        // A shell at a time 🟡 (ReloadTime taken as a full magazine's).
        Time = Entry.ReloadTime * static_cast<float>(std::min(Missing, std::max(Ammo[Entry.AmmoDefinition], 1))) / static_cast<float>(Entry.ClipSize);
    }
    StopFiring();
    ReloadIndex = EquippedIndex;
    ReloadUntil = Now + std::max(Time, 0.3f);
    // The reload as players see a player's: the weapon's ReloadAnimation (the pawn's montage).
    if (UObject* Montage = ObjectOf(reinterpret_cast<UObject*>(EquippedWeapon.Get()), L"ReloadAnimation"))
    {
        PlayReplicatedMontage(Montage, 1.0f);
    }
}

void FCoreAthenaBotController::CancelReload()
{
    if (ReloadIndex >= 0)
    {
        PlayReplicatedMontage(nullptr, 0.0f, true);
    }
    ReloadIndex = -1;
    ReloadUntil = 0.0f;
}

void FCoreAthenaBotController::ManageWeapon(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's _ManageWeapon: the weapon for the target's range (FFortAthenaInventoryInformation::EquipMaxDistance),
    // reloading from the bot's own ammo (no player controller's inventory to reload from) when the magazine is empty, or
    // out of a fight when it isn't full, and changing to a weapon with ammo.
    AFortWeapon* Weapon = EquippedWeapon.Get();
    if (bPickaxeEquipped || EquippedIndex < 0 || EquippedIndex >= static_cast<int32_t>(WeaponList.size()))
    {
        if (HasUsableWeapon())
        {
            EquipWeapon(BestWeaponIndex());
        }
        return;
    }
    FCoreAthenaBotWeapon& Entry = WeaponList[static_cast<size_t>(EquippedIndex)];
    if (!Weapon)
    {
        EquipWeapon(EquippedIndex);
        return;
    }
    Entry.LoadedAmmo = Weapon->AmmoCount;

    // A reload under way: the magazine fills once its time is up.
    if (ReloadIndex >= 0)
    {
        if (ReloadIndex != EquippedIndex)
        {
            CancelReload();
        }
        else if (Now >= ReloadUntil)
        {
            CancelReload();
            int32_t& Reserve = Ammo[Entry.AmmoDefinition];
            const int32_t Loaded = std::min(Reserve, std::max(Entry.ClipSize - Entry.LoadedAmmo, 0));
            if (Loaded > 0)
            {
                Reserve -= Loaded;
                Entry.LoadedAmmo += Loaded;
                Weapon->AmmoCount = Entry.LoadedAmmo;
                Weapon->ForceNetUpdate();
                ++Reloads;
            }
        }
        return;
    }

    // The weapon for the range, while fighting a target in sight (a change takes a moment before the first shot).
    AFortPawn* Target = CurrentTarget.Get();
    if (Target && bHasVisualLineOfSightOnTarget && Now >= NextWeaponSwitch)
    {
        const float Distance = (LocationOf(Target) - LastKnownPosition).Size();
        const int32_t ForRange = WeaponIndexForRange(Distance);
        if (ForRange >= 0 && ForRange != EquippedIndex)
        {
            const FCoreAthenaBotWeapon& Next = WeaponList[static_cast<size_t>(ForRange)];
            // A loaded weapon that suits the range as well stays out: only a clearly better one is worth the swap.
            const int32_t Band = Distance < 1000.0f ? 0 : Distance < 3500.0f ? 1 : 2;
            // A margin past the held gun's range keeps a target at the edge of it from swapping guns back and forth.
            if (Entry.LoadedAmmo == 0 || RangePreference(Next.Type, Band) > RangePreference(Entry.Type, Band) + 0.5f || Distance > Entry.EquipMaxDistance * 1.2f)
            {
                NextWeaponSwitch = Now + 3.0f;
                static int32_t LoggedSwitches = 0;
                if (LoggedSwitches++ < 10)
                {
                    Log::Info(L"Battle Royale bots: {} changes from its {} to its {} for a target {:.0f} m away", BotName, WeaponTypeName(Entry.Type), WeaponTypeName(Next.Type), Distance / 100.0f);
                }
                EquipWeapon(ForRange);
                ++WeaponSwitches;
                NextShotTime = std::max(NextShotTime, Now + 0.4f);
                return;
            }
        }
    }

    const auto Reserve = Ammo.find(Entry.AmmoDefinition);
    const bool bHasReserve = Reserve != Ammo.end() && Reserve->second > 0;
    if (Entry.LoadedAmmo <= 0)
    {
        if (bHasReserve)
        {
            StartReload(Now);
        }
        else if (HasUsableWeapon())
        {
            NextWeaponSwitch = Now + 1.0f;
            EquipWeapon(Target && bHasVisualLineOfSightOnTarget ? WeaponIndexForRange((LocationOf(Target) - LastKnownPosition).Size()) : BestWeaponIndex());
        }
        return;
    }
    // Out of a fight a player tops the magazine up, then the other guns.
    if (!IsInCombat(Now) && Now >= NextWeaponSwitch)
    {
        if (Entry.LoadedAmmo < Entry.ClipSize && bHasReserve)
        {
            StartReload(Now);
            return;
        }
        for (size_t Index = 0; Index < WeaponList.size(); ++Index)
        {
            const FCoreAthenaBotWeapon& Other = WeaponList[Index];
            const auto OtherReserve = Ammo.find(Other.AmmoDefinition);
            if (static_cast<int32_t>(Index) != EquippedIndex && Other.LoadedAmmo < Other.ClipSize && OtherReserve != Ammo.end() && OtherReserve->second > 0)
            {
                NextWeaponSwitch = Now + 1.0f;
                EquipWeapon(static_cast<int32_t>(Index));
                return;
            }
        }
        // Everything loaded: the gun for the likeliest fight in hand.
        const int32_t Best = BestWeaponIndex();
        if (Best >= 0 && Best != EquippedIndex)
        {
            NextWeaponSwitch = Now + 1.0f;
            EquipWeapon(Best);
        }
    }
#endif
}

void FCoreAthenaBotController::FireShot(AFortPawn* Target, float Distance)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortWeapon* Weapon = EquippedWeapon.Get();
    if (!Pawn || !Target)
    {
        return;
    }
    PlayFireMontage();
    const FCoreAthenaBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;

    // The game's own shot where its fire works for the weapon (or isn't known yet): its ability aims along the
    // controller's rotation (the focal point, with the aim error in it), spends the ammo, hits and shows the shot.
    const auto Known = Held ? WeaponFireWorks.find(Held->WeaponDefinition) : WeaponFireWorks.end();
    // Once one weapon's ability refuses a bot (3.0: no inventory owner), none is tried again.
    static bool bGameFireRefused = false;
    const bool bTryGameFire = !bGameFireRefused && Weapon && Held && Weapon->AmmoCount > 0 && (Known == WeaponFireWorks.end() || Known->second == 1);
    if (bTryGameFire)
    {
        const int32_t AmmoBefore = Weapon->AmmoCount;
        if (ActivateWeaponFire(Weapon))
        {
            ++NativeShots;
            if (Known == WeaponFireWorks.end() && WeaponFireCheckTime <= 0.0f)
            {
                WeaponFireAmmoBefore = AmmoBefore;
                WeaponFireCheckTime = Manager->GetGameMode()->GetWorld()->GetTimeSeconds() + 0.35f;
            }
            return;
        }
        if (Known == WeaponFireWorks.end())
        {
            WeaponFireWorks[Held->WeaponDefinition] = 0;
            bGameFireRefused = true;
            Log::Info(L"Battle Royale bots: {}'s {} didn't activate its fire ability: Core fires the bots' weapons and sends their fire cue", BotName, NameOf(Held->WeaponDefinition));
        }
    }
    Pawn->PawnStartFire(0);
    if (Weapon && Held && Weapon->AmmoCount > 0)
    {
        Weapon->AmmoCount = Weapon->AmmoCount - 1;
    }

    // The hit: the shot's line, from the bot's eyes through its aim point (where its tracking has the target, plus the
    // shot's aim error), passes within the target's capsule, and nothing stands between them.
    const FVector Location = LocationOf(Pawn);
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    const FVector TargetLocation = LocationOf(Target);
    const FVector Center(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 20.0f);
    const FVector Tracked = TrackedTarget.Get() == Target ? FVector(TrackedTargetLocation.X, TrackedTargetLocation.Y, TrackedTargetLocation.Z + 20.0f) : Center;
    const FVector Aim = Tracked + AimError;
    const FVector Ray = Aim - Eye;
    const float RayLength = Ray.Size();
    bool bHit = false;
    if (RayLength > 1.0f)
    {
        const FVector Direction = Ray * (1.0f / RayLength);
        const FVector ToTarget = Center - Eye;
        const float Along = ToTarget | Direction;
        const FVector Closest = Eye + Direction * Along;
        const float Horizontal = Dist2D(Closest, Center);
        bHit = Along > 0.0f && Horizontal < 45.0f && std::fabs(Closest.Z - Center.Z) < 90.0f;
    }
    // A wall, a build or the ground in the way stops the shot (both the Visibility and the weapon channel), and a
    // building piece it stops takes the shot's damage as a player's shot gives it.
    UObject* Blocker = nullptr;
    FVector BlockedAt;
    const FVector ShotEnd = RayLength > 1.0f ? Eye + Ray * ((std::max(RayLength, Distance) + 100.0f) / RayLength) : Center;
    const bool bClear = HasClearShot(Pawn, Eye, bHit ? Center : ShotEnd, Target, &Blocker, &BlockedAt);
    const bool bOnTarget = bHit;
    bHit = bHit && bClear;
    if (Held)
    {
        RecordShot(Held->Type, Distance, bHit);
    }
    if (Weapon && Held)
    {
        PlayWeaponFireCue(Weapon, bHit ? Center : !bClear ? BlockedAt : Eye + (RayLength > 1.0f ? Ray * (5000.0f / RayLength) : FVector()));
    }
    if (!bClear)
    {
        static int32_t LoggedBlocks = 0;
        ABuildingSMActor* Building = Blocker ? Cast<ABuildingSMActor>(Blocker) : nullptr;
        float BuildingDamage = 0.0f;
        if (Building && Weapon && !Building->bDestroyed && Building->GetHealth() > 0.0f)
        {
            DamageBuilding(Building, Weapon, BuildingDamage);
        }
        BlockedShots += bOnTarget ? 1 : 0;
        if (bOnTarget && LoggedBlocks++ < 8)
        {
            Log::Info(L"Battle Royale bots: {}'s shot at {} is stopped by {}{}", BotName, NameOf(reinterpret_cast<UObject*>(Target)), NameOf(Blocker),
                Building ? std::format(L" ({:.0f} damage to it)", BuildingDamage) : std::wstring());
        }
        return;
    }
    if (!bHit)
    {
        return;
    }
    float Damage = bPickaxeEquipped ? 20.0f : Held ? Held->Damage : 20.0f;
    int32_t PelletsHit = 1;
    if (Held && Held->Pellets > 1)
    {
        // A shotgun's pellets: all of them close up, fewer with the distance 🟡.
        const float Share = std::clamp(1.0f - (Distance - 400.0f) / 2000.0f, 0.25f, 1.0f);
        Damage *= static_cast<float>(Held->Pellets) * Share;
        PelletsHit = std::max(static_cast<int32_t>(std::lround(static_cast<float>(Held->Pellets) * Share)), 1);
    }
    // The game's damage where it decides the outcome itself (a player's pawn, and bots in solo playlists): the hit
    // numbers, the kill feed and the death are then the game's. Bots in team playlists take Core's damage, which downs
    // them (DBNO) while a teammate stands. Core's damage is the fallback when the game's deals none.
    float Dealt = 0.0f;
    if (Weapon && Manager->UsesGameDamage(Target))
    {
        Manager->NoteBotHit(this, Target);
        if (DamagePawn(Target, Weapon, PelletsHit, Dealt))
        {
            return;
        }
    }
    Manager->ApplyBotDamage(this, Target, Damage, Held ? Held->WeaponDefinition : nullptr);
#endif
}

void FCoreAthenaBotController::StopFiring()
{
    RemoveWeaponFireCue();
    EndShot();
}

void FCoreAthenaBotController::EndShot()
{
    if (!bFiring)
    {
        return;
    }
    bFiring = false;
    ReleaseWeaponFire();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        Pawn->PawnStopFire(0);
    }
}

void FCoreAthenaBotController::SetTargeting(bool bNewTargeting)
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || bNewTargeting == bTargeting)
    {
        return;
    }
    static UBoolProperty* TargetingProperty = static_cast<UBoolProperty*>(AFortPlayerPawn::StaticClass()->FindPropertyByName(FName(L"bIsTargeting", FNAME_Find)));
    if (!TargetingProperty)
    {
        return;
    }
    bTargeting = bNewTargeting;
    TargetingProperty->SetPropertyValue(reinterpret_cast<uint8_t*>(Pawn) + TargetingProperty->GetOffset_ForInternal(), bNewTargeting);
    Pawn->ForceNetUpdate();
}

bool FCoreAthenaBotController::ActivateWeaponFire(AFortWeapon* Weapon)
{
#if WITH_SERVER_CODE
    // A player's fire input activates the weapon's primary fire ability on their ability system (the player state's,
    // the pawn its avatar). The bot's controller is local to the server, so the ability runs here as a listen
    // server's own player's does.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    if (!AbilitySystem || !Weapon || !AFortWeapon::HasPrimaryAbilitySpecHandle())
    {
        return false;
    }
    const FGameplayAbilitySpecHandle Handle = Weapon->PrimaryAbilitySpecHandle;
    FGameplayAbilitySpec* Spec = Handle.IsValid() ? AbilitySystem->FindAbilitySpecFromHandle(Handle) : nullptr;
    static int32_t Logged = 0;
    if (!Spec)
    {
        if (Logged++ < 3)
        {
            Log::Info(L"Battle Royale bots: {}'s {} has no fire ability to activate (handle {}, {} abilities on {})", BotName, NameOf(static_cast<UObject*>(static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData))),
                Handle.Handle, AbilitySystem->ActivatableAbilities.Items.Num(), NameOf(reinterpret_cast<UObject*>(AbilitySystem)));
        }
        return false;
    }
    const bool bActivated = AbilitySystem->TryActivateAbility(Handle);
    if (!bActivated && Logged++ < 3)
    {
        Log::Info(L"Battle Royale bots: {}'s fire ability {} didn't activate (handle {})", BotName, NameOf(reinterpret_cast<UObject*>(static_cast<UGameplayAbility*>(Spec->Ability))), Handle.Handle);
    }
    bWeaponFireActive = bActivated;
    return bActivated;
#else
    return false;
#endif
}

void FCoreAthenaBotController::ReleaseWeaponFire()
{
#if WITH_SERVER_CODE
    // The fire input let go: an automatic weapon's ability waits for it (WaitInputRelease) and ends.
    if (!bWeaponFireActive)
    {
        return;
    }
    bWeaponFireActive = false;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortWeapon* Weapon = EquippedWeapon.Get();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    if (AbilitySystem && Weapon && AFortWeapon::HasPrimaryAbilitySpecHandle())
    {
        AbilitySystem->ServerSetInputReleased(Weapon->PrimaryAbilitySpecHandle);
    }
#endif
}

void FCoreAthenaBotController::PlayWeaponFireCue(AFortWeapon* Weapon, const FVector& ImpactLocation)
{
#if WITH_SERVER_CODE
    // The game's fire ability can't run for a bot: its cost check wants an inventory owner ("has a durability cost but
    // isn't being activated by an inventory owner"), which 8.30's AFortAthenaAIBotController is (it owns an
    // AFortInventory) and a stock AAIController isn't. So Core fires the shot and sends what the ability would show: its
    // WeaponFireCue, executed on every client through the pawn's multicast, as GA_Ranged_GenericDamage does.
    static UFunction* Multicast = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawnAthena:NetMulticast_InvokeGameplayCueExecuted_WithParams");
    static UScriptStruct* CueParametersStruct = FindObject<UScriptStruct>(nullptr, L"/Script/GameplayAbilities.GameplayCueParameters");
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    FGameplayAbilitySpec* Spec = AbilitySystem && AFortWeapon::HasPrimaryAbilitySpecHandle() ? AbilitySystem->FindAbilitySpecFromHandle(Weapon->PrimaryAbilitySpecHandle) : nullptr;
    const UObject* Ability = Spec ? reinterpret_cast<const UObject*>(static_cast<UGameplayAbility*>(Spec->Ability)) : nullptr;
    // A weapon whose ability the bot's ability system wasn't given (no spec for its handle) still has the cue its
    // ability class sends: the weapon data's PrimaryFireAbility default (CoreV2 #252: the bots' guns made no sound when
    // the spec lookup failed, as no cue went out at all).
    if (!Ability && Weapon)
    {
        UObject* Definition = reinterpret_cast<UObject*>(static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData));
        UProperty* AbilityProperty = Definition ? Definition->GetClass()->FindPropertyByName(FName(L"PrimaryFireAbility", FNAME_Find)) : nullptr;
        UClass* AbilityClass = AbilityProperty ? reinterpret_cast<TAssetSubclassOf<UObject>*>(reinterpret_cast<uint8_t*>(Definition) + AbilityProperty->GetOffset_ForInternal())->LoadSynchronous() : nullptr;
        Ability = AbilityClass ? AbilityClass->GetDefaultObject() : nullptr;
    }
    if (!Multicast || !CueParametersStruct || !Ability)
    {
        static int32_t LoggedMissing = 0;
        if (LoggedMissing++ < 3)
        {
            Log::Info(L"Battle Royale bots: {}'s shot sends no fire cue (multicast {}, cue parameters {}, ability {})", BotName, Multicast ? L"found" : L"missing",
                CueParametersStruct ? L"found" : L"missing", NameOf(Ability));
        }
        return;
    }
    auto Found = CueTags.find(Ability);
    if (Found == CueTags.end())
    {
        FName Tag(L"GameplayCue.Weapons.Activation", FNAME_Find);
        UProperty* CueProperty = Ability->GetClass()->FindPropertyByName(FName(L"WeaponFireCue", FNAME_Find));
        if (CueProperty)
        {
            const FGameplayTagContainer& Cues = *reinterpret_cast<const FGameplayTagContainer*>(reinterpret_cast<const uint8_t*>(Ability) + CueProperty->GetOffset_ForInternal());
            if (Cues.GameplayTags.Num() > 0)
            {
                Tag = Cues.GameplayTags[0].TagName;
            }
        }
        Found = CueTags.emplace(Ability, Tag).first;
        Log::Info(L"Battle Royale bots: {}'s fire cue is {}", NameOf(Ability), Tag.ToString().ToStdString());
    }
    if (Found->second.IsNone())
    {
        return;
    }
    // An automatic gun's fire effects loop from the first shot until the cue is removed (PlayWeaponFireFX keeps
    // bIsPlayingFireFX for a weapon with bPersistentFireFX), so its cue is held while the trigger is, not executed per
    // shot: an executed one started the loop on every client and nothing ever stopped it (CoreV2 #263: the bots' guns
    // kept their firing animation after they stopped shooting).
    // (Where the cue can't be added, the executed one goes out as before.)
    if (UsesPersistentFireFX(Weapon))
    {
        if (FireCueTag == Found->second && FireCueAbilitySystem.Get())
        {
            return;
        }
        RemoveWeaponFireCue();
        if (AddWeaponFireCue(Weapon, Found->second, ImpactLocation))
        {
            return;
        }
    }

    Reflection::FParams Params(Multicast, true);
    Params.Set(L"GameplayCueTag", FGameplayTag{ Found->second });
    uint8_t* CueParameters = static_cast<uint8_t*>(Params.GetAddress(L"GameplayCueParameters"));
    if (!CueParameters)
    {
        return;
    }
    FillWeaponCueParameters(CueParameters, Pawn, Weapon, ImpactLocation);
    // On each client the cue reaches the weapon through the pawn (3.3): the multicast's body (0xA25440) hands it to the
    // pawn's ability system, whose avatar's IGameplayCueInterface::HandleGameplayCue (0x54BF60) finds no handler for the
    // tag on the pawn but AFortPawn::GameplayCue, its catch-all for GameplayCue.* (0xE4D340), which forwards it to the
    // pawn's CurrentWeapon (ForwardGameplayCueToTarget); AFortWeapon::GameplayCue_Weapons_Activation then plays the shot's
    // sound and muzzle flash. So the weapon fired has to be the pawn's CurrentWeapon, as it is for a player.
    if (AFortPawn::HasCurrentWeapon() && static_cast<AFortWeapon*>(Pawn->CurrentWeapon) != Weapon)
    {
        static int32_t LoggedNotCurrent = 0;
        if (LoggedNotCurrent++ < 5)
        {
            Log::Info(L"Battle Royale bots: {}'s fire cue is for {}, but its pawn's CurrentWeapon is {}: clients play it on that one", BotName, NameOf(reinterpret_cast<UObject*>(Weapon)),
                NameOf(reinterpret_cast<UObject*>(static_cast<AFortWeapon*>(Pawn->CurrentWeapon))));
        }
    }
    Params.Call(reinterpret_cast<UObject*>(Pawn));
#endif
}

bool FCoreAthenaBotController::AddWeaponFireCue(AFortWeapon* Weapon, FName Tag, const FVector& ImpactLocation)
{
#if WITH_SERVER_CODE
    // UAbilitySystemComponent::AddGameplayCue_Internal into the ability system's ActiveGameplayCues, as the ranged fire
    // ability adds its WeaponFireCue. The cue reaches each client in the ability system's replication (its player
    // state's), which runs its WhileActive on the avatar: the pawn hands it to its CurrentWeapon (AFortPawn::GameplayCue,
    // 0xE4D340), which starts the looping fire effects for a pawn it doesn't control.
    static UProperty* CuesProperty = UAbilitySystemComponent::StaticClass()->FindPropertyByName(FName(L"ActiveGameplayCues", FNAME_Find));
    static UScriptStruct* CueStruct = FActiveGameplayCue::StaticStruct();
    static UProperty* ParametersProperty = CueStruct ? CueStruct->FindPropertyByName(FName(L"Parameters", FNAME_Find)) : nullptr;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    if (!AbilitySystem || !CuesProperty || !CueStruct || !ParametersProperty || Tag.IsNone())
    {
        static int32_t LoggedMissing = 0;
        if (LoggedMissing++ < 3)
        {
            Log::Info(L"Battle Royale bots: {}'s held fire cue can't be added (ability system {}, ActiveGameplayCues {}, ActiveGameplayCue {})", BotName,
                AbilitySystem ? L"found" : L"missing", CuesProperty ? L"found" : L"missing", CueStruct && ParametersProperty ? L"found" : L"missing");
        }
        return false;
    }
    FActiveGameplayCueContainer& Cues = *reinterpret_cast<FActiveGameplayCueContainer*>(reinterpret_cast<uint8_t*>(AbilitySystem) + CuesProperty->GetOffset_ForInternal());
    FActiveGameplayCue& Cue = Cues.AddCue(FGameplayTag{ Tag });
    FillWeaponCueParameters(reinterpret_cast<uint8_t*>(&Cue) + ParametersProperty->GetOffset_ForInternal(), Pawn, Weapon, ImpactLocation);
    FireCueTag = Tag;
    FireCueAbilitySystem = AbilitySystem;
    if (AActor* Owner = AbilitySystem->GetOwner())
    {
        Owner->ForceNetUpdate();
    }
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::RemoveWeaponFireCue()
{
#if WITH_SERVER_CODE
    // UAbilitySystemComponent::RemoveGameplayCue_Internal, as the ranged fire ability's EndAbility removes its
    // WeaponFireCue: each client runs the cue's Removed, and the weapon stops its fire effects (StopWeaponFireFX).
    if (FireCueTag.IsNone())
    {
        return;
    }
    const FName Tag = FireCueTag;
    FireCueTag = FName();
    FireCueUntil = 0.0f;
    UAbilitySystemComponent* AbilitySystem = FireCueAbilitySystem.Get();
    FireCueAbilitySystem = nullptr;
    static UProperty* CuesProperty = UAbilitySystemComponent::StaticClass()->FindPropertyByName(FName(L"ActiveGameplayCues", FNAME_Find));
    if (!AbilitySystem || !CuesProperty)
    {
        return;
    }
    FActiveGameplayCueContainer& Cues = *reinterpret_cast<FActiveGameplayCueContainer*>(reinterpret_cast<uint8_t*>(AbilitySystem) + CuesProperty->GetOffset_ForInternal());
    if (Cues.RemoveCue(FGameplayTag{ Tag }))
    {
        if (AActor* Owner = AbilitySystem->GetOwner())
        {
            Owner->ForceNetUpdate();
        }
    }
#endif
}

void FCoreAthenaBotController::CheckWeaponFireResult(float Now)
{
    // The first shot of a weapon class shows whether the game's fire works for a bot: its magazine went down.
    if (WeaponFireCheckTime <= 0.0f || Now < WeaponFireCheckTime)
    {
        return;
    }
    WeaponFireCheckTime = 0.0f;
    AFortWeapon* Weapon = EquippedWeapon.Get();
    const FCoreAthenaBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
    if (!Weapon || !Held || WeaponFireWorks.contains(Held->WeaponDefinition))
    {
        return;
    }
    const int32_t AmmoNow = Weapon->AmmoCount;
    const bool bWorks = AmmoNow < WeaponFireAmmoBefore;
    WeaponFireWorks[Held->WeaponDefinition] = bWorks ? 1 : 0;
    Log::Info(L"Battle Royale bots: {}'s first shot with {}: the game's fire {} ({} -> {} loaded, active ability {})", BotName, NameOf(Held->WeaponDefinition),
        bWorks ? L"works, the game fires it" : L"spent no ammo, Core fires it", WeaponFireAmmoBefore, AmmoNow, NameOf(static_cast<UObject*>(Weapon->ActiveAbility)));
}

bool FCoreAthenaBotController::RangeAttack(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's _RangeAttack (a ranged weapon at a target in sight; the destination is the target) and 21.40's
    // _MeleeAttack (the harvesting tool, close up). Aim: FCoreAIBotSkills' reaction time before the first shot, an error that
    // grows with the distance, and shots spaced by MaxTimeBetweenShots +- DeviationTimeBetweenShots.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPawn* Target = CurrentTarget.Get();
    AAIController* AIController = Controller.Get();
    if (!Target || !AIController || Target->bIsDying || !Pawn)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    const FVector TargetLocation = LocationOf(Target);
    const float Distance = (TargetLocation - Location).Size();
    const bool bArmed = HasUsableWeapon();
    // A player who boxed themselves in: the bot saw them a moment ago, and what blocks the view now is a player's
    // build right around them. It keeps at them and shoots through the walls (a shot a build stops damages the piece),
    // or swings its pickaxe at them, until they break.
    if (!bHasVisualLineOfSightOnTarget)
    {
        if (Now - LastTargetInSightTime > 15.0f || Distance > 3000.0f || !IsTargetBoxedIn(Target))
        {
            // Behind cover: while it remembers the enemy (UpdateCurrentTarget), an armed bot holds its aim on where it
            // last saw it, aiming down sights without firing, and closes in on that spot (CoreV2 #349). The storm comes
            // first.
            if (!bArmed || bHeadingForSafeZone || Now - LastTargetInSightTime >= TargetMemoryAge)
            {
                return false;
            }
            StopFiring();
            FaceTowards(FVector(LastSeenTargetLocation.X, LastSeenTargetLocation.Y, LastSeenTargetLocation.Z + 20.0f), Now, 0.5f);
            SetTargeting(!bPickaxeEquipped && ReloadUntil <= 0.0f && FacingError() < 45.0f);
            if (Dist2D(Location, LastSeenTargetLocation) > 300.0f)
            {
                MoveTowards(LastSeenTargetLocation, Now, 250.0f);
            }
            else
            {
                StopMoving();
            }
            return true;
        }
        if (!bBoxedInLogged)
        {
            bBoxedInLogged = true;
            Log::Info(L"Battle Royale bots: {} sees {} box themselves in {:.0f} m away and breaks in", BotName, NameOf(Target), Distance / 100.0f);
        }
    }
    else
    {
        bBoxedInLogged = false;
    }
    // Unarmed, a player loots first: they only swing their pickaxe back at someone hitting them right next to them
    // (bots that landed together brawled with their pickaxes, and half a lobby went out that way in the first zone).
    if (!bArmed && (Distance > 300.0f || Now - LastHurtTime > 3.0f))
    {
        return false;
    }

    // Reaction (21.40's TrackingReactionTime): a new target, or one seen again after a second out of sight, isn't shot
    // at before the bot has reacted to it (RangeAttack's bReady waits MaxTrackingReactionTime from TargetAcquiredTime).
    if (Target != TrackedTarget.Get() || Now - LastTargetInSightTime > 1.0f)
    {
        TrackedTarget = Target;
        TrackedTargetLocation = TargetLocation;
        TargetAcquiredTime = Now;
        LastTrackTime = Now;
    }
    // Tracking (TrackingInterpTime): the aim follows the target with a lag, so a target on the move (strafing, running
    // past) is aimed at where it was a moment ago and missed more, as a player's tracking falls behind.
    {
        const float Step = std::clamp(Now - LastTrackTime, 0.0f, 0.25f);
        const float Alpha = 1.0f - std::exp(-Step / std::max(AIBotSkills.MaxTrackingInterpTime, 0.02f));
        TrackedTargetLocation = TrackedTargetLocation + (TargetLocation - TrackedTargetLocation) * Alpha;
        LastTrackTime = Now;
    }
    // The aim error (FWeaponAccuracy's MaxTrackingOffsetError and the near and far distance errors), drawn for every
    // shot: a normal spread across the line of fire, a base plus a share that grows with the range, more beyond 25 m.
    if (Now >= NextShotTime || AimError.SizeSquared() == 0.0f)
    {
        const float Spread = AIBotSkills.MaxTrackingOffsetError + AIBotSkills.MaxTrackingDistanceNearError * std::min(Distance, 2500.0f) / 1000.0f
            + AIBotSkills.MaxTrackingDistanceFarError * std::max(Distance - 2500.0f, 0.0f) / 1000.0f;
        const auto Gaussian = []()
        {
            const float U1 = std::max(FMath::FRand(), 1e-6f);
            const float U2 = FMath::FRand();
            return std::sqrt(-2.0f * std::log(U1)) * std::cos(6.2831853f * U2);
        };
        const FVector Toward = Direction2D(Location, TargetLocation);
        const float Across = Gaussian() * Spread;
        const float Up = Gaussian() * Spread * AIBotSkills.MaxTrackingHeightOffsetError;
        AimError = FVector(-Toward.Y * Across, Toward.X * Across, Up);
    }
    FaceTowards(FVector(TrackedTargetLocation.X, TrackedTargetLocation.Y, TrackedTargetLocation.Z + 20.0f) + AimError, Now, 0.5f);
    LastTargetInSightTime = Now;

    // Keep a fighting distance: close in from afar, back off when too close (a shotgun or the pickaxe wants it close).
    // With the storm closing in (ShouldHeadForSafeZone), it fights on the way: it keeps heading in, shooting, instead
    // of standing (or chasing) in the storm (CoreV2 #233).
    const bool bRunningIn = bHeadingForSafeZone && Storm(Now);
    if (bRunningIn)
    {
        if (!bArmed && !bPickaxeEquipped)
        {
            EquipPickaxe();
        }
    }
    else if (!bArmed)
    {
        if (!bPickaxeEquipped)
        {
            UAthenaPickaxeItemDefinition* Pickaxe = LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe();
            if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
            {
                StopFiring();
                EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
                bPickaxeEquipped = true;
                EquippedIndex = -1;
            }
        }
        MoveTowards(TargetLocation, Now, 80.0f);
    }
    else if (Distance > 2500.0f)
    {
        MoveTowards(TargetLocation, Now, 1500.0f);
    }
    else
    {
        StopMoving();
        if (Distance < 600.0f)
        {
            Pawn->AddMovementInput(Direction2D(TargetLocation, Location), 0.6f, false);
        }
        // Strafing (the community list: bots strafe in fights): side to side across the line of fire, changing
        // direction every second or so.
        // The input eases from one side to the other (about a third of a second) instead of flipping in a frame.
        if (Now >= NextStrafeChange)
        {
            NextStrafeChange = Now + RandomRange(0.9f, 2.2f);
            StrafeSign = FMath::FRand() < 0.6f ? -StrafeSign : StrafeSign;
        }
        const float Step = std::min(std::max(Now - LastStrafeTime, 0.0f), 0.1f) * 3.0f;
        LastStrafeTime = Now;
        StrafeAmount = StrafeAmount < StrafeSign ? std::min(StrafeAmount + Step, StrafeSign) : std::max(StrafeAmount - Step, StrafeSign);
        const FVector Toward = Direction2D(Location, TargetLocation);
        SetMovementStyle(false, Now);
        if (std::fabs(StrafeAmount) > 0.05f)
        {
            Pawn->AddMovementInput(FVector(-Toward.Y * StrafeAmount, Toward.X * StrafeAmount, 0.0f), 0.8f, false);
        }
    }

    if (bFiring && Now >= FireUntil)
    {
        EndShot();
    }
    // A player aims down sights with a gun beyond arm's reach, not while reloading.
    SetTargeting(bArmed && !bPickaxeEquipped && ReloadUntil <= 0.0f && Distance > 400.0f && FacingError() < 45.0f);
    // A player turns to face a target before shooting at it.
    const FCoreAthenaBotWeapon* Ready = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
    const bool bGunReady = bPickaxeEquipped || (Now >= EquipReadyTime && (!Ready || Now >= Ready->ReadyTime));
    const bool bReady = Now >= TargetAcquiredTime + AIBotSkills.MaxTrackingReactionTime && Now >= NextShotTime && bGunReady && ReloadUntil <= 0.0f && FacingError() < 20.0f;
    const bool bLoaded = bPickaxeEquipped ? Distance < 250.0f : (EquippedWeapon.Get() && EquippedWeapon.Get()->AmmoCount > 0);
    if (bReady && bLoaded && !bFiring)
    {
        FireShot(Target, Distance);
        bFiring = true;
        if (ShotsFired++ == 0)
        {
            Log::Info(L"Battle Royale bots: {} opens fire on {} {:.0f} m away with {} ({} loaded)", BotName, NameOf(Target), Distance / 100.0f,
                EquippedWeapon.Get() ? NameOf(static_cast<UFortWeaponItemDefinition*>(EquippedWeapon.Get()->WeaponData)) : std::wstring(L"nothing"), EquippedWeapon.Get() ? static_cast<int32_t>(EquippedWeapon.Get()->AmmoCount) : 0);
        }
        FireUntil = Now + 0.12f;
        FCoreAthenaBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
        // Never faster than the gun itself (its stats row's FiringRate: a pump's or a bolt's delay is in it), with the
        // bot's level-based spacing on top of that.
        const float WeaponInterval = bPickaxeEquipped ? 0.8f : Held && Held->FiringRate > 0.0f ? 1.0f / Held->FiringRate : 0.5f;
        if (Held)
        {
            Held->ReadyTime = Now + WeaponInterval;
        }
        NextShotTime = Now + WeaponInterval + std::max(AIBotSkills.MaxTimeBetweenShots + RandomRange(-AIBotSkills.DeviationTimeBetweenShots, AIBotSkills.DeviationTimeBetweenShots) - WeaponInterval, 0.0f) + RandomRange(0.0f, 0.06f);
        // A held fire cue lasts while the trigger would be: through to the next shot when it follows at about the gun's
        // own rate, else for one shot's interval.
        if (!FireCueTag.IsNone())
        {
            FireCueUntil = std::min(NextShotTime, Now + WeaponInterval * 1.5f) + 0.05f;
        }
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::IsTargetBoxedIn(AFortPawn* Target) const
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !Target)
    {
        return false;
    }
    const FVector From = LocationOf(Pawn) + FVector(0.0f, 0.0f, 50.0f);
    const FVector To = LocationOf(Target) + FVector(0.0f, 0.0f, 20.0f);
    FVector Hit;
    UObject* HitActor = nullptr;
    UObject* HitComponent = nullptr;
    if (!UKismetSystemLibrary::LineTraceSingle(Pawn, From, To, 0, false, true, Hit, HitActor, HitComponent))
    {
        return false;
    }
    ABuildingSMActor* Building = HitActor ? Cast<ABuildingSMActor>(HitActor) : nullptr;
    // A player's piece (a wall, floor, ramp or roof), within a tile of the target.
    return Building && Building->bPlayerPlaced && !Building->bDestroyed && (Hit - To).Size() < 600.0f;
}

bool FCoreAthenaBotController::Loot(float Now, float MaxDistance)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Loot (GatherNearestLoot / ComputeLootScore), for weapons and their ammo on the
    // floor: up to two weapons, then ammo for them, nearest first.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    AFortPickup* Target = LootTarget.Get();
    if (Target && (Target->bPickedUp || Target->IsPendingKill()))
    {
        Target = nullptr;
        LootTarget = nullptr;
    }
    // The nearest wanted pickup, looked for every second. The one the bot is going for (its goal) is kept unless another
    // is less than half as far; one it only noted (farther than this call looks) gives way to whatever is nearest.
    const bool bCommitted = Goal == ECoreAthenaBotGoal::Loot;
    if (Now >= NextLootSearch && (!Target || !bCommitted || Dist2D(LocationOf(Target), Location) > 1000.0f))
    {
        NextLootSearch = Now + 1.0f;
        float Best = 7000.0f;
        AFortPickup* Nearest = nullptr;
        FVector LeashAround;
        float LeashRadius = 0.0f;
        const bool bLeashed = GetSquadLeash(LeashAround, LeashRadius);
        for (const FCoreAthenaBotLoot& Entry : Manager->GetLoot())
        {
            AFortPickup* Pickup = Entry.Pickup.Get();
            if (!Pickup || Pickup->bPickedUp || IsUnreachable(Pickup) || (Pickup != Target && IsTakenByTeammate(Pickup)) || (bLeashed && Dist2D(Entry.Location, LeashAround) > LeashRadius))
            {
                continue;
            }
            FFortItemEntry& PickupEntry = Pickup->PrimaryPickupItemEntry;
            const float Distance = (Entry.Location - Location).Size();
            if (Distance < Best && std::fabs(Entry.Location.Z - Location.Z) < 800.0f && WantsPickup(PickupEntry.ItemDefinition, Entry.bWeapon, Entry.bConsumable))
            {
                Best = Distance;
                Nearest = Pickup;
            }
        }
        if (Nearest != Target && (!Target || !bCommitted || Best < (LocationOf(Target) - Location).Size() * 0.5f))
        {
            Target = Nearest;
            LootTargetTime = Now;
            LastLootPursuit = Now;
            LootTarget = Target;
        }
    }
    if (!Target)
    {
        return false;
    }
    const FVector PickupLocation = LocationOf(Target);
    // Farther than this call looks: a container nearer may come first (the target is kept for the next call).
    if (Dist2D(PickupLocation, Location) > MaxDistance)
    {
        return false;
    }
    // The time spent going for it counts (not the time the bot spent on a container meanwhile).
    LootTargetTime += Now - LastLootPursuit > 0.5f ? Now - LastLootPursuit : 0.0f;
    LastLootPursuit = Now;
    // Not reached in 20 s of going for it (behind a closed door, up on a roof): given up.
    // The next pickup is looked for at once when this one is taken or given up (a second's wait sent the bot off to
    // its next goal with loot left around it).
    if (Now - LootTargetTime > 20.0f)
    {
        MarkUnreachable(Target);
        LootTarget = nullptr;
        NextLootSearch = 0.0f;
        return false;
    }
    if (Dist2D(PickupLocation, Location) < 200.0f && std::fabs(PickupLocation.Z - Location.Z) < 250.0f)
    {
        TakePickup(Target);
        LootTarget = nullptr;
        NextLootSearch = 0.0f;
        return true;
    }
    // Right above or below it (another floor, a roof): out of reach for a bot without a path. With a navmesh the path
    // takes it up the stairs, so it is given a few seconds first (CoreV2 #252: ammo on an upper floor was given up as
    // the bot walked under it).
    if (Dist2D(PickupLocation, Location) < 120.0f && (!Manager->HasNavigation() || Now - LootTargetTime > 8.0f))
    {
        // On the roof (or floor) over it: the bot breaks through to it (CoreV2 #275).
        if (BreakDownFromRoof(Now, PickupLocation))
        {
            LootTargetTime = Now;
            return true;
        }
        MarkUnreachable(Target);
        LootTarget = nullptr;
        NextLootSearch = 0.0f;
        return false;
    }
    MoveTowards(PickupLocation, Now, 50.0f);
    return true;
#else
    return false;
#endif
}

namespace
{
    std::vector<UFortMontageItemDefinitionBase*>& BotEmotes()
    {
        static std::vector<UFortMontageItemDefinitionBase*> EmoteAssets;
        static bool bLoaded = false;
        if (!bLoaded)
        {
            bLoaded = true;
            for (const wchar_t* Path : { L"/Game/Athena/Items/Cosmetics/Dances/EID_DanceMoves.EID_DanceMoves", L"/Game/Athena/Items/Cosmetics/Dances/EID_Floss.EID_Floss",
                     L"/Game/Athena/Items/Cosmetics/Dances/EID_Fresh.EID_Fresh", L"/Game/Athena/Items/Cosmetics/Dances/EID_BreakDance.EID_BreakDance" })
            {
                if (UFortMontageItemDefinitionBase* Emote = KeepLoaded(LoadObject<UFortMontageItemDefinitionBase>(nullptr, Path)))
                {
                    EmoteAssets.push_back(Emote);
                }
            }
        }
        return EmoteAssets;
    }
}

void FCoreAthenaBotController::StopEmote()
{
#if WITH_SERVER_CODE
    if (!bEmoting)
    {
        return;
    }
    bEmoting = false;
    EmoteUntil = 0.0f;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = GameData ? GameData->EmoteGameplayAbility.LoadSynchronous() : nullptr;
    FGameplayAbilitySpec* Spec = AbilitySystem && AbilityClass ? AbilitySystem->FindAbilitySpecFromClass(AbilityClass) : nullptr;
    if (!Spec)
    {
        return;
    }
    for (const TArray<UGameplayAbility*>* Instances : { &static_cast<const TArray<UGameplayAbility*>&>(Spec->ReplicatedInstances), &static_cast<const TArray<UGameplayAbility*>&>(Spec->NonReplicatedInstances) })
    {
        for (int32_t Index = Instances->Num() - 1; Index >= 0; --Index)
        {
            if (UGameplayAbility* Instance = (*Instances)[Index])
            {
                Instance->K2_EndAbility();
                break;
            }
        }
    }
#endif
}

bool FCoreAthenaBotController::IsDanceStunned() const
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UClass* AbilityClass = DanceStunAbilityClass();
    FGameplayAbilitySpec* Spec = AbilitySystem && AbilityClass ? AbilitySystem->FindAbilitySpecFromClass(AbilityClass) : nullptr;
    return Spec && Spec->ActiveCount > 0;
}

void FCoreAthenaBotController::PreloadAssets()
{
#if WITH_SERVER_CODE
    const auto Started = std::chrono::steady_clock::now();
    for (UFortMontageItemDefinitionBase* Emote : BotEmotes())
    {
        Emote->Animation.LoadSynchronous();
    }
    if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
    {
        GameData->EmoteGameplayAbility.LoadSynchronous();
    }
    Log::Info(L"Battle Royale bots: {} emotes loaded up front ({:.0f} ms)", BotEmotes().size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count());
#endif
}

bool FCoreAthenaBotController::PlayEmote(float Now, float Duration)
{
#if WITH_SERVER_CODE
    std::vector<UFortMontageItemDefinitionBase*>& EmoteAssets = BotEmotes();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = GameData ? GameData->EmoteGameplayAbility.LoadSynchronous() : nullptr;
    UGameplayAbility* Ability = AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()) ? static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject()) : nullptr;
    if (!AbilitySystem || !Ability || EmoteAssets.empty())
    {
        return false;
    }
    // A player stands still to emote: a bot still moving waits (it slid along dancing).
    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
    StopMoving();
    if (Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 40.0f * 40.0f)
    {
        return false;
    }
    UFortMontageItemDefinitionBase* Emote = EmoteAssets[static_cast<size_t>(FMath::Rand()) % EmoteAssets.size()];
    Emote->Animation.LoadSynchronous();
    StopFiring();
    UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
    std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
    SpecStruct->InitializeStruct(SpecStorage.data());
    FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
    FGameplayAbilitySpecHandle NewHandle;
    NewHandle.GenerateNewHandle();
    Spec.Handle = NewHandle;
    Spec.Ability = Ability;
    Spec.Level = 1;
    Spec.InputID = INDEX_NONE;
    Spec.SourceObject = Emote;
    const FGameplayAbilitySpecHandle Handle = AbilitySystem->GiveAbilityAndActivateOnce(Spec);
    SpecStruct->DestroyStruct(SpecStorage.data());
    // Whether the ability ran: a given-once ability that activated is still on the ability system (until it ends).
    const bool bPlaying = Handle.IsValid() && AbilitySystem->FindAbilitySpecFromHandle(Handle) != nullptr;
    static int32_t Logged = 0;
    if (Logged++ < 6)
    {
        Log::Info(L"Battle Royale bots: {} emotes {} ({})", BotName, NameOf(Emote), bPlaying ? L"playing" : L"the emote ability didn't stay active");
    }
    if (!bPlaying)
    {
        return false;
    }
    ++Emotes;
    EmoteUntil = Now + Duration;
    bEmoting = true;
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::TakePickup(AFortPickup* Pickup)
{
#if WITH_SERVER_CODE
    // Into the bot's own inventory (8.30 keeps it in the controller's AFortInventory; Core's brain holds it), and the
    // pickup is gone as when a player takes it.
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    UFortItemDefinition* Definition = Entry.ItemDefinition;
    const int32_t Count = std::max(static_cast<int32_t>(Entry.Count), 1);
    if (const FCoreAthenaBotConsumable* Consumable = GetConsumableInfo(Definition))
    {
        // As many as the stack holds; the rest stays on the floor.
        int32_t& Held = Consumables[Definition];
        const int32_t Taken = std::min(Count, std::max(Consumable->MaxStackSize - Held, 0));
        Held += Taken;
        if (Taken < Count)
        {
            DropItem(Definition, Count - Taken, 0);
        }
        Log::Info(L"Battle Royale bots: {} picks up {} {} ({} held)", BotName, Taken, NameOf(Definition), Held);
    }
    else if (UFortWeaponItemDefinition* WeaponDefinition = Cast<UFortWeaponItemDefinition>(Definition))
    {
        const int32_t Slot = WeaponSlotFor(WeaponDefinition);
        if (Slot == -2)
        {
            MarkUnreachable(Pickup);
            return;
        }
        FCoreAthenaBotWeapon Weapon = GetWeaponInfo(WeaponDefinition);
        Weapon.ItemGuid = FGuid::NewGuid();
        Weapon.LoadedAmmo = std::max(static_cast<int32_t>(Entry.LoadedAmmo), 0);
        // A weapon found empty with no ammo to go with it gets a magazine 🟡.
        if (Weapon.LoadedAmmo == 0 && Ammo[Weapon.AmmoDefinition] == 0)
        {
            Ammo[Weapon.AmmoDefinition] += std::max(Weapon.ClipSize, 10);
        }
        // A full inventory swaps: the weapon it replaces drops where the bot stands, as a player's does.
        int32_t Index = static_cast<int32_t>(WeaponList.size());
        if (Slot >= 0)
        {
            const FCoreAthenaBotWeapon& Replaced = WeaponList[static_cast<size_t>(Slot)];
            Log::Info(L"Battle Royale bots: {} swaps its {} {} for a {} {}", BotName, RarityName(Replaced.Rarity), NameOf(Replaced.WeaponDefinition), RarityName(Weapon.Rarity), NameOf(WeaponDefinition));
            DropItem(Replaced.WeaponDefinition, 1, Replaced.LoadedAmmo);
            WeaponList[static_cast<size_t>(Slot)] = Weapon;
            Index = Slot;
        }
        else
        {
            WeaponList.push_back(Weapon);
        }
        EquippedIndex = -1;
        EquipWeapon(Index);
        Log::Info(L"Battle Royale bots: {} picks up {} ({}, {} loaded, {} ammo held)", BotName, NameOf(WeaponDefinition), WeaponTypeName(Weapon.Type), Weapon.LoadedAmmo, Ammo[Weapon.AmmoDefinition]);
    }
    else if (const int32_t Kind = MaterialKindOf(Definition); Kind >= 0)
    {
        // Up to the stack; the rest stays on the floor.
        const int32_t MaxStack = std::max(static_cast<int32_t>(Definition->MaxStackSize), 1);
        const int32_t Taken = std::min(Count, std::max(MaxStack - Materials[Kind], 0));
        Materials[Kind] += Taken;
        if (Taken < Count)
        {
            DropItem(Definition, Count - Taken, 0);
        }
        Log::Info(L"Battle Royale bots: {} picks up {} {} ({} wood, {} stone, {} metal)", BotName, Taken, NameOf(Definition), Materials[0], Materials[1], Materials[2]);
    }
    else
    {
        Ammo[Definition] += Count;
        Log::Info(L"Battle Royale bots: {} picks up {} {}", BotName, Count, NameOf(Definition));
    }
    Pickup->bPickedUp = true;
    Manager->ForgetLoot(Pickup);
    Pickup->K2_DestroyActor();
#endif
}

bool FCoreAthenaBotController::Storm(float Now)
{
    // 8.30's _Storm: the safe zone's phase and state feed the behaviour tree, which heads for the next circle.
    FVector Center;
    float Radius = 0.0f;
    if (!Manager->GetSafeZone(Center, Radius))
    {
        return false;
    }
    const FVector Location = LocationOf(GetPawn());
    // Once on its way in, it goes well inside (it stopped at the edge, wandered out and turned back in).
    if (Dist2D(Location, Center) <= Radius * (Goal == ECoreAthenaBotGoal::Storm ? 0.75f : 0.95f))
    {
        return false;
    }
    // A point well inside the circle on the bot's side of it.
    const FVector Inward = Direction2D(Center, Location);
    const FVector Goal(Center.X + Inward.X * Radius * 0.4f, Center.Y + Inward.Y * Radius * 0.4f, Location.Z);
    MoveTowards(Goal, Now, 300.0f);
    return true;
}

bool FCoreAthenaBotController::ShouldHeadForSafeZone(float Now) const
{
    // Inside the next circle with room to spare, nothing to run from.
    FVector Center;
    float Radius = 0.0f;
    if (!Manager->GetSafeZone(Center, Radius))
    {
        return false;
    }
    const float Remaining = Dist2D(LastKnownPosition, Center) - Radius * 0.9f;
    if (Remaining <= 0.0f)
    {
        return false;
    }
    // In the storm now: out of it first.
    FVector StormCenter;
    float StormRadius = 0.0f;
    if (Manager->GetCurrentSafeZone(StormCenter, StormRadius) && Dist2D(LastKnownPosition, StormCenter) > StormRadius * 0.97f)
    {
        return true;
    }
    // The storm's edge closes in from where it is to the next circle by SafeZoneFinishShrinkTime (it waits, then
    // shrinks at a steady pace): a bot inside the circle now that heads straight in, getting there by then, stays ahead
    // of it all the way. So it sets off once the time left is what the run takes: at a pace below a sprint's (paths
    // aren't straight) 🟡, with 15 s to spare 🟡. A circle that finished closing has its next one set at the next phase.
    const float Finish = Manager->GetSafeZoneFinishShrinkTime();
    if (Finish <= Now || Finish - Now > 1800.0f)
    {
        return false;
    }
    constexpr float RunSpeed = 400.0f;
    constexpr float Spare = 15.0f;
    return Now + Remaining / RunSpeed + Spare >= Finish;
}

void FCoreAthenaBotController::Wander(float Now)
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    if (!bHasWanderGoal || Dist2D(Location, WanderGoal) < 200.0f || Now >= NextWanderTime)
    {
        float Range = State == ECoreAthenaBotState::Warmup ? 800.0f : 2500.0f;
        FVector Center;
        float Radius = 0.0f;
        FVector Around = Location;
        const bool bHasZone = State != ECoreAthenaBotState::Warmup && Manager->GetSafeZone(Center, Radius);
        if (bHasZone && Dist2D(Location, Center) > Radius * 0.5f)
        {
            Around = FVector(Location.X + (Center.X - Location.X) * 0.3f, Location.Y + (Center.Y - Location.Y) * 0.3f, Location.Z);
        }
        // A bot on a player's team stays with its players (StayWithTeam calls it back past 25 m): it wanders around the
        // nearest one standing, within 10 m of them. Onwards, it went out 25 m, was called back, and went out again: back
        // and forth by its player (CoreV2 #264).
        bool bByPlayer = false;
        if (State != ECoreAthenaBotState::Warmup)
        {
            float Nearest = 1e9f;
            for (AFortPlayerPawnAthena* Player : Manager->PlayerTeammatesOf(this))
            {
                if (!Player->bIsDBNO && !Player->bIsSkydiving && !Player->bIsParachuteOpen && Dist2D(LocationOf(Player), Location) < Nearest)
                {
                    Nearest = Dist2D(LocationOf(Player), Location);
                    Around = LocationOf(Player);
                    bByPlayer = true;
                }
            }
        }
        if (bByPlayer)
        {
            Range = 1800.0f;
        }
        // On the ground, onwards: the next goal within 60 degrees of the way it was going (a random one each time sent it
        // back the way it came), turned back towards the middle of the safe zone near its edge.
        float Angle = RandomRange(0.0f, 6.2831853f);
        UCharacterMovementComponent* Movement = Pawn ? static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement : nullptr;
        const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
        if (!bByPlayer && State != ECoreAthenaBotState::Warmup && (bHasWanderGoal || Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 100.0f * 100.0f))
        {
            const FVector Heading = Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 100.0f * 100.0f ? Direction2D(FVector(), FVector(Velocity.X, Velocity.Y, 0.0f)) : Direction2D(Location, WanderGoal);
            if (Heading.SizeSquared() > 0.01f)
            {
                Angle = std::atan2(Heading.Y, Heading.X) + RandomRange(-1.05f, 1.05f);
            }
        }
        float Distance = RandomRange(Range * 0.3f, Range);
        WanderGoal = FVector(Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, Location.Z);
        // By its player, a squad spreads out as players do: of a few spots around them, the one farthest from where its
        // bot teammates are and are going (CoreV2 #275: all of them wandered within 10 m of the player, on top of each
        // other, and moved as one).
        if (bByPlayer)
        {
            std::vector<FVector> Taken{ Around };
            for (const FCoreAthenaBotController* Teammate : Manager->TeammatesOf(this))
            {
                FVector TheirGoal;
                if (Teammate->GetState() != ECoreAthenaBotState::Landed || !Teammate->GetPawn())
                {
                    continue;
                }
                Taken.push_back(LocationOf(Teammate->GetPawn()));
                if (Teammate->GetWanderGoal(TheirGoal))
                {
                    Taken.push_back(TheirGoal);
                }
            }
            float BestSpacing = -1.0f;
            for (int32_t Try = 0; Try < 6; ++Try)
            {
                const float TryAngle = RandomRange(0.0f, 6.2831853f);
                const float TryDistance = RandomRange(Range * 0.3f, Range);
                const FVector Candidate(Around.X + std::cos(TryAngle) * TryDistance, Around.Y + std::sin(TryAngle) * TryDistance, Location.Z);
                float Spacing = 1e9f;
                for (const FVector& Other : Taken)
                {
                    Spacing = std::min(Spacing, Dist2D(Candidate, Other));
                }
                if (Spacing > BestSpacing)
                {
                    BestSpacing = Spacing;
                    WanderGoal = Candidate;
                    Distance = TryDistance;
                }
            }
        }
        if (bHasZone && Dist2D(WanderGoal, Center) > Radius * 0.8f)
        {
            const FVector Inward = Direction2D(Location, Center);
            WanderGoal = FVector(Location.X + Inward.X * Distance, Location.Y + Inward.Y * Distance, Location.Z);
        }
        bHasWanderGoal = true;
        NextWanderTime = Now + (State == ECoreAthenaBotState::Warmup ? RandomRange(5.0f, 9.0f) : 15.0f);
    }
    MoveTowards(WanderGoal, Now, 150.0f);
}

void FCoreAthenaBotController::SetGoal(ECoreAthenaBotGoal NewGoal, float Now)
{
    if (NewGoal != Goal)
    {
        Goal = NewGoal;
        GoalStartTime = Now;
        ++GoalChanges;
    }
}

bool FCoreAthenaBotController::PursueGoal(ECoreAthenaBotGoal InGoal, float Now)
{
    switch (InGoal)
    {
    case ECoreAthenaBotGoal::Loot: return Loot(Now, 1e9f);
    case ECoreAthenaBotGoal::Search: return SearchContainer(Now);
    case ECoreAthenaBotGoal::Storm: return Storm(Now);
    case ECoreAthenaBotGoal::Regroup: return StayWithTeam(Now);
    case ECoreAthenaBotGoal::Harvest: return Harvest(Now);
    case ECoreAthenaBotGoal::Hunt: return Investigate(Now);
    case ECoreAthenaBotGoal::TravelToPOI: return SelectNextPOI(Now);
    case ECoreAthenaBotGoal::Wander: Wander(Now); return true;
    default: return false;
    }
}

const wchar_t* FCoreAthenaBotController::ChooseGoal(float Now)
{
    // The goal the bot set out on comes first while it applies: the pickup or container it is going for, the circle, a
    // teammate it is going back to, the building it is breaking. Then a new one: the next circle, a container, the
    // team, loot farther away, materials. With none of those left (the spot looted out), a goal that takes it
    // somewhere: a player near it (Investigate), the next POI (SelectNextPOI), or onwards (Wander); these also hold
    // until they're done, unless loot or a container shows up meanwhile.
    switch (Goal)
    {
    case ECoreAthenaBotGoal::Loot:
    case ECoreAthenaBotGoal::Search:
    case ECoreAthenaBotGoal::Storm:
    case ECoreAthenaBotGoal::Regroup:
    case ECoreAthenaBotGoal::Harvest:
        if (PursueGoal(Goal, Now))
        {
            return GoalName(Goal);
        }
        break;
    default:
        break;
    }
    for (const ECoreAthenaBotGoal Next : { ECoreAthenaBotGoal::Storm, ECoreAthenaBotGoal::Search, ECoreAthenaBotGoal::Regroup, ECoreAthenaBotGoal::Loot, ECoreAthenaBotGoal::Harvest })
    {
        // Materials wait until the bot has a gun (a bot that landed empty-handed chopped trees instead of arming itself).
        if (Next == ECoreAthenaBotGoal::Harvest && !HasUsableWeapon())
        {
            continue;
        }
        if (Next != Goal && PursueGoal(Next, Now))
        {
            SetGoal(Next, Now);
            return GoalName(Next);
        }
    }
    if (Now < DetourUntil)
    {
        Wander(Now);
        return L"detour";
    }
    if ((Goal == ECoreAthenaBotGoal::Hunt || Goal == ECoreAthenaBotGoal::TravelToPOI) && PursueGoal(Goal, Now))
    {
        return GoalName(Goal);
    }
    for (const ECoreAthenaBotGoal Next : { ECoreAthenaBotGoal::Hunt, ECoreAthenaBotGoal::TravelToPOI, ECoreAthenaBotGoal::Wander })
    {
        if (Next != Goal && PursueGoal(Next, Now))
        {
            SetGoal(Next, Now);
            return GoalName(Next);
        }
    }
    // Wandering already (it is always possible).
    Wander(Now);
    SetGoal(ECoreAthenaBotGoal::Wander, Now);
    return GoalName(ECoreAthenaBotGoal::Wander);
}

bool FCoreAthenaBotController::Investigate(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Investigate (GetLastKnownPosition, SetDestination): an armed bot (two guns with
    // ammo) heads for a player within 100 m inside the safe zone, at most three bots for one player; other bots are left
    // to meet it by chance (WillEngageBot). Once the player is in sight, RangeAttack fights them.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const int32_t Armed = static_cast<int32_t>(std::count_if(WeaponList.begin(), WeaponList.end(), [this](const FCoreAthenaBotWeapon& Weapon) { return HasAmmoFor(Weapon); }));
    // A bot on a player's team stays with its players: it fights what comes to them rather than going hunting.
    if (!Pawn || Armed < 2 || !Manager->PlayerTeammatesOf(this).empty())
    {
        HuntTarget = nullptr;
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    FVector Center;
    float Radius = 0.0f;
    const bool bHasZone = Manager->GetSafeZone(Center, Radius);
    const auto Huntable = [&](AFortPawn* Other, float Range)
    {
        return Other && !Other->bIsDying && !Other->IsPendingKill() && Manager->TeamOfPawn(Other) != Team && Dist2D(LocationOf(Other), Location) < Range
            && (!bHasZone || Dist2D(LocationOf(Other), Center) < Radius);
    };
    AFortPawn* Target = HuntTarget.Get();
    if (Target && !Huntable(Target, HuntRange * 1.5f))
    {
        Target = nullptr;
        HuntTarget = nullptr;
    }
    if (!Target)
    {
        if (Now < NextHuntSearch)
        {
            return false;
        }
        NextHuntSearch = Now + 3.0f;
        float Best = HuntRange;
        for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
        {
            AFortPawn* Other = Weak.Get();
            if (!Huntable(Other, Best) || Manager->FindBot(reinterpret_cast<UObject*>(Other)) || Manager->CountHuntersOf(Other, this) >= MaxHuntersPerPlayer)
            {
                continue;
            }
            Best = Dist2D(LocationOf(Other), Location);
            Target = Other;
        }
        if (!Target)
        {
            return false;
        }
        HuntTarget = Target;
        ++Hunts;
        Log::Info(L"Battle Royale bots: {} goes after {} {:.0f} m away", BotName, NameOf(Target), Best / 100.0f);
    }
    MoveTowards(LocationOf(Target), Now, 300.0f);
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::ClearNextDestination()
{
    NextPOI = -1;
}

bool FCoreAthenaBotController::SelectNextPOI(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_SelectNextPOI: with nothing left to loot where it is, the bot sets off for the
    // best spot near it (FindNextBestPOI: a named location or a cluster of chests with loot left, inside the next safe
    // zone, not one it has been to), a teammate's choice first so the team stays together; it arrives within a third of
    // the spot's radius, and the loot there takes over.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn)
    {
        return false;
    }
    // A bot on a player's team goes where its players go (StayWithTeam): the next spot is theirs to choose.
    if (!Manager->PlayerTeammatesOf(this).empty())
    {
        ClearNextDestination();
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    const FCoreAthenaBotLandingSpot* Spot = Manager->GetPOI(NextPOI);
    if (Spot)
    {
        // A spot the storm is closing on is given up.
        FVector Center;
        float Radius = 0.0f;
        if (Manager->GetSafeZone(Center, Radius) && Dist2D(Spot->Location, Center) > Radius * 0.9f)
        {
            Log::Info(L"Battle Royale bots: {} gives up on {} (outside the safe zone)", BotName, Spot->Name);
            ClearNextDestination();
            Spot = nullptr;
        }
        // Not there in three minutes (the way is blocked): another one.
        else if (Now - POITravelStartTime > 180.0f)
        {
            Log::Info(L"Battle Royale bots: {} gives up on {} ({:.0f} m short of it)", BotName, Spot->Name, Dist2D(Location, POIDestination) / 100.0f);
            ClearNextDestination();
            Spot = nullptr;
        }
        else if (Dist2D(Location, POIDestination) < std::max(Spot->Radius * 0.33f, 1500.0f))
        {
            Log::Info(L"Battle Royale bots: {} arrives at {} after {:.0f} s", BotName, Spot->Name, Now - POITravelStartTime);
            ++POIsVisited;
            ClearNextDestination();
            return false;
        }
    }
    if (!Spot)
    {
        if (Now < NextPOISearch)
        {
            return false;
        }
        NextPOISearch = Now + 3.0f;
        // Where it is now counts as looted.
        if (const int32_t Here = Manager->FindPOIAt(Location); Here >= 0 && std::find(VisitedPOIs.begin(), VisitedPOIs.end(), Here) == VisitedPOIs.end())
        {
            VisitedPOIs.push_back(Here);
        }
        int32_t Next = -1;
        for (FCoreAthenaBotController* Teammate : Manager->TeammatesOf(this))
        {
            if (Teammate->GetState() == ECoreAthenaBotState::Landed && Teammate->GetNextPOI() >= 0
                && std::find(VisitedPOIs.begin(), VisitedPOIs.end(), Teammate->GetNextPOI()) == VisitedPOIs.end())
            {
                Next = Teammate->GetNextPOI();
                break;
            }
        }
        if (Next < 0)
        {
            Next = Manager->FindNextBestPOI(Location, VisitedPOIs);
        }
        Spot = Manager->GetPOI(Next);
        if (!Spot)
        {
            return false;
        }
        NextPOI = Next;
        VisitedPOIs.push_back(Next);
        POITravelStartTime = Now;
        // A spot on the ground near its middle (the volume's centre can be in the air or inside a building).
        if (!Manager->FindGroundNear(Spot->Location, Spot->Radius * 0.25f, POIDestination))
        {
            POIDestination = FVector(Spot->Location.X, Spot->Location.Y, Location.Z);
        }
        Log::Info(L"Battle Royale bots: {} has looted its spot and heads for {} {:.0f} m away", BotName, Spot->Name, Dist2D(Location, POIDestination) / 100.0f);
    }
    MoveTowards(POIDestination, Now, 300.0f);
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::MoveTowards(const FVector& Destination, float Now, float AcceptanceRadius)
{
#if WITH_SERVER_CODE
    // A path on the invoker navmesh (Core's UAthenaNavSystem) where there is one, else the pawn's own movement input,
    // every frame towards the goal, around what blocks it.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AAIController* AIController = Controller.Get();
    if (!Pawn || !AIController)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    if (Dist2D(Location, Destination) <= AcceptanceRadius)
    {
        StopMoving();
        return;
    }
    // Sprinting to get there, running while it fights (and for a moment after its target leaves its sight).
    SetMovementStyle(!bFiring && Now - LastTargetInSightTime > 1.5f, Now);
    FVector SteerGoal = Destination;
    if (FollowPath(Destination, Now, AcceptanceRadius, SteerGoal))
    {
        bHasMoveGoal = true;
        MoveGoal = Destination;
        // A path runs through doorways (the door's nav link): the door on the way is opened before the bot walks into it.
        const FVector Next = AIController->GetImmediateMoveDestination();
        const FVector Ahead = Dist2D(Location, Next) > 10.0f ? Direction2D(Location, Next) : Direction2D(Location, Destination);
        if (Now >= NextDoorProbe)
        {
            OpenDoorInTheWay(Ahead, Now);
        }
        // A wall the path doesn't know of (a piece built since its navmesh tile was made, a closed door that didn't
        // open): held up against it for most of a second, the bot steers around it for a moment instead of walking into
        // it until TryToUnstuck's checks (3 s) give up on the path (CoreV2 #252: "some bots walk into walls").
        UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
        const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
        const bool bHeldUp = Movement && Movement->MovementMode == MoveWalking && std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y) < 60.0f
            && IsBlocked(Pawn, FVector(Location.X, Location.Y, Location.Z - 30.0f), FVector(Location.X, Location.Y, Location.Z - 30.0f) + Ahead * 90.0f)
            && IsBlocked(Pawn, FVector(Location.X, Location.Y, Location.Z + 70.0f), FVector(Location.X, Location.Y, Location.Z + 70.0f) + Ahead * 90.0f);
        if (!bHeldUp)
        {
            HeldUpOnPathSince = -1.0f;
        }
        else if (HeldUpOnPathSince < 0.0f)
        {
            HeldUpOnPathSince = Now;
        }
        else if (Now - HeldUpOnPathSince > 0.75f)
        {
            HeldUpOnPathSince = -1.0f;
            StopMoving();
            SteerReason = L"wall on its path";
            PathFailedUntil = Now + 1.5f;
            bHasSteerTarget = false;
        }
        return;
    }
    if (!bHasMoveGoal || Dist2D(MoveGoal, Destination) > 150.0f)
    {
        MoveGoal = Destination;
        bHasMoveGoal = true;
    }
    // Facing the way it goes (UpdateFacing turns it along its velocity): Battle Royale's pawn moves slower sideways and
    // backwards (its directional speed curve). Before it gets going, it turns towards the goal.
    FVector Direction = Direction2D(Location, SteerGoal);
    OpenDoorInTheWay(Direction, Now);
    SteerAroundObstacles(Direction, Now);
    Pawn->AddMovementInput(Direction, 1.0f, false);
#endif
}

void FCoreAthenaBotController::StopMoving()
{
    bHasMoveGoal = false;
    SteerReason = L"standing";
    if (bPathFollowing)
    {
        bPathFollowing = false;
        if (AAIController* AIController = Controller.Get())
        {
            AIController->StopMovement();
        }
    }
}

bool FCoreAthenaBotController::FollowPath(const FVector& Destination, float Now, float AcceptanceRadius, FVector& SteerGoal)
{
#if WITH_SERVER_CODE
    // 21.40's _Movement with a navmesh: AAIController::MoveToLocation with pathfinding (a partial path where the tiles
    // around the bot end), followed by the engine's path following. A far goal is reached a leg at a time, each leg's end
    // a point on the navmesh, and the next leg is asked for while the bot is still on the last one. It steers only when
    // there is no navmesh to path on (off it, its tiles not built yet) or no path, for a moment (21.40's OnGoalOffNavMesh).
    SteerGoal = Destination;
    AAIController* AIController = Controller.Get();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Manager->HasNavigation() || State != ECoreAthenaBotState::Landed || !AIController || !Pawn)
    {
        if (bPathFollowing)
        {
            StopMoving();
        }
        SteerReason = L"no navigation";
        return false;
    }
    if (Now < PathFailedUntil)
    {
        if (bHasSteerTarget)
        {
            SteerGoal = SteerTarget;
        }
        if (bPathFollowing)
        {
            bPathFollowing = false;
            AIController->StopMovement();
        }
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    // A far goal that drifts (the storm's point on the bot's side of the circle) keeps its leg until it moves a tenth of
    // the way.
    const float GoalDrift = bOnLeg ? std::max(250.0f, Dist2D(Location, Destination) * 0.1f) : 250.0f;
    if (bPathFollowing && Dist2D(PathGoal, Destination) <= GoalDrift && Now < NextPathRequest)
    {
        if (AIController->GetMoveStatus() != PathIdle)
        {
            // Still on its way: a leg's last stretch asks for the next leg, so the bot never stops between legs.
            if (!bOnLeg || Dist2D(Location, PathTarget) > NextLegDistance || Now - LastPathRequest < 1.0f)
            {
                return true;
            }
        }
        else
        {
            bPathFollowing = false;
            if (!bOnLeg && Dist2D(Location, Destination) <= AcceptanceRadius + LastStepDistance)
            {
                // The path ends at the goal's point on the navmesh, a step from the goal itself (an item by a wall, a
                // chest's front): the last step is straight in.
                SteerReason = L"last step";
                PathFailedUntil = Now + 0.75f;
                bHasSteerTarget = false;
                return false;
            }
            if (Dist2D(Location, PathRequestLocation) < 150.0f)
            {
                // A path that took it nowhere: a partial one ending where it stood (the goal is past the navmesh built
                // so far, or cut off from it).
                SteerReason = L"path ends here";
                PathFailedUntil = Now + 1.0f;
                bHasSteerTarget = false;
                return false;
            }
        }
    }
    // At most a few requests a second (a path that ends at once is asked for again).
    if (Now - LastPathRequest < 0.25f)
    {
        return bPathFollowing;
    }
    return RequestPath(Destination, Now, AcceptanceRadius, SteerGoal);
#else
    return false;
#endif
}

bool FCoreAthenaBotController::RequestPath(const FVector& Destination, float Now, float AcceptanceRadius, FVector& SteerGoal)
{
#if WITH_SERVER_CODE
    AAIController* AIController = Controller.Get();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UWorld* World = Manager->GetGameMode()->GetWorld();
    const FVector Location = LocationOf(Pawn);
    LastPathRequest = Now;
    const auto Steer = [&](const wchar_t* Reason, float Seconds)
    {
        if (bPathFollowing)
        {
            bPathFollowing = false;
            AIController->StopMovement();
        }
        SteerReason = Reason;
        PathFailedUntil = Now + Seconds;
        bHasSteerTarget = false;
        return false;
    };

    // The bot on the navmesh. Just off it (pressed into the edge the navmesh keeps from walls, on a prop, by a hole),
    // it goes straight back onto its nearest point and asks again from there.
    FVector OnNavMesh;
    if (!UAthenaNavSystem::ProjectPointToNavigation(World, Location, OnNavMesh, FVector(100.0f, 100.0f, 250.0f)))
    {
        if (UAthenaNavSystem::ProjectPointToNavigation(World, Location, OnNavMesh, FVector(600.0f, 600.0f, 400.0f)))
        {
            Steer(L"back to the navmesh", 0.5f);
            SteerGoal = OnNavMesh;
            SteerTarget = OnNavMesh;
            bHasSteerTarget = true;
            return false;
        }
        return Steer(L"off the navmesh", 1.0f);
    }

    // The path's end on the navmesh. A far goal: a leg 30 m along the way (inside the 40 m of tiles built around the
    // bot), projected onto the navmesh with the height between, else a shorter or a turned one. A path search across
    // hundreds of metres held the game thread for seconds. A near goal: its own point on the navmesh, near it.
    const bool bFar = Dist2D(Location, Destination) > LegGoalDistance;
    FVector Target;
    bool bHasTarget = false;
    if (bFar)
    {
        const FVector Way = Direction2D(Location, Destination);
        for (const float Length : { 3000.0f, 2000.0f, 1200.0f })
        {
            for (const float Angle : { 0.0f, 20.0f, -20.0f, 45.0f, -45.0f })
            {
                const FVector Turned = RotateYaw(Way, Angle);
                const FVector Point(Location.X + Turned.X * Length, Location.Y + Turned.Y * Length, Location.Z);
                if (UAthenaNavSystem::ProjectPointToNavigation(World, Point, Target, FVector(200.0f, 200.0f, 2000.0f)))
                {
                    bHasTarget = true;
                    break;
                }
            }
            if (bHasTarget)
            {
                break;
            }
        }
    }
    else
    {
        bHasTarget = UAthenaNavSystem::ProjectPointToNavigation(World, Destination, Target, FVector(300.0f, 300.0f, 400.0f))
            || UAthenaNavSystem::ProjectPointToNavigation(World, Destination, Target, FVector(800.0f, 800.0f, 1500.0f));
    }
    if (!bHasTarget)
    {
        ++PathFailures;
        return Steer(bFar ? L"no leg on the navmesh" : L"goal off the navmesh", 1.0f);
    }

    const EPathFollowingRequestResult::Type Result = AIController->MoveToLocation(Target, bFar ? 150.0f : AcceptanceRadius, false, true, false, false, true);
    ++PathRequests;
    if (Result == EPathFollowingRequestResult::Failed)
    {
        ++PathFailures;
        return Steer(L"no path", 1.0f);
    }
    if (Result == EPathFollowingRequestResult::AlreadyAtGoal)
    {
        // At the goal's point on the navmesh already: the last step straight in.
        return Steer(L"last step", 0.75f);
    }
    if (PathRequests == 1)
    {
        Log::Info(L"Battle Royale bots: {} follows its first path, to ({:.0f}, {:.0f}, {:.0f}) {:.0f} m away", BotName, Destination.X, Destination.Y, Destination.Z, Dist2D(Location, Destination) / 100.0f);
    }
    bPathFollowing = true;
    bOnLeg = bFar;
    PathGoal = Destination;
    PathTarget = Target;
    PathRequestLocation = Location;
    NextPathRequest = Now + (bFar ? 6.0f : 4.0f);
    SteerReason = L"";
    // The bot faces along its path itself (UpdateFacing follows the velocity), turning smoothly at corners.
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::OpenDoorInTheWay(const FVector& Direction, float Now)
{
#if WITH_SERVER_CODE
    // A player walks up to a closed door and presses interact (AFortPlayerController::ServerAttemptInteract on the door
    // component). The bot does the same through the wall's IFortInteractInterface::ServerOnAttemptInteract: the game's own
    // door code swings it away from the bot, turns its collision off and its nav link open, as for a player, and closes
    // it again only as the game does for players. A door is left open behind the bot, as players leave them.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Now < NextDoorProbe || Direction.SizeSquared() < 0.01f || !ABuildingWall::HasDoor())
    {
        return;
    }
    NextDoorProbe = Now + 0.25f;
    const FVector Location = LocationOf(Pawn);
    // Waist height, a door's reach ahead (a player's interact reaches about 2.5 m).
    const FVector Start(Location.X, Location.Y, Location.Z);
    UObject* Hit = nullptr;
    if (!IsBlocked(Pawn, Start, Start + Direction * 220.0f, &Hit) || !Hit)
    {
        return;
    }
    ABuildingWall* Wall = Cast<ABuildingWall>(Hit);
    if (!Wall || !Wall->IsDoor() || Wall->bDoorOpen || (Wall == RefusedDoor && Now < RefusedDoorUntil))
    {
        return;
    }
    FaceTowards(LocationOf(reinterpret_cast<AActor*>(Wall)), Now, 0.3f);
    if (Wall->ServerOnAttemptInteract(reinterpret_cast<AFortPawn*>(Pawn)))
    {
        ++DoorsOpened;
        static int32_t LoggedDoors = 0;
        if (LoggedDoors++ < 12)
        {
            Log::Info(L"Battle Royale bots: {} opens {} ({})", BotName, NameOf(reinterpret_cast<UObject*>(Wall)), Wall->bDoorOpen ? L"open" : L"still closed");
        }
        return;
    }
    RefusedDoor = Wall;
    RefusedDoorUntil = Now + 5.0f;
    static int32_t LoggedRefusals = 0;
    if (LoggedRefusals++ < 12)
    {
        Log::Info(L"Battle Royale bots: {} can't open {} (the door's interaction refused it)", BotName, NameOf(reinterpret_cast<UObject*>(Wall)));
    }
#endif
}

void FCoreAthenaBotController::SteerAroundObstacles(FVector& Direction, float Now)
{
#if WITH_SERVER_CODE
    // Without a path: traces ahead at knee height from the capsule's middle and both its sides. Something low (the
    // head-height traces are clear) is jumped; a wall or a rock turns the bot, for a moment, to the side that opens up
    // first, with the most room that way, keeping to the side it turned to last (no back and forth along a wall).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Direction.SizeSquared() < 0.01f)
    {
        return;
    }
    if (Now < AvoidUntil)
    {
        Direction = AvoidDirection;
        return;
    }
    if (Now < NextObstacleProbe)
    {
        return;
    }
    NextObstacleProbe = Now + 0.2f;
    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    if (!Movement || Movement->MovementMode != MoveWalking)
    {
        return;
    }
    // Only when the bot is held up: a slope it walks up also blocks the knee trace.
    const FVector Velocity = Movement->Velocity;
    const float MaxSpeed = MaxSpeedOf(Movement);
    if (Velocity.Size() > std::max(MaxSpeed, 300.0f) * 0.6f)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    const FVector Knee(Location.X, Location.Y, Location.Z - 30.0f);
    const FVector Head(Location.X, Location.Y, Location.Z + 70.0f);
    // The room ahead along Way: the shortest of the traces at knee height from the capsule's middle and its two sides
    // (36 uu), and at head height from its middle, up to Length.
    const auto Room = [&](const FVector& Way, float Length)
    {
        const FVector Side = RotateYaw(Way, 90.0f) * 36.0f;
        float Shortest = Length;
        for (const FVector& Start : { Knee, Knee + Side, Knee - Side, Head })
        {
            FVector Hit;
            UObject* HitActor = nullptr;
            UObject* HitComponent = nullptr;
            if (UKismetSystemLibrary::LineTraceSingle(Pawn, Start, Start + Way * Length, 0, false, true, Hit, HitActor, HitComponent) && !(HitActor && Cast<APawn>(HitActor)))
            {
                Shortest = std::min(Shortest, Dist2D(Start, Hit));
            }
        }
        return Shortest;
    };
    UObject* Obstacle = nullptr;
    if (!IsBlocked(Pawn, Knee, Knee + Direction * 150.0f, &Obstacle))
    {
        return;
    }
    static int32_t LoggedObstacles = 0;
    if (Obstacle && LoggedObstacles++ < 12)
    {
        Log::Info(L"Battle Royale bots: {} is held up by {} ({})", BotName, NameOf(Obstacle), NameOf(Obstacle->GetClass()));
    }
    // A downed (DBNO) pawn crawls and can't jump, as a downed player can't (CoreV2 #233): it steers around instead.
    if (!Pawn->bIsDBNO && !IsBlocked(Pawn, Head, Head + Direction * 150.0f))
    {
        if (Now >= NextJumpTime)
        {
            NextJumpTime = Now + 1.2f;
            static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->Jump();
            ++Jumps;
        }
        return;
    }
    // Each side's first clear heading (3 m of room), the smaller turn winning; the same turn on both sides goes to the
    // one with more room, then the side it took last.
    constexpr float Clear = 300.0f;
    float BestAngle = 0.0f;
    float BestRoom = -1.0f;
    for (const float Turn : { 30.0f, 55.0f, 80.0f, 105.0f, 135.0f })
    {
        for (const float Side : { AvoidSide < 0.0f ? -1.0f : 1.0f, AvoidSide < 0.0f ? 1.0f : -1.0f })
        {
            const float Space = Room(RotateYaw(Direction, Turn * Side), 500.0f);
            if (Space >= Clear && Space > BestRoom + 50.0f)
            {
                BestAngle = Turn * Side;
                BestRoom = Space;
            }
        }
        if (BestRoom >= 0.0f)
        {
            break;
        }
    }
    if (BestRoom >= 0.0f)
    {
        AvoidSide = BestAngle > 0.0f ? 1.0f : -1.0f;
        AvoidDirection = RotateYaw(Direction, BestAngle);
        AvoidUntil = Now + 0.4f + 0.004f * std::fabs(BestAngle);
        Direction = AvoidDirection;
        return;
    }
    AvoidDirection = Direction * -1.0f;
    AvoidUntil = Now + 1.0f;
    Direction = AvoidDirection;
#endif
}

void FCoreAthenaBotController::SetMovementStyle(bool bSprint, float Now)
{
    // At most one change a second: a player doesn't flick sprint on and off.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const uint8_t Style = bSprint ? StyleSprinting : StyleRunning;
    if (!Pawn || MovementStyle == Style || (MovementStyle != 0xFF && Now < NextMovementStyleChange))
    {
        return;
    }
    NextMovementStyleChange = Now + 1.0f;
    MovementStyle = Style;
    Pawn->CurrentMovementStyle = Style;
}

void FCoreAthenaBotController::FaceTowards(const FVector& Point, float Now, float Hold)
{
    FacePoint = Point;
    bHasFacePoint = true;
    FacePointUntil = Now + Hold;
}

float FCoreAthenaBotController::FacingError() const
{
    if (!bHasFacePoint || !bHasFacingYaw)
    {
        return 180.0f;
    }
    const FVector Direction = Direction2D(LastKnownPosition, FacePoint);
    if (Direction.SizeSquared() < 0.01f)
    {
        return 0.0f;
    }
    float Delta = std::atan2(Direction.Y, Direction.X) * (180.0f / 3.1415926535897932f) - FacingYaw;
    Delta = std::fmod(Delta + 540.0f, 360.0f) - 180.0f;
    return std::fabs(Delta);
}

void FCoreAthenaBotController::UpdateFacing(float Now, float DeltaSeconds)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AAIController* AIController = Controller.Get();
    if (!Pawn || !AIController)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    if (!bHasFacingYaw)
    {
        FacingYaw = static_cast<FRotator>(AIController->ControlRotation).Yaw;
        bHasFacingYaw = true;
    }
    // What it wants to look at: the point it was given (a target, a chest), else the way it moves, else its goal.
    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
    const float Speed2D = std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y);
    // A pawn that starts or stops moving is sent to clients at once: some bots stood in their idle pose while moving
    // until a hit or a jump sent them again.
    if ((Speed2D > 50.0f) != bWasMoving)
    {
        bWasMoving = Speed2D > 50.0f;
        Pawn->ForceNetUpdate();
    }
    FVector Desired;
    if (bHasFacePoint && Now < FacePointUntil)
    {
        Desired = Direction2D(Location, FacePoint);
    }
    else if (Speed2D > 150.0f)
    {
        Desired = FVector(Velocity.X / Speed2D, Velocity.Y / Speed2D, 0.0f);
    }
    else if (bHasMoveGoal)
    {
        Desired = Direction2D(Location, MoveGoal);
    }
    else if (bHasFacePoint)
    {
        Desired = Direction2D(Location, FacePoint);
    }
    if (Desired.SizeSquared() > 0.01f)
    {
        // A player's turn: quick onto a target (540 degrees a second), easier while moving about (300).
        const float TurnRate = Now - LastTargetInSightTime < 1.0f ? 540.0f : 300.0f;
        float Delta = std::atan2(Desired.Y, Desired.X) * (180.0f / 3.1415926535897932f) - FacingYaw;
        Delta = std::fmod(Delta + 540.0f, 360.0f) - 180.0f;
        const float MaxStep = TurnRate * std::clamp(DeltaSeconds, 0.0f, 0.1f);
        FacingYaw += std::clamp(Delta, -MaxStep, MaxStep);
        FacingYaw = std::fmod(FacingYaw + 540.0f, 360.0f) - 180.0f;
    }
    const float Radians = FacingYaw * (3.1415926535897932f / 180.0f);
    const float Z = bHasFacePoint && Now < FacePointUntil ? FacePoint.Z : Location.Z + 60.0f;
    AIController->SetFocalPoint(FVector(Location.X + std::cos(Radians) * 1000.0f, Location.Y + std::sin(Radians) * 1000.0f, Z));
#endif
}

void FCoreAthenaBotController::LogMovementDiagnostics()
{
    // What the bot's movement reads its speed from (the speeds of its movement set for its style), once after landing.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UCharacterMovementComponent* Movement = Pawn ? static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement : nullptr;
    if (!Movement)
    {
        return;
    }
    UFortMovementSet* Set = Pawn->MovementSet;
    const FVector Velocity = Movement->Velocity;
    Log::Info(L"Battle Royale bots: {}'s movement: speed {:.0f}, max {:.0f}, mode {}, style {}, movement set {} (walk {:.0f}, run {:.0f}, sprint {:.0f}), max walk {:.0f}, {}",
        BotName, Velocity.Size(), MaxSpeedOf(Movement), static_cast<int32_t>(Movement->MovementMode), static_cast<int32_t>(Pawn->CurrentMovementStyle), NameOf(reinterpret_cast<UObject*>(Set)),
        Set ? static_cast<float>(Set->WalkSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->RunSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->SprintSpeed.CurrentValue) : -1.0f,
        static_cast<float>(Movement->MaxWalkSpeed), bPathFollowing ? L"on a path" : L"steering");
}

bool FCoreAthenaBotController::IsUnreachable(const void* Object) const
{
    return Object && std::find(Unreachable.begin(), Unreachable.end(), Object) != Unreachable.end();
}

void FCoreAthenaBotController::MarkUnreachable(const void* Object)
{
    if (Object && !IsUnreachable(Object))
    {
        Unreachable.push_back(Object);
    }
}

bool FCoreAthenaBotController::IsTakenByTeammate(const void* Object) const
{
    if (!Object)
    {
        return false;
    }
    for (const FCoreAthenaBotController* Teammate : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() == ECoreAthenaBotState::Landed && (Teammate->GetLootTarget() == Object || Teammate->GetContainerTarget() == Object))
        {
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotController::GetSquadLeash(FVector& OutAround, float& OutRadius) const
{
    // 🟡 40 m: a house or two from its player, so it still reaches them in a few seconds (it regroups past 25 m).
    const FVector Location = LastKnownPosition;
    float Nearest = 1e9f;
    for (AFortPlayerPawnAthena* Player : Manager->PlayerTeammatesOf(this))
    {
        if (!Player->bIsDBNO && !Player->bIsSkydiving && !Player->bIsParachuteOpen && Dist2D(LocationOf(Player), Location) < Nearest)
        {
            Nearest = Dist2D(LocationOf(Player), Location);
            OutAround = LocationOf(Player);
        }
    }
    OutRadius = 4000.0f;
    return Nearest < 1e9f;
}

bool FCoreAthenaBotController::SearchContainer(float Now, float MaxDistance)
{
#if WITH_SERVER_CODE
    // Ammo boxes are worth it while the bot wants ammo for its guns; chests while it wants anything, a better gun too
    // (a free slot, or one below epic: the community list, bots look for better loot).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    bool bWantsAmmo = false;
    bool bWantsBetter = WeaponList.size() < MaxWeapons;
    for (const FCoreAthenaBotWeapon& Weapon : WeaponList)
    {
        const auto Found = Ammo.find(Weapon.AmmoDefinition);
        bWantsAmmo = bWantsAmmo || Found == Ammo.end() || Found->second < 60;
        bWantsBetter = bWantsBetter || Weapon.Rarity < EpicRarity;
    }
    const bool bWantsChest = bWantsAmmo || bWantsBetter || Consumables.empty();
    ABuildingContainer* Target = ContainerTarget.Get();
    if (Target && (Target->IsPendingKill() || Target->bDestroyed || Target->IsAlreadySearched(reinterpret_cast<AFortPawn*>(Pawn))))
    {
        Manager->OnContainerSearched(Target);
        Target = nullptr;
        ContainerTarget = nullptr;
        SearchUntil = 0.0f;
        NextContainerSearch = 0.0f;
    }
    if (!Pawn || (!bWantsChest && !Target))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    // One the bot only noted (it isn't its goal) gives way to the nearest when it looks again.
    if (Target && Goal != ECoreAthenaBotGoal::Search && SearchUntil <= 0.0f && Now >= NextContainerSearch)
    {
        Target = nullptr;
    }
    if (!Target && Now >= NextContainerSearch)
    {
        NextContainerSearch = Now + 2.0f;
        float Best = 5000.0f;
        FVector LeashAround;
        float LeashRadius = 0.0f;
        const bool bLeashed = GetSquadLeash(LeashAround, LeashRadius);
        for (const FCoreAthenaBotContainer& Entry : Manager->GetContainers())
        {
            ABuildingContainer* Container = Entry.Container.Get();
            const float Distance = Dist2D(Entry.Location, Location) * (Entry.bChest ? 0.8f : 1.0f);
            if (Container && (Entry.bChest || bWantsAmmo) && !IsUnreachable(Container) && Distance < Best && std::fabs(Entry.Location.Z - Location.Z) < 800.0f
                && !IsTakenByTeammate(Container) && (!bLeashed || Dist2D(Entry.Location, LeashAround) <= LeashRadius))
            {
                Best = Distance;
                Target = Container;
            }
        }
        if (Target != ContainerTarget.Get())
        {
            ContainerTargetTime = Now;
            LastContainerPursuit = Now;
        }
        ContainerTarget = Target;
    }
    if (!Target)
    {
        return false;
    }
    // Farther than this call looks: kept for the goal's call.
    if (Dist2D(LocationOf(Target), Location) > MaxDistance && SearchUntil <= 0.0f)
    {
        return false;
    }
    // The time spent going for it counts (not the time the bot spent on something else meanwhile).
    ContainerTargetTime += Now - LastContainerPursuit > 0.5f ? Now - LastContainerPursuit : 0.0f;
    LastContainerPursuit = Now;
    if (Now - ContainerTargetTime > 25.0f && SearchUntil <= 0.0f)
    {
        MarkUnreachable(Target);
        ContainerTarget = nullptr;
        NextContainerSearch = 0.0f;
        return false;
    }
    const FVector ContainerLocation = LocationOf(Target);
    if (Dist2D(ContainerLocation, Location) < 190.0f && std::fabs(ContainerLocation.Z - Location.Z) < 250.0f)
    {
        // A player holds the interact for the search's time (its SearchSpeed curve, 🟡 1.5 s for a bot).
        StopMoving();
        if (SearchUntil <= 0.0f)
        {
            SearchUntil = Now + 1.5f;
            FaceTowards(ContainerLocation, Now, 1.5f);
            return true;
        }
        if (Now < SearchUntil)
        {
            return true;
        }
        SearchUntil = 0.0f;
        const bool bLoot = Target->ServerOnAttemptInteract(reinterpret_cast<AFortPawn*>(Pawn));
        ++ContainersSearched;
        Log::Info(L"Battle Royale bots: {} searches {}{}", BotName, NameOf(reinterpret_cast<UObject*>(Target)), bLoot ? L"" : L" (nothing came out)");
        Manager->OnContainerSearched(Target);
        ContainerTarget = nullptr;
        LootTarget = nullptr;
        NextLootSearch = 0.0f;
        return true;
    }
    SearchUntil = 0.0f;
    if (Dist2D(ContainerLocation, Location) < 120.0f)
    {
        // On the roof (or floor) over it: the bot breaks through to it (CoreV2 #275).
        if (BreakDownFromRoof(Now, ContainerLocation))
        {
            ContainerTargetTime = Now;
            return true;
        }
        MarkUnreachable(Target);
        ContainerTarget = nullptr;
        NextContainerSearch = 0.0f;
        return false;
    }
    MoveTowards(ContainerLocation, Now, 100.0f);
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::EquipPickaxe()
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAthenaPickaxeItemDefinition* Pickaxe = LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe();
    UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr;
    if (!Pawn || !PickaxeWeapon || (bPickaxeEquipped && EquippedWeapon.Get()))
    {
        return;
    }
    StopFiring();
    CancelReload();
    EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
    bPickaxeEquipped = true;
    EquippedIndex = -1;
#endif
}

bool FCoreAthenaBotController::SwingAtBuilding(ABuildingSMActor* Building, float& OutDamage)
{
    OutDamage = 0.0f;
    AFortWeapon* Pickaxe = bPickaxeEquipped ? EquippedWeapon.Get() : nullptr;
    if (Pickaxe)
    {
        PlayFireMontage();
    }
    return Pickaxe && DamageBuilding(Building, Pickaxe, OutDamage);
}

void FCoreAthenaBotController::PlayFireMontage()
{
#if WITH_SERVER_CODE
    AFortWeapon* Weapon = EquippedWeapon.Get();
    UFortWeaponItemDefinition* Definition = Weapon ? static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData) : nullptr;
    static std::map<UFortWeaponItemDefinition*, std::pair<UObject*, float>> Montages;
    if (!Definition)
    {
        return;
    }
    auto Found = Montages.find(Definition);
    if (Found == Montages.end())
    {
        UProperty* AbilityProperty = reinterpret_cast<UObject*>(Definition)->GetClass()->FindPropertyByName(FName(L"PrimaryFireAbility", FNAME_Find));
        UClass* AbilityClass = AbilityProperty ? reinterpret_cast<TAssetSubclassOf<UObject>*>(reinterpret_cast<uint8_t*>(Definition) + AbilityProperty->GetOffset_ForInternal())->LoadSynchronous() : nullptr;
        float PlayRate = 1.0f;
        // UFortGameplayAbility::MontageInfo (the melee combo plays it with PlayMontageInfoAndWaitForTrigger).
        UObject* Montage = AbilityClass ? ObjectOf(AbilityClass->GetDefaultObject(), L"MontageInfo", L"MontageToPlay", &PlayRate, L"AnimPlayRate") : nullptr;
        static int32_t Logged = 0;
        if (Logged++ < 8)
        {
            Log::Info(L"Battle Royale bots: {}'s fire montage: {} ({})", NameOf(Definition), NameOf(Montage), NameOf(AbilityClass));
        }
        KeepLoaded(Definition);
        Found = Montages.emplace(Definition, std::make_pair(KeepLoaded(Montage), PlayRate > 0.0f ? PlayRate : 1.0f)).first;
    }
    if (Found->second.first)
    {
        if (PlayReplicatedMontage(Found->second.first, Found->second.second))
        {
            EndMontageAfterSection(Found->second.first);
        }
        return;
    }
    // A gun's fire ability (GA_Ranged_GenericDamage) has no montage: a player's shot shows on the pawn through the
    // gun's own fire animation (AFortWeaponRanged::FireAnimation, FireDownsightsAnimation while aiming), the pawn's
    // montage, as its ReloadAnimation is for the reload.
    UObject* GunMontage = bTargeting ? ObjectOf(reinterpret_cast<UObject*>(Weapon), L"FireDownsightsAnimation") : nullptr;
    GunMontage = GunMontage ? GunMontage : ObjectOf(reinterpret_cast<UObject*>(Weapon), L"FireAnimation");
    static int32_t LoggedGun = 0;
    if (LoggedGun < 4 && GunMontage)
    {
        ++LoggedGun;
        Log::Info(L"Battle Royale bots: {}'s fire animation: {}{}", NameOf(Definition), NameOf(GunMontage), bTargeting ? L" (aiming)" : L"");
    }
    if (GunMontage)
    {
        PlayReplicatedMontage(GunMontage, 1.0f);
    }
#endif
}

void FCoreAthenaBotController::EndMontageAfterSection(UObject* Montage)
{
#if WITH_SERVER_CODE
    // The melee combo's sections link into each other (PlayerMeleeCombo: Swing1 -> Swing2 -> Swing1), so a player's
    // pickaxe keeps swinging while their fire input is held and their ability chains the swings. A bot's swing is one
    // section: UAnimInstance::Montage_SetNextSection(the playing section, None) ends the montage after it, and the
    // ability system's next replicated update (AnimMontage_UpdateReplicatedData's NextSectionID) ends it on the clients.
    static UFunction* GetAnimInstance = FindObject<UFunction>(nullptr, L"/Script/Engine.SkeletalMeshComponent:GetAnimInstance");
    static UFunction* GetCurrentSection = FindObject<UFunction>(nullptr, L"/Script/Engine.AnimInstance:Montage_GetCurrentSection");
    static UFunction* SetNextSection = FindObject<UFunction>(nullptr, L"/Script/Engine.AnimInstance:Montage_SetNextSection");
    static UProperty* MeshProperty = ACharacter::StaticClass()->FindPropertyByName(FName(L"Mesh", FNAME_Find));
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !Montage || !GetAnimInstance || !GetCurrentSection || !SetNextSection || !MeshProperty)
    {
        return;
    }
    UObject* Mesh = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Pawn) + MeshProperty->GetOffset_ForInternal());
    Reflection::FParams Instance(GetAnimInstance, true);
    UObject* AnimInstance = Mesh && Instance.Call(Mesh) ? Instance.Take<UObject*>(L"ReturnValue") : nullptr;
    Reflection::FParams Current(GetCurrentSection, true);
    Current.Set(L"Montage", Montage);
    const FName Section = AnimInstance && Current.Call(AnimInstance) ? Current.Take<FName>(L"ReturnValue") : FName();
    if (Section.IsNone())
    {
        return;
    }
    Reflection::FParams Next(SetNextSection, true);
    Next.Set(L"SectionNameToChange", Section);
    Next.Set(L"NextSection", FName());
    Next.Set(L"Montage", Montage);
    Next.Call(AnimInstance);
#endif
}

bool FCoreAthenaBotController::ApplyWeaponDamageEffect(UObject* TargetActor, AFortWeapon* Weapon, bool bMelee, const FVector* HitFrom)
{
#if WITH_SERVER_CODE
    // A weapon's hit as its ability applies it: the weapon's damage effect (GE_Ranged_GenericDamage for a gun, the
    // pickaxe's GE_Melee_GenericDamage), made on the bot's ability system and applied to the target's. Its execution,
    // UFortDamageFormulaExecutionCalculation (3.1 0xC49090), takes the base damage from the context's damage source:
    // FFortGameplayEffectContext::DamageSourceObject (an IFortDamageSourceInterface; AFortWeapon's reads its WeaponData's
    // stats row: a building takes its environment damage, a pawn its damage with the range's falloff). A context with no
    // damage source deals 0 (what phase 4 saw); the ability fills it with the weapon, and so does Core here. The damage
    // then goes the game's own way: the target's health, its damage response (the hit's numbers and direction, the
    // pawn's Damagers), and for a pawn its death or DBNO with the bot's controller as the instigator.
    static UClass* MeleeEffect = LoadObject<UClass>(nullptr, L"/Game/Abilities/Weapons/Melee/GE_Melee_GenericDamage.GE_Melee_GenericDamage_C");
    static UClass* RangedEffect = LoadObject<UClass>(nullptr, L"/Game/Abilities/Weapons/Ranged/GE_Ranged_GenericDamage.GE_Ranged_GenericDamage_C");
    static UFunction* MakeContext = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:MakeEffectContext");
    static UFunction* MakeSpec = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:MakeOutgoingSpec");
    static UFunction* ApplyToTarget = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:BP_ApplyGameplayEffectSpecToTarget");
    static UScriptStruct* FortContext = FindObject<UScriptStruct>(nullptr, L"/Script/FortniteGame.FortGameplayEffectContext");
    static UProperty* DamageSourceProperty = FortContext ? FortContext->FindPropertyByName(FName(L"DamageSourceObject", FNAME_Find)) : nullptr;
    UClass* DamageEffect = bMelee ? MeleeEffect : RangedEffect;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* Source = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UAbilitySystemComponent* Target = TargetActor ? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(reinterpret_cast<AActor*>(TargetActor)) : nullptr;
    static int32_t Logged = 0;
    if (!DamageEffect || !MakeContext || !MakeSpec || !ApplyToTarget || !DamageSourceProperty || !Weapon || !Source || !Target)
    {
        if (Logged++ < 3)
        {
            Log::Info(L"Battle Royale bots: {} can't hit {}: effect {}, functions {}, damage source {}, weapon {}, the bot's ability system {}, the target's {}", BotName, NameOf(TargetActor),
                NameOf(DamageEffect), MakeContext && MakeSpec && ApplyToTarget ? L"found" : L"missing", DamageSourceProperty ? L"found" : L"missing", NameOf(reinterpret_cast<UObject*>(Weapon)),
                NameOf(reinterpret_cast<UObject*>(Source)), NameOf(reinterpret_cast<UObject*>(Target)));
        }
        return false;
    }
    Reflection::FParams Context(MakeContext, true);
    if (!Context.Call(reinterpret_cast<UObject*>(Source)))
    {
        return false;
    }
    // FGameplayEffectContextHandle: its shared FGameplayEffectContext follows the handle's vtable. The ability system
    // globals make Fortnite's context (UFortAbilitySystemGlobals::AllocGameplayEffectContext, 0xC8 bytes on 3.1).
    void* ContextHandle = Context.GetAddress(L"ReturnValue");
    uint8_t* ContextData = ContextHandle ? static_cast<uint8_t**>(ContextHandle)[1] : nullptr;
    if (!ContextData)
    {
        return false;
    }
    *reinterpret_cast<FWeakObjectPtr*>(ContextData + DamageSourceProperty->GetOffset_ForInternal()) = reinterpret_cast<UObject*>(Weapon);
    // The instigator is the pawn and the causer the weapon, as for a player's shot: the bot's ability system is owned by
    // its player state (OnRep_PlayerState), so MakeEffectContext names the player state, and the victim's damage cue
    // (AFortPawn::GameplayCue_Damage, 3.1 0xDECD40) reads the instigator as a pawn: its controller and its location
    // for the hit's direction.
    static UProperty* InstigatorProperty = FortContext->FindPropertyByName(FName(L"Instigator", FNAME_Find));
    static UProperty* CauserProperty = FortContext->FindPropertyByName(FName(L"EffectCauser", FNAME_Find));
    if (InstigatorProperty && CauserProperty)
    {
        *reinterpret_cast<FWeakObjectPtr*>(ContextData + InstigatorProperty->GetOffset_ForInternal()) = reinterpret_cast<UObject*>(Pawn);
        *reinterpret_cast<FWeakObjectPtr*>(ContextData + CauserProperty->GetOffset_ForInternal()) = reinterpret_cast<UObject*>(Weapon);
    }
    if (HitFrom)
    {
        AddHitToContext(ContextHandle, TargetActor, *HitFrom);
    }
    Reflection::FParams Spec(MakeSpec, true);
    if (!Spec.Set(L"GameplayEffectClass", DamageEffect) || !Spec.Set(L"Level", 1.0f) || !Spec.CopyStruct(L"Context", ContextHandle) || !Spec.Call(reinterpret_cast<UObject*>(Source)))
    {
        return false;
    }
    const void* Handle = Spec.GetAddress(L"ReturnValue");
    if (!Handle || !*static_cast<void* const*>(Handle))
    {
        return false;
    }
    Reflection::FParams Apply(ApplyToTarget, true);
    return Apply.CopyStruct(L"SpecHandle", Handle) && Apply.Set(L"Target", Target) && Apply.Call(reinterpret_cast<UObject*>(Source));
#else
    return false;
#endif
}

bool FCoreAthenaBotController::DamageBuilding(ABuildingSMActor* Building, AFortWeapon* Weapon, float& OutDamage)
{
#if WITH_SERVER_CODE
    OutDamage = 0.0f;
    const float Before = Building->GetHealth();
    if (!ApplyWeaponDamageEffect(reinterpret_cast<UObject*>(Building), Weapon, Weapon == EquippedWeapon.Get() && bPickaxeEquipped))
    {
        return false;
    }
    OutDamage = Before - Building->GetHealth();
    static int32_t Logged = 0;
    if (Logged++ < 6)
    {
        Log::Info(L"Battle Royale bots: {} hits {} with {}: health {:.0f} -> {:.0f}", BotName, NameOf(reinterpret_cast<UObject*>(Building)), NameOf(static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData)), Before, Building->GetHealth());
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::DamagePawn(AFortPawn* Victim, AFortWeapon* Weapon, int32_t Hits, float& OutDamage)
{
#if WITH_SERVER_CODE
    // A shot that hit, the game's way (ApplyWeaponDamageEffect), once per pellet that hit: a player sees the hit as any
    // other (its damage, the hit direction), and goes down or out through the game's own death path, with this bot as
    // the instigator for the kill feed.
    OutDamage = 0.0f;
    if (!Victim || !Weapon || Victim->bIsDying)
    {
        return false;
    }
    const float Before = Victim->GetHealth() + Victim->GetShield();
    const float HealthBefore = Victim->GetHealth();
    const float ShieldBefore = Victim->GetShield();
    bool bApplied = false;
    for (int32_t Index = 0; Index < std::max(Hits, 1) && !Victim->bIsDying; ++Index)
    {
        const FVector From = LocationOf(GetPawn()) + FVector(0.0f, 0.0f, 60.0f);
        bApplied |= ApplyWeaponDamageEffect(reinterpret_cast<UObject*>(Victim), Weapon, bPickaxeEquipped && Weapon == EquippedWeapon.Get(), &From);
    }
    // A pawn that can't be damaged (the warmup island's, a player in god mode) shows the hit's numbers and keeps its
    // health and shield, as the game's own hits leave a player there.
    if (FCoreAthenaBotManager::IsInvulnerable(Victim) && !Victim->bIsDying && (Victim->GetHealth() != HealthBefore || Victim->GetShield() != ShieldBefore))
    {
        Victim->SetHealth(HealthBefore);
        Victim->SetShield(ShieldBefore);
    }
    OutDamage = Before - (Victim->bIsDying ? 0.0f : Victim->GetHealth() + Victim->GetShield());
    static int32_t Logged = 0;
    if (bApplied && Logged++ < 8)
    {
        Log::Info(L"Battle Royale bots: {} hits {} with {} the game's way ({} x, {:.0f} damage: {:.0f} -> {:.0f}{})", BotName, NameOf(reinterpret_cast<UObject*>(Victim)),
            NameOf(static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData)), std::max(Hits, 1), OutDamage, Before, Victim->bIsDying ? 0.0f : Victim->GetHealth() + Victim->GetShield(),
            Victim->bIsDying ? L", eliminated" : Victim->bIsDBNO ? L", down" : L"");
    }
    return bApplied && OutDamage > 0.0f;
#else
    return false;
#endif
}

void FCoreAthenaBotController::StopHarvesting()
{
    HarvestTarget = nullptr;
    bHarvestToUnstick = false;
    if (bPickaxeEquipped && HasUsableWeapon())
    {
        EquipWeapon(BestWeaponIndex());
    }
}

void FCoreAthenaBotController::OnResourcesHarvested(ABuildingSMActor* Building, UFortItemDefinition* ResourceItem, int32_t Count, bool bDestroyed)
{
#if WITH_SERVER_CODE
    bHarvestCredited = true;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!GameData || Count <= 0)
    {
        return;
    }
    const int32_t Kind = ResourceItem == GameData->WoodItemDefinition ? 0 : ResourceItem == GameData->StoneItemDefinition ? 1 : ResourceItem == GameData->MetalItemDefinition ? 2 : -1;
    if (Kind < 0)
    {
        return;
    }
    // A player's materials stop at the item's stack size (999 on 3.x).
    const int32_t MaxStack = ResourceItem->MaxStackSize > 0 ? static_cast<int32_t>(ResourceItem->MaxStackSize) : 999;
    Materials[Kind] = std::min(Materials[Kind] + Count, MaxStack);
    static int32_t Logged = 0;
    if (Logged++ < 8 || bDestroyed)
    {
        Log::Info(L"Battle Royale bots: {} harvests {} {} from {}{} ({} wood, {} stone, {} metal)", BotName, Count, Kind == 0 ? L"wood" : Kind == 1 ? L"stone" : L"metal",
            NameOf(reinterpret_cast<UObject*>(Building)), bDestroyed ? L", which breaks" : L"", Materials[0], Materials[1], Materials[2]);
    }
#endif
}

bool FCoreAthenaBotController::FindBuildingInTheWay(float Now)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !bHasMoveGoal || IsInCombat(Now))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    // The way it is trying to go first (inside a house its path turns through rooms, and the wall that holds it up is
    // seldom the one towards its goal: the goal's way alone found nothing to break), then the way to its goal.
    const FVector Ahead = WayAhead(Now, Location);
    const FVector ToGoal = Direction2D(Location, MoveGoal);
    std::vector<FVector> Ways;
    for (const FVector& Way : { Ahead, ToGoal })
    {
        if (Way.SizeSquared() > 0.01f && std::none_of(Ways.begin(), Ways.end(), [&Way](const FVector& Other) { return (Other - Way).SizeSquared() < 0.01f; }))
        {
            Ways.push_back(Way);
        }
    }
    // Ahead, and a little to each side, at knee, chest and head height.
    for (const FVector& Direction : Ways)
    {
        for (const float Yaw : { 0.0f, -25.0f, 25.0f })
        {
            for (const float Height : { -30.0f, 30.0f, 70.0f })
            {
                const FVector Start(Location.X, Location.Y, Location.Z + Height);
                FVector Hit;
                UObject* HitActor = nullptr;
                UObject* HitComponent = nullptr;
                if (!UKismetSystemLibrary::LineTraceSingle(Pawn, Start, Start + RotateYaw(Direction, Yaw) * 200.0f, 0, false, true, Hit, HitActor, HitComponent))
                {
                    continue;
                }
                ABuildingSMActor* Building = HitActor ? Cast<ABuildingSMActor>(HitActor) : nullptr;
                // Not a chest or an ammo box: the bot searches those.
                if (Building && !Cast<ABuildingContainer>(HitActor) && !Building->bDestroyed && Building->GetHealth() > 0.0f && !IsUnreachable(Building))
                {
                    HarvestTarget = Building;
                    HarvestTargetTime = Now;
                    HarvestPoint = Hit;
                    bHarvestToUnstick = true;
                    ++BreakThroughs;
                    Log::Info(L"Battle Royale bots: {} is stuck against {} ({:.0f} health) and breaks through it", BotName, NameOf(reinterpret_cast<UObject*>(Building)), Building->GetHealth());
                    return true;
                }
            }
        }
    }
    return false;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::BreakDownFromRoof(float Now, const FVector& Below)
{
#if WITH_SERVER_CODE
    // Where it is going is well below (a level is 384 uu), and what it stands on is a building's piece (a roof, a
    // floor): no path leads down from there (the roof's navmesh doesn't reach the ground, or the loot is in the house
    // under it), so it breaks the piece and drops through. On the ground or a rock there is nothing to break.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || HarvestTarget.Get() || IsInCombat(Now))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    if (Location.Z - Below.Z < 250.0f)
    {
        return false;
    }
    FVector Hit;
    UObject* HitActor = nullptr;
    UObject* HitComponent = nullptr;
    if (!UKismetSystemLibrary::LineTraceSingle(Pawn, Location, FVector(Location.X, Location.Y, Location.Z - 300.0f), 0, false, true, Hit, HitActor, HitComponent))
    {
        return false;
    }
    ABuildingSMActor* Building = HitActor ? Cast<ABuildingSMActor>(HitActor) : nullptr;
    if (!Building || Cast<ABuildingContainer>(HitActor) || Building->bDestroyed || Building->GetHealth() <= 0.0f || IsUnreachable(Building))
    {
        return false;
    }
    StopMoving();
    HarvestTarget = Building;
    HarvestTargetTime = Now;
    HarvestPoint = Hit;
    bHarvestToUnstick = true;
    ++BreakThroughs;
    Log::Info(L"Battle Royale bots: {} is up on {} ({:.0f} health), {:.0f} m above where it is going, and breaks through it", BotName, NameOf(reinterpret_cast<UObject*>(Building)),
        Building->GetHealth(), (Location.Z - Below.Z) / 100.0f);
    return true;
#else
    return false;
#endif
}

namespace
{
    // The player building grid: 512 uu cells, 384 uu a level.
    constexpr float BuildingCellSize = 512.0f;
    constexpr float BuildingLevelHeight = 384.0f;
    // What a Battle Royale piece costs a player, whichever the material.
    constexpr int32_t BuildingPieceCost = 10;
    // How far a bot keeps from the middle of a wall it is about to build (its capsule's radius and the wall's half
    // thickness, with room to spare), and the most a wall may stand above the ground at its middle: a player's wall
    // needs the world's support, and shots passed under a bot's wall hanging over a dip.
    constexpr float BotWallClearance = 60.0f;
    constexpr float MaxWallGroundGap = 50.0f;

    // A grid axis: 0 +X, 1 +Y, 2 -X, 3 -Y (a quarter turn to the right each, as yaw turns X towards Y).
    void GridAxis(int32_t Axis, int32_t& OutX, int32_t& OutY)
    {
        static constexpr int32_t Axes[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
        OutX = Axes[((Axis % 4) + 4) % 4][0];
        OutY = Axes[((Axis % 4) + 4) % 4][1];
    }

    bool IsWallPiece(ECoreOrientedConstructionBuildingType Type)
    {
        return Type == ECoreOrientedConstructionBuildingType::WallForward || Type == ECoreOrientedConstructionBuildingType::WallRight || Type == ECoreOrientedConstructionBuildingType::WallBack
            || Type == ECoreOrientedConstructionBuildingType::WallLeft;
    }

    // The axis a piece faces from its cell, relative to the build's forward.
    int32_t PieceAxisOffset(ECoreOrientedConstructionBuildingType Type)
    {
        switch (Type)
        {
        case ECoreOrientedConstructionBuildingType::WallRight:
            return 1;
        case ECoreOrientedConstructionBuildingType::WallBack:
            return 2;
        case ECoreOrientedConstructionBuildingType::WallLeft:
            return 3;
        default:
            return 0;
        }
    }

    const wchar_t* PieceName(ECoreOrientedConstructionBuildingType Type)
    {
        return IsWallPiece(Type) ? L"wall" : Type == ECoreOrientedConstructionBuildingType::RampForward ? L"ramp" : L"cone";
    }

    // The Battle Royale build tool's pieces (its BuildingItemData_Wall, _Stair_W and _RoofS place these): 0 wall, 1 ramp,
    // 2 cone; by material, 0 wood, 1 stone, 2 metal.
    UClass* PlayerBuildingClass(int32_t Piece, int32_t MaterialKind)
    {
        static const wchar_t* const Paths[3][3] = {
            {
                L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C",
                L"/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_Solid.PBWA_S1_Solid_C",
                L"/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_Solid.PBWA_M1_Solid_C",
            },
            {
                L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_StairW.PBWA_W1_StairW_C",
                L"/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_StairW.PBWA_S1_StairW_C",
                L"/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_StairW.PBWA_M1_StairW_C",
            },
            {
                L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_RoofC.PBWA_W1_RoofC_C",
                L"/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_RoofC.PBWA_S1_RoofC_C",
                L"/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_RoofC.PBWA_M1_RoofC_C",
            },
        };
        static UClass* Classes[3][3] = {};
        static bool bLoaded[3][3] = {};
        if (Piece < 0 || Piece > 2 || MaterialKind < 0 || MaterialKind > 2)
        {
            return nullptr;
        }
        if (!bLoaded[Piece][MaterialKind])
        {
            bLoaded[Piece][MaterialKind] = true;
            UClass* Class = LoadObject<UClass>(nullptr, Paths[Piece][MaterialKind]);
            Classes[Piece][MaterialKind] = Class && Class->IsChildOf(ABuildingSMActor::StaticClass()) ? KeepLoaded(Class) : nullptr;
            if (!Classes[Piece][MaterialKind])
            {
                Log::Warning(L"Battle Royale bots: no building class {}", Paths[Piece][MaterialKind]);
            }
        }
        return Classes[Piece][MaterialKind];
    }

    // The building grid's cell at Location, as the game finds a player's piece's: the structural support system's
    // K2_GetGridBoxFromWorldLoc (3.5.1 0xAF7590) rounds X and Y to the nearest cell centre (the grid origin plus whole
    // 512 uu cells, so a cell's edges are half a cell off the origin's) and floors Z to a 384 uu level above the origin.
    // Core's cells from the world origin and the ground under the bot put the bots' pieces off the grid (CoreV2 #264).
    bool GetBuildingGridCell(UWorld* World, const FVector& Location, FVector& OutCellMin)
    {
        static UFunction* GetGridBox = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.BuildingStructuralSupportSystem:K2_GetGridBoxFromWorldLoc");
        AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        UBuildingStructuralSupportSystem* SupportSystem = GameState ? static_cast<UBuildingStructuralSupportSystem*>(GameState->StructuralSupportSystem) : nullptr;
        static bool bLogged = false;
        if (!GetGridBox || !SupportSystem)
        {
            if (!bLogged)
            {
                bLogged = true;
                Log::Warning(L"Battle Royale bots: no building grid ({}), the bots build on cells from the world origin", GetGridBox ? L"no structural support system" : L"no K2_GetGridBoxFromWorldLoc");
            }
            return false;
        }
        Reflection::FParams Params(GetGridBox);
        FBox* Box = Params.Get<FBox>(L"ReturnValue");
        if (!Box || !Params.Set(L"WorldLocation", Location) || !Params.Call(reinterpret_cast<UObject*>(SupportSystem)) || !Box->IsValid)
        {
            return false;
        }
        OutCellMin = Box->Min;
        if (!bLogged)
        {
            bLogged = true;
            Log::Info(L"Battle Royale bots: the building grid's cell at ({:.0f}, {:.0f}, {:.0f}) is ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f})", Location.X, Location.Y, Location.Z,
                Box->Min.X, Box->Min.Y, Box->Min.Z, Box->Max.X, Box->Max.Y, Box->Max.Z);
        }
        return true;
    }
}

FVector FCoreAthenaBotController::WayAhead(float Now, const FVector& Location) const
{
    if (AAIController* AIController = Controller.Get(); AIController && bPathFollowing)
    {
        const FVector Next = AIController->GetImmediateMoveDestination();
        if (Dist2D(Location, Next) > 10.0f)
        {
            return Direction2D(Location, Next);
        }
    }
    if (bHasSteerTarget && Now < PathFailedUntil && Dist2D(Location, SteerTarget) > 10.0f)
    {
        return Direction2D(Location, SteerTarget);
    }
    return Direction2D(Location, MoveGoal);
}

bool FCoreAthenaBotController::FreeItself(float Now, const wchar_t* Why)
{
#if WITH_SERVER_CODE
    // A stuck player doesn't walk back and forth: they break the wall that holds them in, break the floor to get down,
    // or build up and over what they can't walk up (Chapter 2's bots ramp up hills and out of holes the same way).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !bHasMoveGoal || IsInCombat(Now) || HarvestTarget.Get() || !BuildQueue.Descriptors.empty() || Pawn->bIsDBNO)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    if (FindBuildingInTheWay(Now) || BreakDownFromRoof(Now, MoveGoal))
    {
        return true;
    }
    // Not where it's going down to: a hill, a cliff or a wall that isn't a building's in the way at head height (what
    // the bot can't step or jump onto), or where it's going a level and more above (out of a basement, up a slope).
    const float Rise = MoveGoal.Z - Location.Z;
    const FVector Ahead = WayAhead(Now, Location);
    const FVector Knee(Location.X, Location.Y, Location.Z - 30.0f);
    const FVector Head(Location.X, Location.Y, Location.Z + 70.0f);
    const bool bWallAhead = Ahead.SizeSquared() > 0.01f && IsBlocked(Pawn, Knee, Knee + Ahead * 120.0f) && IsBlocked(Pawn, Head, Head + Ahead * 120.0f);
    if (Location.Z - MoveGoal.Z < 250.0f && (bWallAhead || Rise > 250.0f) && BuildWayUp(Now, Location))
    {
        Log::Info(L"Battle Royale bots: {} is stuck ({}, {}) and builds {}", BotName, Why, bWallAhead ? L"a slope or a wall it can't get over" : L"where it is going is above it", BuildQueue.Reason);
        return true;
    }
    // Under a building's floor with where it's going above (a basement): the ceiling broken, to make room for the ramp.
    if (Rise > 250.0f && BreakUpThroughCeiling(Now, Location))
    {
        return true;
    }
    return false;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::BuildWayUp(float Now, const FVector& Location)
{
#if WITH_SERVER_CODE
    // A ramp in the bot's cell rising the way it is trying to go, and a second from its top where the hill goes on
    // (StartBuildingQueue leaves out a piece whose place isn't clear: no ground under it, a house's wall). Bounded: one
    // build every 6 s, four in a minute, two pieces each, and only what it can pay for.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UCharacterMovementComponent* Movement = Pawn ? static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement : nullptr;
    if (!Pawn || !Movement || Movement->MovementMode != MoveWalking || State != ECoreAthenaBotState::Landed || !Pawn->bCanBeDamaged || Now < NextUnstickBuildTime
        || Natives::ABuildingActor_InitializeBuildingActorIndex < 0 || Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex < 0)
    {
        return false;
    }
    if (Now - UnstickBuildWindowStart > 60.0f)
    {
        UnstickBuildWindowStart = Now;
        UnstickBuildsInWindow = 0;
    }
    if (UnstickBuildsInWindow >= 4)
    {
        return false;
    }
    NextUnstickBuildTime = Now + 2.0f;
    const FVector Ahead = WayAhead(Now, Location);
    if (Ahead.SizeSquared() < 0.01f)
    {
        return false;
    }
    const std::vector<FCoreAthenaBotBuildDescriptor> Descriptors = {
        { { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::RampForward },
        { { 1, 0, 1 }, ECoreOrientedConstructionBuildingType::RampForward },
    };
    const float Delay = AIBotSkills.Building.DelayBetweenBuildPieces;
    if (!StartBuildingQueue(Now, Location + Ahead * 500.0f, Descriptors, Delay, Delay, 0.0f, Delay, 0.0f, L"a ramp to climb out on", true))
    {
        return false;
    }
    // The upper ramp only on top of the first: alone it would hang in the air, out of reach.
    if (BuildQueue.Descriptors.front().Location[2] != 0)
    {
        ClearBuildingQueue();
        return false;
    }
    NextUnstickBuildTime = Now + 6.0f;
    ++UnstickBuildsInWindow;
    ++Climbs;
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::BreakUpThroughCeiling(float Now, const FVector& Location)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || HarvestTarget.Get() || IsInCombat(Now))
    {
        return false;
    }
    FVector Hit;
    UObject* HitActor = nullptr;
    UObject* HitComponent = nullptr;
    if (!UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Location.X, Location.Y, Location.Z + 60.0f), FVector(Location.X, Location.Y, Location.Z + 450.0f), 0, false, true, Hit, HitActor, HitComponent))
    {
        return false;
    }
    ABuildingSMActor* Building = HitActor ? Cast<ABuildingSMActor>(HitActor) : nullptr;
    if (!Building || Cast<ABuildingContainer>(HitActor) || Building->bDestroyed || Building->GetHealth() <= 0.0f || IsUnreachable(Building))
    {
        return false;
    }
    StopMoving();
    HarvestTarget = Building;
    HarvestTargetTime = Now;
    HarvestPoint = Hit;
    bHarvestToUnstick = true;
    ++BreakThroughs;
    Log::Info(L"Battle Royale bots: {} is under {} ({:.0f} health) with where it is going above, and breaks up through it", BotName, NameOf(reinterpret_cast<UObject*>(Building)), Building->GetHealth());
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::DefensiveBuilding(float Now, AFortPawn* Attacker)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_DefensiveBuilding (CanBuildNextTimeDelay, the building skill set's
    // DefensiveBuildingDelayBetweenBuilds and WeightedBuildingLists): shot out in the open by an enemy it sees, the bot
    // now and then builds towards them, on the edge of its own grid cell that faces them: a wall, or (a better bot more
    // often) the wall with a ramp up to it behind, as Chapter 2's bots do. Not on the warmup island (nobody can be hurt
    // there), nor where the place isn't clear (a house's wall, anyone's piece).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !Attacker || Now < NextDefensiveBuildTime || !BuildQueue.Descriptors.empty() || State != ECoreAthenaBotState::Landed || !Pawn->bCanBeDamaged || Pawn->bIsDBNO
        || Natives::ABuildingActor_InitializeBuildingActorIndex < 0 || Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex < 0)
    {
        return false;
    }
    const FCoreAthenaBotBuildingSkills& Skills = AIBotSkills.Building;
    // Occasionally: a share of the hits that find it in the open, then the skill set's delay before the next build.
    NextDefensiveBuildTime = Now + 1.0f;
    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    if (!Movement || Movement->MovementMode != MoveWalking || FMath::FRand() > Skills.DefensiveBuildingOdds)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    const FVector Shooter = LocationOf(Attacker);
    const float Distance = Dist2D(Location, Shooter);
    // Out in the open: nothing between the bot's eyes and the shooter's.
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    if (Distance < 700.0f || Distance > 8000.0f || IsBlocked(Pawn, Eye, FVector(Shooter.X, Shooter.Y, Shooter.Z + 60.0f)))
    {
        return false;
    }
    // One of the weighted lists.
    std::vector<FCoreAthenaBotBuildDescriptor> Descriptors;
    Descriptors.push_back({ { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::WallForward });
    const float TotalWeight = Skills.WallListWeight + Skills.WallAndRampListWeight;
    const bool bWallAndRamp = TotalWeight > 0.0f && FMath::FRand() * TotalWeight >= Skills.WallListWeight;
    if (bWallAndRamp)
    {
        Descriptors.push_back({ { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::RampForward });
    }
    if (!StartBuildingQueue(Now, Shooter, Descriptors, 0.0f, Skills.DelayBetweenBuildPieces, 0.0f, Skills.DelayBetweenBuildPieces, 0.0f, bWallAndRamp ? L"a wall and a ramp" : L"a wall"))
    {
        return false;
    }
    NextDefensiveBuildTime = Now + Skills.DefensiveBuildingDelayBetweenBuilds + RandomRange(-1.0f, 1.0f) * Skills.DefensiveBuildingDelayDeviationBetweenBuilds;
    static int32_t Logged = 0;
    if (Logged++ < 12)
    {
        Log::Info(L"Battle Royale bots: {} is shot by {} {:.0f} m away in the open and builds {}", BotName, NameOf(reinterpret_cast<UObject*>(Attacker)), Distance / 100.0f, BuildQueue.Reason);
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::Bunker(float Now, AFortPawn* Threat, bool bUnknownThreat)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Bunker with UFortAthenaAIPerk_Bunker: the bot boxes itself in, four walls on its
    // cell's edges (the one towards the threat first), a cone on top (OddsToUseRoofBuilding) and a ramp inside up to
    // the front wall (OddsToBuildForwardRamp), each piece after the perk's delays (InitialBuildDelay, then
    // SamePieceBuildDelay between walls and DifferentPieceBuildDelay between kinds). It does so as it heals (the
    // evaluator's HealingStatusKey) and, at OddsToBeUsedAgainstUnknownThreat, when an enemy it hadn't seen shoots it;
    // the perk's cooldown keeps it to now and then.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Now < NextBunkerTime || !BuildQueue.Descriptors.empty() || State != ECoreAthenaBotState::Landed || !Pawn->bCanBeDamaged || Pawn->bIsDBNO
        || Natives::ABuildingActor_InitializeBuildingActorIndex < 0 || Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex < 0)
    {
        return false;
    }
    const FCoreAthenaBotBuildingSkills& Skills = AIBotSkills.Building;
    // A failed roll isn't rolled again for a moment (a burst of hits is one chance, not ten).
    NextBunkerTime = Now + (bUnknownThreat ? 3.0f : 1.0f);
    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    if (!Movement || Movement->MovementMode != MoveWalking || FMath::FRand() > (bUnknownThreat ? Skills.OddsToBeUsedAgainstUnknownThreat : Skills.BunkerOddsToActivate))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    // Towards the threat, the enemy it last saw, or the way it faces.
    FVector Towards = Location + RotateYaw(FVector(1.0f, 0.0f, 0.0f), FacingYaw) * 500.0f;
    if (Threat)
    {
        Towards = LocationOf(Threat);
    }
    else if (LastSeenTarget.Get() && Now - LastTargetInSightTime < 30.0f)
    {
        Towards = LastSeenTargetLocation;
    }
    std::vector<FCoreAthenaBotBuildDescriptor> Descriptors = {
        { { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::WallForward },
        { { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::WallLeft },
        { { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::WallRight },
        { { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::WallBack },
    };
    const bool bRamp = FMath::FRand() < Skills.OddsToBuildForwardRamp;
    const bool bRoof = FMath::FRand() < Skills.OddsToUseRoofBuilding;
    if (bRamp)
    {
        Descriptors.push_back({ { 0, 0, 0 }, ECoreOrientedConstructionBuildingType::RampForward });
    }
    if (bRoof)
    {
        Descriptors.push_back({ { 0, 0, 1 }, ECoreOrientedConstructionBuildingType::Roof });
    }
    if (!StartBuildingQueue(Now, Towards, Descriptors, Skills.InitialBuildDelay, Skills.SamePieceBuildDelay, Skills.SamePieceBuildDelayRandomDeviation, Skills.DifferentPieceBuildDelay,
            Skills.DifferentPieceBuildDelayRandomDeviation, bUnknownThreat ? L"a box (shot by an enemy it hadn't seen)" : L"a box to heal in"))
    {
        return false;
    }
    // A box is three walls at least (a house's wall may be the fourth).
    const size_t Walls = static_cast<size_t>(std::count_if(BuildQueue.Descriptors.begin(), BuildQueue.Descriptors.end(), [](const FCoreAthenaBotBuildDescriptor& Descriptor) { return IsWallPiece(Descriptor.BuildingType); }));
    if (Walls < 3)
    {
        ClearBuildingQueue();
        return false;
    }
    NextBunkerTime = Now + Skills.BunkerCooldownDuration;
    // Under fire it stays in the box a little after the last piece; healing, until the item is used (Heal holds it).
    if (bUnknownThreat)
    {
        HideEndTime = Now + Skills.InitialBuildDelay + static_cast<float>(BuildQueue.Descriptors.size()) * Skills.DifferentPieceBuildDelay + RandomRange(2.0f, 4.0f);
    }
    static int32_t Logged = 0;
    if (Logged++ < 12)
    {
        Log::Info(L"Battle Royale bots: {} builds {}: {} walls{}{}", BotName, BuildQueue.Reason, Walls, bRamp ? L", a ramp" : L"", bRoof ? L", a cone" : L"");
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::StartBuildingQueue(float Now, const FVector& Towards, const std::vector<FCoreAthenaBotBuildDescriptor>& Descriptors, float InitialDelay, float SameDelay,
    float SameDeviation, float DifferentDelay, float DifferentDeviation, const wchar_t* Reason, bool bToUnstick)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Descriptors.empty())
    {
        return false;
    }
    // The material it has the most of, as a player picks one for a build.
    const int32_t Kind = static_cast<int32_t>(std::max_element(std::begin(Materials), std::end(Materials)) - std::begin(Materials));
    if (Materials[Kind] < BuildingPieceCost)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    // The ground under the bot's feet, and the building grid's cell there: every piece of the build is on the grid, as
    // a player's is (a box's walls and roof line up, and with everyone's pieces).
    FVector Ground;
    UObject* GroundActor = nullptr;
    UObject* GroundComponent = nullptr;
    if (!UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Location.X, Location.Y, Location.Z), FVector(Location.X, Location.Y, Location.Z - 300.0f), 0, false, true, Ground, GroundActor, GroundComponent)
        || (GroundActor && Cast<APawn>(GroundActor)))
    {
        return false;
    }
    FCoreAthenaBotBuildingQueue Queue;
    FVector CellMin;
    if (!GetBuildingGridCell(Manager->GetGameMode() ? Manager->GetGameMode()->GetWorld() : nullptr, FVector(Location.X, Location.Y, Ground.Z + 10.0f), CellMin))
    {
        CellMin = FVector(std::floor(Location.X / BuildingCellSize) * BuildingCellSize, std::floor(Location.Y / BuildingCellSize) * BuildingCellSize, Ground.Z);
    }
    Queue.CellMinX = CellMin.X;
    Queue.CellMinY = CellMin.Y;
    Queue.BaseZ = CellMin.Z;
    const FVector Way = Direction2D(Location, Towards);
    Queue.Forward = std::fabs(Way.X) >= std::fabs(Way.Y) ? (Way.X >= 0.0f ? 0 : 2) : (Way.Y >= 0.0f ? 1 : 3);
    Queue.MaterialKind = Kind;
    Queue.InitialBuildDelay = InitialDelay;
    Queue.SamePieceBuildDelay = SameDelay;
    Queue.SamePieceBuildDelayRandomDeviation = SameDeviation;
    Queue.DifferentPieceBuildDelay = DifferentDelay;
    Queue.DifferentPieceBuildDelayRandomDeviation = DifferentDeviation;
    Queue.Reason = Reason;
    Queue.bToUnstick = bToUnstick;
    // The pieces whose place is clear now, as many as it can pay for (the placement reads the queue's frame).
    BuildQueue = std::move(Queue);
    std::vector<FCoreAthenaBotBuildDescriptor> Clear;
    for (const FCoreAthenaBotBuildDescriptor& Descriptor : Descriptors)
    {
        if (static_cast<int32_t>(Clear.size() + 1) * BuildingPieceCost > Materials[Kind])
        {
            break;
        }
        if (IsPiecePlaceClear(Descriptor))
        {
            Clear.push_back(Descriptor);
        }
    }
    if (Clear.empty())
    {
        BuildQueue = FCoreAthenaBotBuildingQueue();
        return false;
    }
    BuildQueue.Descriptors = std::move(Clear);
    NextBuildPiece = 0;
    LastBuildTime = Now;
    DelayBeforeNextBuildPiece = InitialDelay;
    ++BuildsStarted;
    StopMoving();
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::ClearBuildingQueue()
{
    BuildQueue = FCoreAthenaBotBuildingQueue();
    NextBuildPiece = 0;
    DelayBeforeNextBuildPiece = 0.0f;
}

bool FCoreAthenaBotController::TickBuildingQueue(float Now)
{
#if WITH_SERVER_CODE
    if (BuildQueue.Descriptors.empty())
    {
        return false;
    }
    // A build ends with the bot down, off its feet, or out of its cell (pushed, or it had to move).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UCharacterMovementComponent* Movement = Pawn ? static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement : nullptr;
    const FVector Location = LocationOf(Pawn);
    if (!Pawn || !Movement || State != ECoreAthenaBotState::Landed || Pawn->bIsDBNO || Movement->MovementMode != MoveWalking || NextBuildPiece >= BuildQueue.Descriptors.size()
        || Location.X < BuildQueue.CellMinX || Location.X >= BuildQueue.CellMinX + BuildingCellSize || Location.Y < BuildQueue.CellMinY || Location.Y >= BuildQueue.CellMinY + BuildingCellSize)
    {
        ClearBuildingQueue();
        return false;
    }
    // The bot looks the way it builds, as a player aims a piece, also while it steps into place: it backs up to a ramp's
    // low edge rather than turning round to walk there (its facing follows its velocity otherwise), so the ramp goes up
    // where it looks, not behind it (CoreV2 #324: bots stuck in a 1x1 built their stairs behind themselves).
    int32_t ForwardX = 0;
    int32_t ForwardY = 0;
    GridAxis(BuildQueue.Forward, ForwardX, ForwardY);
    FaceTowards(Location + FVector(static_cast<float>(ForwardX), static_cast<float>(ForwardY), 0.0f) * 500.0f, Now, 0.5f);
    // A wall goes up beside the bot, not through it: by its cell's edge, the bot's capsule stuck out of a wall placed
    // there (the piece spawns whatever overlaps it), and players shot it through its own wall (CoreV2 #264). It steps
    // towards the cell's middle first, for a second and a half at most.
    const FCoreAthenaBotBuildDescriptor& Upcoming = BuildQueue.Descriptors[NextBuildPiece];
    FVector WallOrigin;
    float WallYaw = 0.0f;
    if (IsWallPiece(Upcoming.BuildingType) && Now < LastBuildTime + DelayBeforeNextBuildPiece + 1.5f && GetPiecePlacement(Upcoming, WallOrigin, WallYaw))
    {
        const FVector Across = RotateYaw(FVector(0.0f, 1.0f, 0.0f), WallYaw);
        const FVector Along = RotateYaw(FVector(1.0f, 0.0f, 0.0f), WallYaw);
        const float FromWall = std::fabs((Location.X - WallOrigin.X) * Across.X + (Location.Y - WallOrigin.Y) * Across.Y);
        const float AlongWall = std::fabs((Location.X - WallOrigin.X) * Along.X + (Location.Y - WallOrigin.Y) * Along.Y);
        if (FromWall < BotWallClearance && AlongWall < BuildingCellSize * 0.5f + BotWallClearance)
        {
            MoveTowards(FVector(BuildQueue.CellMinX + BuildingCellSize * 0.5f, BuildQueue.CellMinY + BuildingCellSize * 0.5f, Location.Z), Now, 40.0f);
            return true;
        }
    }
    // A ramp to climb out on goes up under the bot from where it stands: it steps back to the ramp's low edge first (it
    // stood against the hill, under the ramp's high end), for a second and a half at most.
    if (BuildQueue.bToUnstick && Upcoming.BuildingType == ECoreOrientedConstructionBuildingType::RampForward && Upcoming.Location[0] == 0 && Upcoming.Location[2] == 0
        && Now < LastBuildTime + DelayBeforeNextBuildPiece + 1.5f)
    {
        int32_t BackX = 0;
        int32_t BackY = 0;
        GridAxis(BuildQueue.Forward, BackX, BackY);
        const FVector LowEnd(BuildQueue.CellMinX + BuildingCellSize * 0.5f - static_cast<float>(BackX) * 160.0f, BuildQueue.CellMinY + BuildingCellSize * 0.5f - static_cast<float>(BackY) * 160.0f, Location.Z);
        if (Dist2D(Location, LowEnd) > 60.0f)
        {
            MoveTowards(LowEnd, Now, 40.0f);
            return true;
        }
    }
    StopMoving();
    if (Now < LastBuildTime + DelayBeforeNextBuildPiece)
    {
        return true;
    }
    const FCoreAthenaBotBuildDescriptor Descriptor = BuildQueue.Descriptors[NextBuildPiece++];
    // Placed only if its place is still clear (someone may have built there meanwhile).
    if (!IsPiecePlaceClear(Descriptor) || !BuildPiece(Descriptor))
    {
        static int32_t LoggedSkipped = 0;
        if (LoggedSkipped++ < 12)
        {
            Log::Info(L"Battle Royale bots: {} leaves out a {} of {} (its place isn't clear or it can't pay)", BotName, PieceName(Descriptor.BuildingType), BuildQueue.Reason);
        }
    }
    if (NextBuildPiece >= BuildQueue.Descriptors.size() || Materials[BuildQueue.MaterialKind] < BuildingPieceCost)
    {
        ClearBuildingQueue();
        return true;
    }
    const bool bSamePiece = IsWallPiece(Descriptor.BuildingType) == IsWallPiece(BuildQueue.Descriptors[NextBuildPiece].BuildingType)
        && (IsWallPiece(Descriptor.BuildingType) || Descriptor.BuildingType == BuildQueue.Descriptors[NextBuildPiece].BuildingType);
    LastBuildTime = Now;
    DelayBeforeNextBuildPiece = bSamePiece ? BuildQueue.SamePieceBuildDelay + RandomRange(-1.0f, 1.0f) * BuildQueue.SamePieceBuildDelayRandomDeviation
                                           : BuildQueue.DifferentPieceBuildDelay + RandomRange(-1.0f, 1.0f) * BuildQueue.DifferentPieceBuildDelayRandomDeviation;
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::GetPiecePlacement(const FCoreAthenaBotBuildDescriptor& Descriptor, FVector& OutLocation, float& OutYaw) const
{
    // The piece's cell from the origin cell (forward, right, up).
    int32_t ForwardX = 0;
    int32_t ForwardY = 0;
    int32_t RightX = 0;
    int32_t RightY = 0;
    GridAxis(BuildQueue.Forward, ForwardX, ForwardY);
    GridAxis(BuildQueue.Forward + 1, RightX, RightY);
    // Cells from the origin cell's lower corner.
    const float CellX = static_cast<float>(ForwardX * Descriptor.Location[0] + RightX * Descriptor.Location[1]) + BuildQueue.CellMinX / BuildingCellSize;
    const float CellY = static_cast<float>(ForwardY * Descriptor.Location[0] + RightY * Descriptor.Location[1]) + BuildQueue.CellMinY / BuildingCellSize;
    const float Z = BuildQueue.BaseZ + static_cast<float>(Descriptor.Location[2]) * BuildingLevelHeight;
    // The pieces' meshes (3.x's PBW_W1_*): a wall spans 512 along its X, its origin the bottom middle; a ramp (StairW) and
    // a cone (RoofC) span 512 along X and along +Y from their origin, the middle of one edge. The ramp is high at its
    // origin's edge (392) and meets the ground at the far one.
    if (Descriptor.BuildingType == ECoreOrientedConstructionBuildingType::Roof)
    {
        OutLocation = FVector((static_cast<float>(CellX) + 0.5f) * BuildingCellSize, static_cast<float>(CellY) * BuildingCellSize, Z);
        OutYaw = 0.0f;
        return true;
    }
    // A wall on the cell's edge on that side; a ramp rising to that edge, its origin there and its +Y back into the cell.
    int32_t SideX = 0;
    int32_t SideY = 0;
    GridAxis(BuildQueue.Forward + PieceAxisOffset(Descriptor.BuildingType), SideX, SideY);
    OutLocation = FVector((static_cast<float>(CellX) + 0.5f + 0.5f * static_cast<float>(SideX)) * BuildingCellSize, (static_cast<float>(CellY) + 0.5f + 0.5f * static_cast<float>(SideY)) * BuildingCellSize, Z);
    if (IsWallPiece(Descriptor.BuildingType))
    {
        OutYaw = SideX != 0 ? 90.0f : 0.0f;
    }
    else
    {
        // Yaw turns +Y to (-sin, cos): pointing it back into the cell, against the side.
        OutYaw = SideX > 0 ? 90.0f : SideX < 0 ? -90.0f : SideY > 0 ? 180.0f : 0.0f;
    }
    return true;
}

bool FCoreAthenaBotController::IsPiecePlaceClear(const FCoreAthenaBotBuildDescriptor& Descriptor) const
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    FVector Origin;
    float Yaw = 0.0f;
    if (!Pawn || !GetPiecePlacement(Descriptor, Origin, Yaw))
    {
        return false;
    }
    // Ground under it, a step below the build's at most (not hanging over a drop): under a wall's middle, under a ramp's
    // or a cone's cell centre (half a cell along their +Y from the origin's edge). The grid's level can be partly in
    // the ground (as a player's piece is on a slope), so it is looked for from the level's top down.
    const FVector Probe = IsWallPiece(Descriptor.BuildingType) ? Origin : Origin + RotateYaw(FVector(0.0f, 1.0f, 0.0f), Yaw) * (BuildingCellSize * 0.5f);
    if (Descriptor.BuildingType != ECoreOrientedConstructionBuildingType::Roof)
    {
        FVector Ground;
        UObject* GroundActor = nullptr;
        UObject* GroundComponent = nullptr;
        if (!UKismetSystemLibrary::LineTraceSingle(Pawn, FVector(Probe.X, Probe.Y, Origin.Z + BuildingLevelHeight), FVector(Probe.X, Probe.Y, Origin.Z - 250.0f), 0, false, true, Ground, GroundActor, GroundComponent)
            || (GroundActor && Cast<APawn>(GroundActor)) || (IsWallPiece(Descriptor.BuildingType) && Ground.Z < Origin.Z - MaxWallGroundGap))
        {
            return false;
        }
    }
    // Nothing where it goes (a house's wall, a tree, anyone's piece): traces through its body.
    if (IsWallPiece(Descriptor.BuildingType))
    {
        // Along the edge, short of the corners (a neighbouring wall's thickness), and across it, at knee and head height.
        const FVector Along = RotateYaw(FVector(1.0f, 0.0f, 0.0f), Yaw) * (BuildingCellSize * 0.5f - 40.0f);
        const FVector Across = RotateYaw(FVector(0.0f, 1.0f, 0.0f), Yaw) * 40.0f;
        for (const float Height : { 50.0f, 300.0f })
        {
            const FVector Middle(Origin.X, Origin.Y, Origin.Z + Height);
            if (IsBlocked(Pawn, Middle - Along, Middle + Along) || IsBlocked(Pawn, Middle - Across, Middle + Across))
            {
                return false;
            }
        }
        return true;
    }
    if (Descriptor.BuildingType == ECoreOrientedConstructionBuildingType::RampForward)
    {
        // Above the slope, from the low edge to short of the high one, in the middle and towards both sides. A ramp to
        // climb out on (bToUnstick) may run into the hill it climbs, as a player's ramp up a slope does; a building's piece
        // (a house's, anyone's, a rock) in its way still keeps it out.
        const FVector Back = RotateYaw(FVector(0.0f, 1.0f, 0.0f), Yaw);
        const FVector Side = RotateYaw(FVector(1.0f, 0.0f, 0.0f), Yaw);
        for (const float Offset : { -200.0f, 0.0f, 200.0f })
        {
            const FVector Low = Origin + Back * (BuildingCellSize - 40.0f) + Side * Offset;
            const FVector High = Origin + Back * 60.0f + Side * Offset;
            UObject* Hit = nullptr;
            if (IsBlocked(Pawn, FVector(Low.X, Low.Y, Origin.Z + 60.0f + 376.0f * (40.0f / BuildingCellSize)), FVector(High.X, High.Y, Origin.Z + 60.0f + 376.0f * ((BuildingCellSize - 60.0f) / BuildingCellSize)), &Hit)
                && (!BuildQueue.bToUnstick || !Hit || Cast<ABuildingSMActor>(Hit)))
            {
                return false;
            }
        }
        return true;
    }
    // A cone: across its cell's diagonals, above the walls' tops.
    const float MinX = Probe.X - BuildingCellSize * 0.5f + 40.0f;
    const float MaxX = Probe.X + BuildingCellSize * 0.5f - 40.0f;
    const float MinY = Probe.Y - BuildingCellSize * 0.5f + 40.0f;
    const float MaxY = Probe.Y + BuildingCellSize * 0.5f - 40.0f;
    const float Height = Origin.Z + 60.0f;
    return !IsBlocked(Pawn, FVector(MinX, MinY, Height), FVector(MaxX, MaxY, Height)) && !IsBlocked(Pawn, FVector(MinX, MaxY, Height), FVector(MaxX, MinY, Height));
#else
    return false;
#endif
}

bool FCoreAthenaBotController::BuildPiece(const FCoreAthenaBotBuildDescriptor& Descriptor)
{
#if WITH_SERVER_CODE
    // A player's piece goes through AFortPlayerController::ServerCreateBuildingActor, whose checks (CanAffordTo...,
    // CanPlaceBuildableClassInStructuralGrid) are the player controller's own virtuals; a bot's controller is an
    // AAIController. So the bot's piece is spawned and initialized the way that body does it after its checks
    // (SetMirrored, InitializeBuildingActor, PostInitializeSpawnedBuildingActor), with the bot's team, and the navmesh
    // around it is rebuilt.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UWorld* World = Manager->GetGameMode() ? Manager->GetGameMode()->GetWorld() : nullptr;
    const int32_t Kind = BuildQueue.MaterialKind;
    if (!Pawn || !World || Kind < 0 || Kind > 2 || Materials[Kind] < BuildingPieceCost || Natives::ABuildingActor_InitializeBuildingActorIndex < 0
        || Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex < 0)
    {
        return false;
    }
    const int32_t Piece = IsWallPiece(Descriptor.BuildingType) ? 0 : Descriptor.BuildingType == ECoreOrientedConstructionBuildingType::RampForward ? 1 : 2;
    UClass* PieceClass = PlayerBuildingClass(Piece, Kind);
    FVector Origin;
    float Yaw = 0.0f;
    if (!PieceClass || !GetPiecePlacement(Descriptor, Origin, Yaw))
    {
        return false;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FTransform SpawnTransform;
    SpawnTransform.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion();
    SpawnTransform.SetLocation(Origin);
    ABuildingSMActor* Building = Cast<ABuildingSMActor>(World->SpawnActor(PieceClass, &SpawnTransform, SpawnParameters));
    if (!Building)
    {
        return false;
    }
    if (Natives::ABuildingSMActor_SetMirroredIndex >= 0)
    {
        CallVirtual<void>(Building, Natives::ABuildingSMActor_SetMirroredIndex, false);
    }
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    const int32_t WorldPlayerId = PlayerState ? static_cast<int32_t>(PlayerState->WorldPlayerId) : -1;
    CallVirtual<void>(Building, Natives::ABuildingActor_InitializeBuildingActorIndex, EFortBuildingInitializationReason::Spawned, WorldPlayerId, static_cast<void*>(nullptr), static_cast<void*>(nullptr), false);
    CallVirtual<void>(Building, Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex, EFortBuildingInitializationReason::Spawned);
    // The bot's team, as Battle Royale's ScoreBuildingConstruction gives a player's piece theirs (#120): its team can
    // edit it, and its shots and its teammates' pass it as their own.
    if (PlayerState && Building->Team == EFortTeam::GetSpectator())
    {
        Building->Team = PlayerState->TeamIndex;
    }
    UAthenaNavSystem::AddDirtyArea(Building);
    Materials[Kind] -= BuildingPieceCost;
    ++PiecesBuilt;
    static int32_t Logged = 0;
    if (Logged++ < 24)
    {
        Log::Info(L"Battle Royale bots: {} builds {} ({}, {} wood, {} stone, {} metal left)", BotName, NameOf(reinterpret_cast<UObject*>(Building)), BuildQueue.Reason, Materials[0], Materials[1], Materials[2]);
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::Harvest(float Now)
{
#if WITH_SERVER_CODE
    // Whether a swing damages a building at all, learnt from the first swings of any bot.
    static bool bHarvestWorks = true;
    static int32_t Misses = 0;
    static int32_t Hits = 0;
    static bool bLoggedPath = false;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!bHarvestWorks || !Pawn)
    {
        return false;
    }
    ABuildingSMActor* Target = HarvestTarget.Get();
    if (Target && (Target->IsPendingKill() || Target->bDestroyed || Target->GetHealth() <= 0.0f))
    {
        StopHarvesting();
        Target = nullptr;
    }
    // Materials: out of a fight, up to a stack of each a player would build with (Core's 🟡 300 in all).
    const int32_t Total = Materials[0] + Materials[1] + Materials[2];
    if (IsInCombat(Now) || (!bHarvestToUnstick && Total >= 300))
    {
        if (Target)
        {
            StopHarvesting();
        }
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    if (!Target)
    {
        if (bHarvestToUnstick || Now < NextHarvestSearch)
        {
            bHarvestToUnstick = false;
            return false;
        }
        NextHarvestSearch = Now + 3.0f;
        // A tree, rock, wall or car next to the bot: traces around it at waist height.
        float Best = 1e9f;
        const FVector Start(Location.X, Location.Y, Location.Z + 20.0f);
        for (int32_t Step = 0; Step < 8; ++Step)
        {
            const FVector Direction = RotateYaw(FVector(1.0f, 0.0f, 0.0f), 45.0f * static_cast<float>(Step));
            FVector Hit;
            UObject* HitActor = nullptr;
            UObject* HitComponent = nullptr;
            if (!UKismetSystemLibrary::LineTraceSingle(Pawn, Start, Start + Direction * 800.0f, 0, false, true, Hit, HitActor, HitComponent))
            {
                continue;
            }
            ABuildingSMActor* Building = HitActor ? Cast<ABuildingSMActor>(HitActor) : nullptr;
            const float Distance = Dist2D(Hit, Location);
            const uint8_t Resource = Building ? static_cast<uint8_t>(Building->ResourceType) : 0;
            const bool bHarvestable = Building && (Resource == EFortResourceType::GetWood() || Resource == EFortResourceType::GetStone() || Resource == EFortResourceType::GetMetal());
            if (bHarvestable && !Building->bPlayerPlaced && Building->GetHealth() > 0.0f && !IsUnreachable(Building) && Distance < Best)
            {
                Best = Distance;
                Target = Building;
                HarvestPoint = Hit;
            }
        }
        if (!Target)
        {
            return false;
        }
        HarvestTarget = Target;
        HarvestTargetTime = Now;
    }
    if (Now - HarvestTargetTime > (bHarvestToUnstick ? 30.0f : 20.0f))
    {
        MarkUnreachable(Target);
        StopHarvesting();
        return false;
    }
    if (Dist2D(HarvestPoint, Location) > 170.0f)
    {
        MoveTowards(HarvestPoint, Now, 120.0f);
        return true;
    }
    StopMoving();
    FaceTowards(HarvestPoint, Now, 1.0f);
    EquipPickaxe();
    if (Now < NextHarvestSwing)
    {
        return true;
    }
    NextHarvestSwing = Now + 0.9f;
    ++Swings;
    Pawn->PawnStartFire(0);
    Pawn->PawnStopFire(0);
    float Damage = 0.0f;
    bHarvestCredited = false;
    const bool bApplied = SwingAtBuilding(Target, Damage);
    if (!bApplied || Damage <= 0.0f)
    {
        MarkUnreachable(Target);
        StopHarvesting();
        if (Hits == 0 && ++Misses >= 5)
        {
            bHarvestWorks = false;
            Log::Warning(L"Battle Royale bots: harvesting is off: {} swings at buildings did no damage (the pickaxe's damage effect{})", Misses,
                bApplied ? L" applies but its damage execution gives 0" : L" couldn't be applied");
        }
        return false;
    }
    ++Hits;
    // The materials: the building's own resource spawn (Core's ABuildingSMActor::AttemptSpawnResources, which credits
    // a bot instigator). The game's damage response calls it for a player's hit; when it didn't for the bot's, Core does.
    if (!bHarvestCredited)
    {
        Target->AttemptSpawnResources(Pawn, Damage, false);
    }
    if (!bLoggedPath)
    {
        bLoggedPath = true;
        Log::Info(L"Battle Royale bots: a pickaxe hit's materials come from {}", bHarvestCredited ? L"the building's damage response (AttemptSpawnResources)" : L"Core's call of AttemptSpawnResources");
    }
    return true;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::PlayReplicatedMontage(UObject* Montage, float PlayRate, bool bStop)
{
#if WITH_SERVER_CODE
    // How an ability's PlayMontageAndWait reaches other players (UAbilitySystemComponent::PlayMontage): the ability
    // system's replicated montage, FGameplayAbilityRepAnimMontage RepAnimMontageInfo, which each client plays in
    // OnRep_ReplicatedAnimMontage for a pawn it doesn't control, a new montage or a flipped ForcePlayBit starting it.
    // AFortPlayerPawnAthena keeps its own copy (RepAnimMontageInfo, OnRep_ReplicatedAnimMontage) and gets the same.
    // The bot's reload and healing abilities can't run (no inventory owner), so Core sets it as they would.
    static UScriptStruct* RepStruct = FindObject<UScriptStruct>(nullptr, L"/Script/GameplayAbilities.GameplayAbilityRepAnimMontage");
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UObject* AbilitySystem = Pawn ? reinterpret_cast<UObject*>(static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent)) : nullptr;
    if (!RepStruct || !AbilitySystem || (!Montage && !bStop))
    {
        return false;
    }
    const auto Field = [](const wchar_t* Name) { return RepStruct->FindPropertyByName(FName(Name, FNAME_Find)); };
    static UProperty* MontageField = Field(L"AnimMontage");
    static UProperty* RateField = Field(L"PlayRate");
    static UProperty* PositionField = Field(L"Position");
    static UProperty* BlendField = Field(L"BlendTime");
    static UBoolProperty* StoppedField = static_cast<UBoolProperty*>(Field(L"IsStopped"));
    static UBoolProperty* PlayBitField = static_cast<UBoolProperty*>(Field(L"ForcePlayBit"));
    static UBoolProperty* SkipCorrectionField = static_cast<UBoolProperty*>(Field(L"SkipPositionCorrection"));
    if (!MontageField || !RateField || !PositionField || !StoppedField || !PlayBitField)
    {
        return false;
    }
    UProperty* AbilityInfoProperty = AbilitySystem->GetClass()->FindPropertyByName(FName(L"RepAnimMontageInfo", FNAME_Find));
    uint8_t* AbilityInfo = AbilityInfoProperty ? reinterpret_cast<uint8_t*>(AbilitySystem) + AbilityInfoProperty->GetOffset_ForInternal() : nullptr;
    // The game's own way first, as a weapon's equip animation reaches the players (AFortWeapon's equip calls the pawn's
    // PlayAnimMontage): AFortPawn::PlayAnimMontage (4.5 0xF5E240) is UAbilitySystemComponent::PlayMontage (0x58AA00),
    // which sets the ability system's LocalAnimMontageInfo and RepAnimMontageInfo together, and StopAnimMontage
    // (0xF6CC30) is its CurrentMontageStop. Writing RepAnimMontageInfo alone showed nothing once the bot had equipped a
    // weapon (#363): from then on its LocalAnimMontageInfo holds the equip montage, and the ability system's tick
    // (UAbilitySystemComponent::TickComponent, 0x591890, AnimMontage_UpdateReplicatedData 0x5619A0 on the server)
    // copies that montage back over RepAnimMontageInfo before the frame replicates. The pawn's copy follows the
    // ability system's when the pawn replicates (AFortPlayerPawnAthena, 0xAA5300).
    if (bStop)
    {
        static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->StopAnimMontage(nullptr);
    }
    else if (static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->PlayAnimMontage(Montage, PlayRate > 0.0f ? PlayRate : 1.0f) > 0.0f && AbilityInfo && *reinterpret_cast<UObject**>(AbilityInfo + MontageField->GetOffset_ForInternal()) == Montage)
    {
        // A montage Core wrote before skips the clients' position and next-section corrections; PlayMontage leaves the
        // flag as it found it.
        if (SkipCorrectionField)
        {
            SkipCorrectionField->SetPropertyValue_InContainer(AbilityInfo, false);
        }
        Pawn->ForceNetUpdate();
        static int32_t LoggedGame = 0;
        if (LoggedGame++ < 6)
        {
            Log::Info(L"Battle Royale bots: {} plays {} for the players (the pawn's PlayAnimMontage)", BotName, NameOf(Montage));
        }
        return true;
    }
    // Where the game's way doesn't replicate it (no anim instance on the server, or a pawn whose PlayAnimMontage
    // doesn't go through its ability system): the replicated records as an ability's montage fills them.
    int32_t Written = 0;
    for (UObject* Owner : { AbilitySystem, reinterpret_cast<UObject*>(Pawn) })
    {
        UProperty* Property = Owner->GetClass()->FindPropertyByName(FName(L"RepAnimMontageInfo", FNAME_Find));
        if (!Property)
        {
            continue;
        }
        uint8_t* Info = reinterpret_cast<uint8_t*>(Owner) + Property->GetOffset_ForInternal();
        if (bStop)
        {
            StoppedField->SetPropertyValue_InContainer(Info, true);
        }
        else
        {
            *reinterpret_cast<UObject**>(Info + MontageField->GetOffset_ForInternal()) = Montage;
            *reinterpret_cast<float*>(Info + RateField->GetOffset_ForInternal()) = PlayRate > 0.0f ? PlayRate : 1.0f;
            *reinterpret_cast<float*>(Info + PositionField->GetOffset_ForInternal()) = 0.0f;
            if (BlendField)
            {
                *reinterpret_cast<float*>(Info + BlendField->GetOffset_ForInternal()) = 0.25f;
            }
            StoppedField->SetPropertyValue_InContainer(Info, false);
            PlayBitField->SetPropertyValue_InContainer(Info, !PlayBitField->GetPropertyValue_InContainer(Info));
            if (SkipCorrectionField)
            {
                SkipCorrectionField->SetPropertyValue_InContainer(Info, true);
            }
        }
        ++Written;
    }
    static int32_t Logged = 0;
    if (!bStop && Logged++ < 6)
    {
        Log::Info(L"Battle Royale bots: {} plays {} for the players ({} replicated montage records)", BotName, NameOf(Montage), Written);
    }
    return Written > 0;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::IsDBNO() const
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    return Pawn && Pawn->bIsDBNO;
}

bool FCoreAthenaBotController::CanGoDBNO() const
{
    // Battle Royale's DBNO (GAB_AthenaDBNO): in a team playlist a player whose teammate is still up goes down instead of
    // out; a team all down is eliminated. The teammate is the game's (FCoreAthenaBotManager::HasStandingTeammate), so a
    // bot Core's damage knocks and one a player's knocks go down alike.
    return Manager->GetTeamSize() > 1 && IsAlive() && !IsDBNO() && Manager->HasStandingTeammate(this);
}

namespace
{
    // A row of AthenaGameData (/Game/Athena/Balance/DataTables/AthenaGameData), the curve table the DBNO abilities and
    // effects read (Default.DBNO.InitialHealth, BleedDamage, BleedTick), or Default when it hasn't the row.
    float AthenaGameDataValue(const wchar_t* Row, float Default)
    {
        static UObject* Table = LoadObject<UObject>(nullptr, L"/Game/Athena/Balance/DataTables/AthenaGameData.AthenaGameData");
        static UFunction* Evaluate = FindObject<UFunction>(nullptr, L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow");
        static UClass* LibraryClass = FindObject<UClass>(nullptr, L"/Script/Engine.DataTableFunctionLibrary");
        UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
        Reflection::FParams Params(Evaluate, true);
        if (!Table || !Library || !Params.Set(L"CurveTable", Table) || !Params.Set(L"RowName", FName(Row)) || !Params.Set(L"InXY", 0.0f) || !Params.Call(Library)
            || Params.Take<uint8_t>(L"OutResult") != 0)
        {
            return Default;
        }
        return Params.Take<float>(L"OutXY");
    }

    // GE_Athena_DBNO_Start heals a downed pawn to Default.DBNO.InitialHealth; GE_Athena_DBNO_Bleed takes
    // Default.DBNO.BleedDamage every Default.DBNO.BleedTick s.
    float DBNOInitialHealth()
    {
        static const float InitialHealth = std::max(AthenaGameDataValue(L"Default.DBNO.InitialHealth", 100.0f), 1.0f);
        return InitialHealth;
    }

    float DBNOBleedTick()
    {
        static const float BleedTick = std::max(AthenaGameDataValue(L"Default.DBNO.BleedTick", 1.0f), 0.1f);
        return BleedTick;
    }

    // GAB_AthenaDBNO is active on the pawn (it applies the bleed, GE_Athena_DBNO_Bleed, and removes it as it ends).
    bool HasDBNOAbilityActive(AFortPlayerPawnAthena* Pawn)
    {
        UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
        static UClass* DBNOAbility = LoadObject<UClass>(nullptr, L"/Game/Abilities/NPC/Generic/GAB_AthenaDBNO.GAB_AthenaDBNO_C");
        FGameplayAbilitySpec* Spec = AbilitySystem && DBNOAbility ? AbilitySystem->FindAbilitySpecFromClass(DBNOAbility) : nullptr;
        return Spec && Spec->ActiveCount > 0;
    }

    // UAbilitySystemBlueprintLibrary::SendGameplayEventToActor, with the payload's instigator.
    bool SendGameplayEvent(AActor* Actor, const wchar_t* Tag, UObject* Instigator)
    {
        static UFunction* Send = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:SendGameplayEventToActor");
        static UClass* LibraryClass = FindObject<UClass>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary");
        static UScriptStruct* PayloadStruct = FindObject<UScriptStruct>(nullptr, L"/Script/GameplayAbilities.GameplayEventData");
        static UProperty* InstigatorProperty = PayloadStruct ? PayloadStruct->FindPropertyByName(FName(L"Instigator", FNAME_Find)) : nullptr;
        static UProperty* TargetProperty = PayloadStruct ? PayloadStruct->FindPropertyByName(FName(L"Target", FNAME_Find)) : nullptr;
        UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
        const FName TagName(Tag, FNAME_Find);
        Reflection::FParams Params(Send, true);
        uint8_t* Payload = static_cast<uint8_t*>(Params.GetAddress(L"Payload"));
        if (!Library || TagName.IsNone() || !Payload || !Params.Set(L"Actor", Actor) || !Params.Set(L"EventTag", FGameplayTag{ TagName }))
        {
            return false;
        }
        if (InstigatorProperty)
        {
            *reinterpret_cast<UObject**>(Payload + InstigatorProperty->GetOffset_ForInternal()) = Instigator;
        }
        if (TargetProperty)
        {
            *reinterpret_cast<UObject**>(Payload + TargetProperty->GetOffset_ForInternal()) = reinterpret_cast<UObject*>(Actor);
        }
        return Params.Call(Library);
    }
}

void FCoreAthenaBotController::EnterDBNO(AFortPlayerStateAthena* InDowner, float Now)
{
#if WITH_SERVER_CODE
    // Down, not out: the pawn's bIsDBNO (replicated: clients show it crawling, with its DBNO icon), its DBNO health
    // (AthenaGameData Default.DBNO.InitialHealth), the box teammates revive through, and the game's own DBNO ability,
    // GAB_AthenaDBNO, which a player's death event (Event.Damage.DiedAthena) triggers: its montage (DBNO_Athena_Intro), its
    // start effect and its bleed effect (GE_Athena_DBNO_Bleed). The native damage response that sends that event for a
    // player doesn't run for a bot's Core-made hit, so Core sends it.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Pawn->bIsDBNO)
    {
        return;
    }
    Pawn->bIsDBNO = true;
    Pawn->SetHealth(DBNOInitialHealth());
    if (Natives::AFortPlayerPawn_UpdateDBNOInteractCollision)
    {
        Natives::AFortPlayerPawn_UpdateDBNOInteractCollision(reinterpret_cast<AFortPawn*>(Pawn));
    }
    UAbilitySystemComponent* AbilitySystem = static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent);
    static UClass* DBNOAbility = LoadObject<UClass>(nullptr, L"/Game/Abilities/NPC/Generic/GAB_AthenaDBNO.GAB_AthenaDBNO_C");
    bool bAbility = false;
    if (AbilitySystem && DBNOAbility && DBNOAbility->IsChildOf(UGameplayAbility::StaticClass()))
    {
        GiveAbility(AbilitySystem, DBNOAbility);
        AController* DownerController = InDowner ? Cast<AController>(reinterpret_cast<UObject*>(InDowner->GetOwner())) : nullptr;
        bAbility = SendGameplayEvent(reinterpret_cast<AActor*>(Pawn), L"Event.Damage.DiedAthena", reinterpret_cast<UObject*>(DownerController));
    }
    OnEnteredDBNO(InDowner, Now, true);
    Log::Info(L"Battle Royale bots: {} is down (DBNO){}, {:.0f} health, the DBNO ability {}", BotName, InDowner ? std::format(L", knocked by {}", InDowner->GetPlayerName().ToStdString()) : std::wstring(),
        Pawn->GetHealth(), bAbility ? L"triggered" : L"not triggered");
#endif
}

void FCoreAthenaBotController::OnEnteredDBNO(AFortPlayerStateAthena* InDowner, float Now, bool bHealed)
{
#if WITH_SERVER_CODE
    // Down by Core's knock (EnterDBNO, which gave the pawn its DBNO health) or the game's (a player's hit: Tick finds the
    // pawn DBNO at the knock's 0 health, the DBNO ability's start effect heals it a moment later): the bot stops what it
    // was doing and starts bleeding.
    StopFiring();
    CancelReload();
    CancelHeal();
    if (HarvestTarget.Get())
    {
        StopHarvesting();
    }
    StopMoving();
    EmoteUntil = 0.0f;
    EndReviveInteraction();
    ReviveTarget = nullptr;
    RevivePlayer = nullptr;
    ReviveDoneAt = 0.0f;
    Downer = InDowner;
    DBNOStartedAt = Now;
    bDBNOHealed = bHealed;
    NextBleedTime = Now + DBNOBleedTick();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    LastDBNOHealth = Pawn ? Pawn->GetHealth() : 0.0f;
    ++TimesDowned;
#endif
}

void FCoreAthenaBotController::Downed(float Now)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn)
    {
        return;
    }
    StopFiring();
    // The team all down: eliminated (the last one knocked is everyone's finisher). A teammate standing anywhere on the
    // team keeps the bot down, as the game's DBNO handling counts one; it crawls to the nearest of its squad.
    const bool bTeammateUp = Manager->HasStandingTeammate(this);
    bool bHasNearest = false;
    FVector Nearest;
    float NearestDistance = 1e9f;
    const FVector Location = LocationOf(Pawn);
    const auto Consider = [&](const FVector& Other)
    {
        const float Distance = Dist2D(Other, Location);
        if (Distance < NearestDistance)
        {
            NearestDistance = Distance;
            Nearest = Other;
            bHasNearest = true;
        }
    };
    for (FCoreAthenaBotController* Teammate : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() == ECoreAthenaBotState::Landed && !Teammate->IsDBNO())
        {
            Consider(LocationOf(Teammate->GetPawn()));
        }
    }
    // A bot on a player's team: a player teammate standing is one to crawl to (their revive is the game's own, through
    // the downed pawn's DBNO interaction; Tick notices the bot is up).
    for (AFortPlayerPawnAthena* Player : Manager->PlayerTeammatesOf(this))
    {
        if (!Player->bIsDBNO)
        {
            Consider(LocationOf(Player));
        }
    }
    AFortPlayerStateAthena* Finisher = Downer.Get();
    const auto Eliminate = [this, Pawn, Finisher](const wchar_t* Reason, const wchar_t* Why)
    {
        AController* Killer = Finisher ? Cast<AController>(reinterpret_cast<UObject*>(Finisher->GetOwner())) : nullptr;
        Log::Info(L"Battle Royale bots: {} is eliminated while down ({})", BotName, Why);
        Pawn->SetHealth(1.0f);
        Pawn->ForceKill(FGameplayTag{ FName(Reason, FNAME_Find) }, Killer, Killer ? reinterpret_cast<AActor*>(static_cast<APawn*>(Killer->Pawn)) : nullptr);
    };
    // The DBNO health: a pawn the game downed is at the knock's 0 health until GAB_AthenaDBNO's start effect
    // (GE_Athena_DBNO_Start, after the ability's 0.01 s delay) heals it to Default.DBNO.InitialHealth; that 0 isn't a
    // bleed-out (#248: bots knocked by players were eliminated the moment they went down). Core heals it the same when
    // the ability hasn't within half a second.
    if (!bDBNOHealed)
    {
        if (Pawn->GetHealth() > 0.5f)
        {
            bDBNOHealed = true;
        }
        else if (Now - DBNOStartedAt < 0.5f)
        {
            StopMoving();
            return;
        }
        else
        {
            Pawn->SetHealth(DBNOInitialHealth());
            bDBNOHealed = true;
            Log::Info(L"Battle Royale bots: {} had no DBNO health from the game's DBNO ability, Core gives it {:.0f}", BotName, Pawn->GetHealth());
        }
        LastDBNOHealth = Pawn->GetHealth();
        NextBleedTime = Now + DBNOBleedTick();
    }
    if (!bTeammateUp)
    {
        Eliminate(L"Gameplay.Damage.DBNOBleedOut", L"no teammate left standing");
        return;
    }
    // Bleeding: GE_Athena_DBNO_Bleed (AthenaGameData Default.DBNO.BleedDamage every Default.DBNO.BleedTick s) while the
    // DBNO ability that applied it is active (the effect pauses while a player revives the pawn,
    // Gameplay.Status.DBNOBeingRevived). Core bleeds the pawn the same only without the ability, when its health hasn't
    // moved.
    if (Now >= NextBleedTime)
    {
        static const float BleedDamage = AthenaGameDataValue(L"Default.DBNO.BleedDamage", 1.0f);
        const float BleedTick = DBNOBleedTick();
        NextBleedTime = Now + BleedTick;
        const float Health = Pawn->GetHealth();
        const bool bCoreBleeds = !HasDBNOAbilityActive(Pawn) && Health >= LastDBNOHealth - 0.01f && Health > 0.0f;
        if (bCoreBleeds)
        {
            Pawn->SetHealth(Health - BleedDamage);
        }
        static int32_t LoggedBleeds = 0;
        if (LoggedBleeds < 12)
        {
            ++LoggedBleeds;
            Log::Info(L"Battle Royale bots: {} bleeds while down: {:.0f} -> {:.0f} ({}, {:.1f} every {:.1f} s)", BotName, LastDBNOHealth, Pawn->GetHealth(),
                bCoreBleeds ? L"Core's bleed" : L"the game's bleed or a hit", BleedDamage, BleedTick);
        }
        LastDBNOHealth = Pawn->GetHealth();
    }
    if (Pawn->GetHealth() <= 0.5f)
    {
        Eliminate(L"Gameplay.Damage.DBNOBleedOut", L"bled out");
        return;
    }
    // Crawling to the nearest teammate standing, who comes to revive it.
    if (bHasNearest && NearestDistance > 250.0f)
    {
        MoveTowards(Nearest, Now, 200.0f);
    }
    else
    {
        StopMoving();
    }
#endif
}

bool FCoreAthenaBotController::ReviveTeammate(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's bots revive their downed teammates (the community list: "reviving each other"): out of a fight, the nearest
    // downed teammate within 60 m, the pawn's ReviveFromDBNOTime standing by it, then the game's own revive
    // (AFortPlayerPawn::ReviveFromDBNO, BlueprintAuthorityOnly; Core's restored one where the build strips it).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Manager->GetTeamSize() <= 1)
    {
        return false;
    }
    // The downed teammate: a bot (ReviveTarget), or a player (RevivePlayer) for a bot on a player's team.
    FCoreAthenaBotController* Target = ReviveTarget;
    AFortPlayerPawnAthena* PlayerTarget = Target ? nullptr : RevivePlayer.Get();
    // A revive is safe: no enemy in sight and nobody shooting at the bot, and neither of them in the storm (the bot
    // goes for the circle first; the downed teammate crawls after it).
    FVector StormCenter;
    float StormRadius = 0.0f;
    const bool bHasStorm = Manager->GetCurrentSafeZone(StormCenter, StormRadius);
    const auto InStorm = [&](AFortPlayerPawnAthena* Someone)
    {
        return bHasStorm && Someone && Dist2D(LocationOf(Someone), StormCenter) > StormRadius;
    };
    const auto TargetPawn = [&]() { return Target ? Target->GetPawn() : PlayerTarget; };
    const auto TargetName = [&]() { return Target ? Target->GetBotName() : PlayerTarget ? NameOf(PlayerTarget) : std::wstring(); };
    const bool bThreat = IsUnderThreat(Now) || (CurrentTarget.Get() && bHasVisualLineOfSightOnTarget) || InStorm(Pawn) || InStorm(TargetPawn());
    const bool bTargetValid = Target ? Target->IsAlive() && Target->IsDBNO() && Target->GetPawn()
        : PlayerTarget && PlayerTarget->bIsDBNO && !PlayerTarget->bIsDying && !PlayerTarget->IsPendingKill();
    if (!bTargetValid || bThreat)
    {
        if ((Target || PlayerTarget) && ReviveDoneAt > 0.0f)
        {
            Log::Info(L"Battle Royale bots: {} stops reviving {}", BotName, TargetName());
        }
        EndReviveInteraction();
        ReviveTarget = nullptr;
        RevivePlayer = nullptr;
        ReviveDoneAt = 0.0f;
        Target = nullptr;
        PlayerTarget = nullptr;
        if (bThreat)
        {
            return false;
        }
    }
    const FVector Location = LocationOf(Pawn);
    if (!Target && !PlayerTarget)
    {
        float Best = 6000.0f;
        for (FCoreAthenaBotController* Teammate : Manager->TeammatesOf(this))
        {
            const float Distance = Teammate->IsDBNO() && Teammate->GetPawn() && !InStorm(Teammate->GetPawn()) ? Dist2D(LocationOf(Teammate->GetPawn()), Location) : 1e9f;
            if (Distance < Best)
            {
                Best = Distance;
                Target = Teammate;
            }
        }
        // A bot on a player's team revives its downed player teammates too.
        for (AFortPlayerPawnAthena* Player : Manager->PlayerTeammatesOf(this))
        {
            const float Distance = Player->bIsDBNO && !InStorm(Player) ? Dist2D(LocationOf(Player), Location) : 1e9f;
            if (Distance < Best)
            {
                Best = Distance;
                Target = nullptr;
                PlayerTarget = Player;
            }
        }
        if (!Target && !PlayerTarget)
        {
            return false;
        }
        ReviveTarget = Target;
        RevivePlayer = PlayerTarget;
        ReviveDoneAt = 0.0f;
    }
    AFortPlayerPawnAthena* Downed = TargetPawn();
    const FVector DownedLocation = LocationOf(Downed);
    if (Dist2D(DownedLocation, Location) > 180.0f)
    {
        // Out of the interaction's reach (the downed pawn crawled off, or the bot was pushed): it starts over there.
        if (ReviveDoneAt > 0.0f)
        {
            EndReviveInteraction();
        }
        ReviveDoneAt = 0.0f;
        MoveTowards(DownedLocation, Now, 120.0f);
        return true;
    }
    StopMoving();
    FaceTowards(DownedLocation, Now, 1.0f);
    if (ReviveDoneAt <= 0.0f)
    {
        // A player holds the interaction with the downed pawn: hands busy, no gun up, no reload or heal under way. The
        // interaction's start is the game's ServerDBNOReviveStarted: the downed pawn's revive cue and tag (its "being
        // revived" animation, the bleed paused), as for a player's revive (CoreV2 #287: the bots reloaded and looked
        // about as they revived, and no revive showed).
        StopFiring();
        SetTargeting(false);
        CancelReload();
        CancelHeal();
        BeginReviveInteraction(Downed);
        const float ReviveTime = NumberOf(reinterpret_cast<UObject*>(Downed), L"ReviveFromDBNOTime", 10.0f);
        ReviveDoneAt = Now + std::max(ReviveTime, 1.0f);
        Log::Info(L"Battle Royale bots: {} revives {} ({:.0f} s, {})", BotName, TargetName(), ReviveTime,
            ReviveInteractionPawn.Get() ? L"the game's revive interaction" : L"no revive interaction in this build");
        return true;
    }
    if (Now < ReviveDoneAt)
    {
        return true;
    }
    ReviveTarget = nullptr;
    RevivePlayer = nullptr;
    ReviveDoneAt = 0.0f;
    static UFunction* Revive = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawn:ReviveFromDBNO");
    Reflection::FParams Params(Revive, true);
    AAIController* Reviver = Controller.Get();
    if (Revive && Params.Set(L"EventInstigator", reinterpret_cast<AController*>(Reviver)))
    {
        Params.Call(reinterpret_cast<UObject*>(Downed));
    }
    const bool bNative = !Downed->bIsDBNO;
    if (!bNative)
    {
        // The game's revive refused the AI controller (CanReviveFromDBNO): Core gets the pawn up as the revive effect
        // would (GE_Athena_Revive: AthenaGameData Default.DBNO.ReviveHealth), and ends its DBNO ability.
        static const float ReviveHealth = AthenaGameDataValue(L"Default.DBNO.ReviveHealth", 30.0f);
        Downed->bIsDBNO = false;
        Downed->SetHealth(std::max(ReviveHealth, 1.0f));
        if (Natives::AFortPlayerPawn_UpdateDBNOInteractCollision)
        {
            Natives::AFortPlayerPawn_UpdateDBNOInteractCollision(reinterpret_cast<AFortPawn*>(Downed));
        }
        SendGameplayEvent(reinterpret_cast<AActor*>(Downed), L"Event.RevivedAthena", reinterpret_cast<UObject*>(Reviver));
        UAbilitySystemComponent* AbilitySystem = static_cast<UAbilitySystemComponent*>(Downed->AbilitySystemComponent);
        const FName DBNOTag(L"Gameplay.Action.Player.DBNOAthena", FNAME_Find);
        if (AbilitySystem && !DBNOTag.IsNone() && Natives::UAbilitySystemComponent_CancelAbilities)
        {
            FGameplayTagContainer Tags;
            Tags.GameplayTags.Add(FGameplayTag{ DBNOTag });
            Natives::UAbilitySystemComponent_CancelAbilities(AbilitySystem, &Tags, nullptr, nullptr);
        }
    }
    // Up: out of its "being revived" state, as AFortPlayerPawnAthena's ServerReviveFromDBNO ends it after a player's.
    EndReviveInteraction();
    if (Target)
    {
        Target->OnRevived();
    }
    ++Revives;
    Log::Info(L"Battle Royale bots: {} revived {} ({}), {:.0f} health", BotName, TargetName(), bNative ? L"the game's ReviveFromDBNO" : L"Core's, the game's refused", Downed->GetHealth());
    return true;
#else
    return false;
#endif
}

namespace
{
    // The slot of an AFortPlayerControllerAthena RPC's _Implementation (from its exec), or -1 where the build lacks it or
    // strips it to the shared stub.
    int32_t AthenaControllerImplementationIndex(const wchar_t* Path)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, Path);
        const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
        UClass* Class = Index >= 0 ? AFortPlayerControllerAthena::StaticClass() : nullptr;
        UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        return VTable && VTable[Index] && !Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])) ? Index : -1;
    }
}

void FCoreAthenaBotController::BeginReviveInteraction(AFortPlayerPawnAthena* Downed)
{
#if WITH_SERVER_CODE
    // AFortPlayerControllerAthena::ServerDBNOReviveStarted_Implementation (3.5.1 0xA2CF00, 4.1 0xA6D140), which a
    // player's client sends as it starts holding the interaction on the downed pawn: the pawn's DBNORevivalStacking goes
    // up, and with the first reviver its ability system gets Gameplay.Status.DBNOBeingRevived (which pauses its bleed)
    // and the replicated GameplayCue.Athena.Player.BeingRevivedFromDBNO (its revive animation and effect on every
    // client). The body reads nothing of the controller it runs on but its player state, for the reviver's squad status;
    // a bot's controller is an AAIController, so it runs on the class default, as Core's restored ServerReviveFromDBNO
    // runs ServerDBNOReviveInterrupted (that status is left out).
    static const int32_t Index = AthenaControllerImplementationIndex(L"/Script/FortniteGame.FortPlayerControllerAthena:ServerDBNOReviveStarted");
    EndReviveInteraction();
    UClass* Class = AFortPlayerControllerAthena::StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    if (!Downed || !Downed->bIsDBNO || Index < 0 || !Default || !AFortPlayerPawnAthena::HasDBNORevivalStacking())
    {
        return;
    }
    CallVirtual<void>(reinterpret_cast<AFortPlayerControllerAthena*>(Default), Index, Downed);
    ReviveInteractionPawn = Downed;
#endif
}

void FCoreAthenaBotController::EndReviveInteraction()
{
#if WITH_SERVER_CODE
    // ServerDBNOReviveInterrupted_Implementation (3.5.1 0xA2CE10): one reviver fewer, and with the last one gone the tag
    // and the cue come off. Its count is a byte taken down unchecked, so it is only called on a pawn the bot is counted
    // on.
    static const int32_t Index = AthenaControllerImplementationIndex(L"/Script/FortniteGame.FortPlayerControllerAthena:ServerDBNOReviveInterrupted");
    AFortPlayerPawnAthena* Downed = ReviveInteractionPawn.Get();
    ReviveInteractionPawn = nullptr;
    UClass* Class = AFortPlayerControllerAthena::StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    if (!Downed || Downed->IsPendingKill() || Index < 0 || !Default || static_cast<uint8_t>(Downed->DBNORevivalStacking) == 0)
    {
        return;
    }
    CallVirtual<void>(reinterpret_cast<AFortPlayerControllerAthena*>(Default), Index, Downed);
#endif
}

void FCoreAthenaBotController::OnRevived()
{
    Downer = nullptr;
    DBNOStartedAt = 0.0f;
    LastHurtTime = -100.0f;
    StopMoving();
}

bool FCoreAthenaBotController::StayWithTeam(float Now)
{
#if WITH_SERVER_CODE
    // Teammates stick together (the community list): a bot with nothing to do rejoins the nearest teammate standing when
    // it has drifted more than 25 m away.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Manager->GetTeamSize() <= 1)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    float Best = 1e9f;
    FVector TeammateLocation;
    // A bot on a player's team follows its players: the nearest one standing on the ground, before any bot teammate.
    for (AFortPlayerPawnAthena* Player : Manager->PlayerTeammatesOf(this))
    {
        if (Player->bIsDBNO || Player->bIsSkydiving || Player->bIsParachuteOpen)
        {
            continue;
        }
        const FVector Other = LocationOf(Player);
        const float Distance = Dist2D(Other, Location);
        if (Distance < Best)
        {
            Best = Distance;
            TeammateLocation = Other;
        }
    }
    for (FCoreAthenaBotController* Teammate : Best < 1e9f ? std::vector<FCoreAthenaBotController*>() : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() != ECoreAthenaBotState::Landed || Teammate->IsDBNO() || !Teammate->GetPawn())
        {
            continue;
        }
        const FVector Other = LocationOf(Teammate->GetPawn());
        const float Distance = Dist2D(Other, Location);
        if (Distance < Best)
        {
            Best = Distance;
            TeammateLocation = Other;
        }
    }
    // Once on its way back, it goes all the way (it stopped at 25 m, wandered off and came back).
    if (Best > (Goal == ECoreAthenaBotGoal::Regroup ? 1000.0f : 2500.0f) && Best < 40000.0f)
    {
        // The wander goal it had is behind it now: back with the team, it picks a new one there (Wander), instead of
        // walking out to it again and being called back at 25 m, over and over (CoreV2 #264).
        bHasWanderGoal = false;
        MoveTowards(TeammateLocation, Now, 800.0f);
        return true;
    }
    return false;
#else
    return false;
#endif
}

bool FCoreAthenaBotController::FollowMapMarker(float Now)
{
#if WITH_SERVER_CODE
    // The squad's map marker (3.3: a player's MapIndicatorPos, ServerPlaceMapCursor's), looked for twice a second.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !bOnPlayersTeam)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    if (Now >= NextMapMarkerCheck)
    {
        NextMapMarkerCheck = Now + 0.5f;
        FVector Marker;
        std::wstring Player;
        if (!Manager->GetSquadMapMarker(this, Location, Marker, Player))
        {
            if (bHasMapMarker)
            {
                bHasMapMarker = false;
                Log::Info(L"Battle Royale bots: {}'s squad marker is gone, it goes back to playing", BotName);
            }
            return false;
        }
        // A new marker (or one moved): where it is on the ground.
        if (!bHasMapMarker || Dist2D(Marker, MapMarker) > 100.0f)
        {
            bHasMapMarker = true;
            bMapMarkerReached = false;
            MapMarker = Marker;
            MapMarkerGround = Marker;
            FVector Ground;
            if (Manager->FindGroundNear(FVector(Marker.X, Marker.Y, Location.Z), 200.0f, Ground))
            {
                MapMarkerGround = Ground;
            }
            ++MapMarkersFollowed;
            ClearNextDestination();
            HuntTarget = nullptr;
            Log::Info(L"Battle Royale bots: {} heads for {}'s map marker {:.0f} m away", BotName, Player, Dist2D(Location, MapMarkerGround) / 100.0f);
        }
    }
    if (!bHasMapMarker || bMapMarkerReached)
    {
        return false;
    }
    // The storm comes first: a marker outside the circle waits until the circle reaches it.
    FVector StormCenter;
    float StormRadius = 0.0f;
    if (Manager->GetCurrentSafeZone(StormCenter, StormRadius) && Dist2D(MapMarkerGround, StormCenter) > StormRadius)
    {
        return false;
    }
    // There: it takes what lies close by, then plays on from there (its goals: loot, the team, the circle). It stood at
    // the marker with nothing to do until the marker went or the storm came, and a marker left on the map (the drop's)
    // held it there for the whole circle (CoreV2 #275: "freeze and stand in place without a goal until the next zone").
    if (Dist2D(Location, MapMarkerGround) < 800.0f)
    {
        if (Loot(Now, 1500.0f) || SearchContainer(Now, 1500.0f))
        {
            return true;
        }
        bMapMarkerReached = true;
        Log::Info(L"Battle Royale bots: {} reached its squad's map marker and plays on", BotName);
        return false;
    }
    MoveTowards(MapMarkerGround, Now, 400.0f);
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::TryToUnstuck(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Movement::TryToUnstuck: a bot that wants to move and hasn't, jumps; still stuck,
    // it gives up on its goal and goes somewhere else.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Now < NextStuckCheck)
    {
        return;
    }
    NextStuckCheck = Now + 1.5f;
    const FVector Location = LocationOf(Pawn);
    const bool bMoving = bHasMoveGoal;
    // Giving up: the loot or container it was going for, and, stuck again near where it gave up last time, the far goal
    // it keeps coming back to (the POI it was heading for, the player it hunted); then a detour away from where it was
    // going. CoreV2 #233: bots stuck wandering around the same spot walked their detour and went back to the goal they
    // couldn't reach, again and again (a POI was given up only after 3 minutes, a hunted player never).
    const auto GiveUp = [&](const wchar_t* Why)
    {
        StuckChecks = 0;
        MarkUnreachable(LootTarget.Get());
        MarkUnreachable(ContainerTarget.Get());
        LootTarget = nullptr;
        ContainerTarget = nullptr;
        SearchUntil = 0.0f;
        NextContainerSearch = Now + 6.0f;
        const bool bAgain = Now - LastGiveUpTime < 30.0f && Dist2D(Location, LastGiveUpLocation) < 1500.0f;
        LastGiveUpTime = Now;
        LastGiveUpLocation = Location;
        if (bAgain && Goal == ECoreAthenaBotGoal::TravelToPOI && NextPOI >= 0)
        {
            const FCoreAthenaBotLandingSpot* Spot = Manager->GetPOI(NextPOI);
            Log::Info(L"Battle Royale bots: {} gives up on {} ({}, {:.0f} m short of it)", BotName, Spot ? Spot->Name : std::wstring(L"its next spot"), Why, Dist2D(Location, POIDestination) / 100.0f);
            ClearNextDestination();
        }
        if (bAgain && Goal == ECoreAthenaBotGoal::Hunt && HuntTarget.Get())
        {
            Log::Info(L"Battle Royale bots: {} stops going after {} ({})", BotName, NameOf(HuntTarget.Get()), Why);
            HuntTarget = nullptr;
            NextHuntSearch = Now + 20.0f;
        }
        // The detour: 7 m off to a side or back, away from the way that didn't go.
        const FVector Away = bHasMoveGoal ? Direction2D(MoveGoal, Location) : FVector();
        const float Angle = Away.SizeSquared() > 0.01f ? std::atan2(Away.Y, Away.X) + RandomRange(-1.6f, 1.6f) : RandomRange(0.0f, 6.2831853f);
        bHasWanderGoal = true;
        WanderGoal = FVector(Location.X + std::cos(Angle) * 700.0f, Location.Y + std::sin(Angle) * 700.0f, Location.Z);
        NextWanderTime = Now + 3.0f;
        // On its way somewhere far (a POI, a player), it walks the detour first, then sets off again.
        DetourUntil = Now + 3.0f;
        ProgressDistance = -1.0f;
    };
    // Moving but getting no nearer (circling an obstacle, sliding along a wall, paths that end where it stands) isn't
    // caught by the 60 cm check below: 15 s without coming 3 m nearer to a goal over 10 m away counts as stuck too.
    if (bMoving && Now >= DetourUntil)
    {
        const float GoalDistance = Dist2D(Location, MoveGoal);
        if (ProgressDistance < 0.0f || Dist2D(MoveGoal, ProgressGoal) > 500.0f || GoalDistance < ProgressDistance - 300.0f)
        {
            ProgressGoal = MoveGoal;
            ProgressDistance = GoalDistance;
            ProgressSince = Now;
        }
        else if (GoalDistance > 1000.0f && Now - ProgressSince > 8.0f && FreeItself(Now, L"no nearer in 8 s"))
        {
            // Walking about on a roof with no way down, or along a cliff it can't get up: through it, or up it (8 s; it
            // gave up only after 15).
            ProgressDistance = -1.0f;
            LastStuckCheckLocation = Location;
            return;
        }
        else if (GoalDistance > 1000.0f && Now - ProgressSince > 15.0f)
        {
            GiveUp(L"no nearer in 15 s");
            LastStuckCheckLocation = Location;
            return;
        }
    }
    else if (!bMoving)
    {
        ProgressDistance = -1.0f;
    }
    // Held in one place: wanting to move and still within 2.5 m of where it was 4.5 s ago (CoreV2 #287: bots twitched
    // back and forth against walls, hills and in basements for a long time. Turning away from a wall for a second, the
    // path and the steering taking turns, or the detour and back moved them more than the 60 cm below between two
    // looks, so that check never counted up, and the one above needs 15 s and a goal 10 m away).
    const bool bBusy = HarvestTarget.Get() || !BuildQueue.Descriptors.empty();
    if (bMoving && !bBusy && !IsInCombat(Now))
    {
        if (ConfineSince < 0.0f || Dist2D(Location, ConfineAnchor) > 250.0f)
        {
            ConfineAnchor = Location;
            ConfineSince = Now;
        }
        else if (Now - ConfineSince > 4.5f)
        {
            ConfineSince = -1.0f;
            StuckChecks = 0;
            LastStuckCheckLocation = Location;
            if (!FreeItself(Now, L"held in one place"))
            {
                GiveUp(L"held in one place");
            }
            return;
        }
    }
    else
    {
        ConfineSince = -1.0f;
    }
    if (bMoving && Dist2D(Location, LastStuckCheckLocation) < 60.0f)
    {
        ++StuckChecks;
        if (!Pawn->bIsDBNO)
        {
            static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->Jump();
            ++Jumps;
        }
        // Something it can break is in the way (the pickaxe, as a stuck player does), or a slope it can build up. Looked
        // for before the path is let go: StopMoving below drops the move goal the look needs (it found nothing on a path).
        if (StuckChecks == 2 && FreeItself(Now, L"stuck"))
        {
            StuckChecks = 0;
            ConfineSince = -1.0f;
            LastStuckCheckLocation = Location;
            return;
        }
        if (StuckChecks == 2 && bPathFollowing)
        {
            // The path doesn't get it anywhere (something the navmesh doesn't know of is in the way): steer a moment.
            StopMoving();
            SteerReason = L"stuck on its path";
            PathFailedUntil = Now + 2.0f;
            bHasSteerTarget = false;
        }
        if (StuckChecks >= 3)
        {
            GiveUp(L"stuck");
        }
    }
    else
    {
        StuckChecks = 0;
    }
    LastStuckCheckLocation = Location;
#endif
}

void FCoreAthenaBotController::TakeStormDamage(float Now)
{
#if WITH_SERVER_CODE
    if (Now < NextStormTick)
    {
        return;
    }
    NextStormTick = Now + 1.0f;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    FVector Center;
    float Radius = 0.0f;
    if (!Pawn || !Pawn->bCanBeDamaged || !Manager->GetCurrentSafeZone(Center, Radius))
    {
        LastStormHealth = -1.0f;
        return;
    }
    const float Health = Pawn->GetHealth();
    if (Dist2D(LocationOf(Pawn), Center) <= Radius)
    {
        LastStormHealth = -1.0f;
        return;
    }
    // Outside: the game's own storm took some since the last second, or Core takes it.
    const bool bGameHurts = LastStormHealth >= 0.0f && Health < LastStormHealth - 0.1f;
    if (!bGameHurts)
    {
        const float NewHealth = Health - Manager->GetStormDamagePerSecond();
        if (NewHealth <= 0.5f)
        {
            Log::Info(L"Battle Royale bots: the storm takes {}", BotName);
            Pawn->SetHealth(1.0f);
            Pawn->ForceKill(FGameplayTag{ FName(L"Gameplay.Damage.OutsideSafeZone", FNAME_Find) }, nullptr, nullptr);
            return;
        }
        Pawn->SetHealth(NewHealth);
    }
    LastStormHealth = Pawn->GetHealth();
#endif
}

void FCoreAthenaBotController::DropItem(UFortItemDefinition* Definition, int32_t Count, int32_t LoadedAmmo, const FVector* DeathSpot)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    if (!Pawn || !World || !Definition || Count <= 0 || !EntryStruct)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    std::vector<uint8_t> Storage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(Storage.data());
    FFortItemEntry::Construct(Entry, Definition, Count, 0);
    Entry->LoadedAmmo = LoadedAmmo;
    // Spread around where the bot fell, as a player's death drop lands: each item at its own angle (the golden angle
    // from the last) and 120-280 uu out, so none land stacked.
    static uint32_t DropCount = 0;
    const float Angle = static_cast<float>(DropCount++) * 2.39996f + RandomRange(-0.3f, 0.3f);
    const float Radius = RandomRange(120.0f, 280.0f);
    const FVector Spot(Location.X + std::cos(Angle) * Radius, Location.Y + std::sin(Angle) * Radius, Location.Z);
    // Tossed from the pawn as a player's drop is (AFortPickup::TossPickup with the pawn as its owner): an untossed
    // pickup never took clients' auto-pickup.
    if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, *Entry, &Location))
    {
        if (DeathSpot)
        {
            // A death drop, as DropInventoryOnDeath tosses a player's.
            Pickup->PawnWhoDroppedPickup = reinterpret_cast<AFortPawn*>(Pawn);
            Pickup->TossOntoFloor(*DeathSpot, reinterpret_cast<AFortPawn*>(Pawn));
        }
        else
        {
            Pickup->TossPickup(Spot, reinterpret_cast<AFortPawn*>(Pawn), 0);
        }
    }
    EntryStruct->DestroyStruct(Entry);
#endif
}

void FCoreAthenaBotController::DropInventory()
{
#if WITH_SERVER_CODE
    // 8.30's bots drop their loot as players do: each weapon (with its magazine), the ammo and the healing items, where
    // the bot fell. A player's death drop (AFortPlayerControllerAthena::DropInventoryOnDeath) is followed: each item gets
    // its own spot on the rings around the body (GetInventoryDropSpots) and is tossed onto it (TossOntoFloor). The plain
    // toss the bots used falls short of the spot it is aimed at (#89: aimed 1.2 to 2.8m out, it comes down 10 to 35 out),
    // so a bot's loot lay in a pile (#339).
    HealingItem = nullptr;
    struct FBotDrop
    {
        UFortItemDefinition* Definition;
        int32_t Count;
        int32_t LoadedAmmo;
    };
    std::vector<FBotDrop> Drops;
    const auto AddDrop = [&Drops](UFortItemDefinition* Definition, int32_t Count, int32_t LoadedAmmo)
    {
        if (Definition && Count > 0)
        {
            Drops.push_back({ Definition, Count, LoadedAmmo });
        }
    };
    for (const FCoreAthenaBotWeapon& Weapon : WeaponList)
    {
        AddDrop(Weapon.WeaponDefinition, 1, Weapon.LoadedAmmo);
    }
    for (const auto& [Definition, Count] : Ammo)
    {
        AddDrop(Definition, Count, 0);
    }
    for (const auto& [Definition, Count] : Consumables)
    {
        AddDrop(Definition, Count, 0);
    }
    if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
    {
        AddDrop(GameData->WoodItemDefinition, Materials[0], 0);
        AddDrop(GameData->StoneItemDefinition, Materials[1], 0);
        AddDrop(GameData->MetalItemDefinition, Materials[2], 0);
    }
    if (AFortPlayerPawnAthena* Pawn = GetPawn(); Pawn && !Drops.empty())
    {
        const std::vector<FVector> Spots = AFortPlayerControllerAthena::GetInventoryDropSpots(reinterpret_cast<AActor*>(Pawn), LocationOf(Pawn), static_cast<int32_t>(Drops.size()));
        for (size_t Index = 0; Index < Drops.size() && Index < Spots.size(); ++Index)
        {
            DropItem(Drops[Index].Definition, Drops[Index].Count, Drops[Index].LoadedAmmo, &Spots[Index]);
        }
    }
    Materials[0] = Materials[1] = Materials[2] = 0;
    HarvestTarget = nullptr;
    WeaponList.clear();
    Ammo.clear();
    Consumables.clear();
#endif
}

bool FCoreAthenaBotController::Heal(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Heal. A player holds the item for its use time (the ability's TriggerDuration),
    // standing still; a hit or an enemy in sight interrupts it, and the item isn't used up.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn)
    {
        return false;
    }
    if (HealingItem)
    {
        if (IsInCombat(Now))
        {
            CancelHeal();
            return false;
        }
        StopMoving();
        if (Now < HealUntil)
        {
            return true;
        }
        const FCoreAthenaBotConsumable* Info = GetConsumableInfo(HealingItem);
        const float Health = Pawn->GetHealth();
        const float Shield = Pawn->GetShield();
        const float MaxHealth = std::max(Pawn->GetMaxHealth(), 1.0f);
        const float MaxShield = std::max(Pawn->GetMaxShield(), 1.0f);
        if (Info && Info->Health > 0.0f && Health < std::min(Info->HealthCap, MaxHealth))
        {
            Pawn->SetHealth(std::min(Health + Info->Health, std::min(Info->HealthCap, MaxHealth)));
        }
        if (Info && Info->Shield > 0.0f && Shield < std::min(Info->ShieldCap, MaxShield))
        {
            Pawn->SetShield(std::min(Shield + Info->Shield, std::min(Info->ShieldCap, MaxShield)));
        }
        LastHealthShield = Pawn->GetHealth() + Pawn->GetShield();
        ++Heals;
        Log::Info(L"Battle Royale bots: {} uses {}: {:.0f} health, {:.0f} shield (was {:.0f}, {:.0f})", BotName, NameOf(HealingItem), Pawn->GetHealth(), Pawn->GetShield(), Health, Shield);
        if (--Consumables[HealingItem] <= 0)
        {
            Consumables.erase(HealingItem);
        }
        CancelHeal();
        NextHealCheck = Now + 0.5f;
        return true;
    }
    if (Now < NextHealCheck || Consumables.empty() || IsInCombat(Now))
    {
        return false;
    }
    NextHealCheck = Now + 1.0f;
    // Not in the storm (the bot runs out of it first).
    FVector Center;
    float Radius = 0.0f;
    if (Manager->GetCurrentSafeZone(Center, Radius) && Dist2D(LastKnownPosition, Center) > Radius)
    {
        return false;
    }
    // The item that restores most for its time, without wasting much of it: at least 40 % of what it gives, and 25 or
    // its half (a bandage below its cap of 75, a medkit at 60 health or less, a shield potion at 75 shield or less, a
    // Chug Jug when both are low).
    const float Health = Pawn->GetHealth();
    const float Shield = Pawn->GetShield();
    const float MaxHealth = std::max(Pawn->GetMaxHealth(), 1.0f);
    const float MaxShield = std::max(Pawn->GetMaxShield(), 1.0f);
    UFortItemDefinition* Best = nullptr;
    float BestScore = 0.0f;
    for (const auto& [Definition, Count] : Consumables)
    {
        const FCoreAthenaBotConsumable* Info = Count > 0 ? GetConsumableInfo(Definition) : nullptr;
        if (!Info)
        {
            continue;
        }
        const float HealthGain = std::min(Info->Health, std::max(std::min(Info->HealthCap, MaxHealth) - Health, 0.0f));
        const float ShieldGain = std::min(Info->Shield, std::max(std::min(Info->ShieldCap, MaxShield) - Shield, 0.0f));
        const float Worth = Info->Health + Info->Shield;
        if (HealthGain + ShieldGain < std::min(Worth * 0.5f, 25.0f) || (HealthGain + ShieldGain) < Worth * 0.4f)
        {
            continue;
        }
        const float Score = (HealthGain * 1.2f + ShieldGain) / Info->UseTime;
        if (Score > BestScore)
        {
            BestScore = Score;
            Best = Definition;
        }
    }
    if (!Best)
    {
        return false;
    }
    StopFiring();
    StopMoving();
    CancelReload();
    HealingItem = Best;
    HealUntil = Now + GetConsumableInfo(Best)->UseTime;
    // The item in the bot's hands while it uses it, as players see a player's.
    if (UFortWeaponItemDefinition* ItemWeapon = Cast<UFortWeaponItemDefinition>(Best))
    {
        if (AFortWeapon* Held = Pawn->EquipWeaponDefinition(ItemWeapon, FGuid::NewGuid()))
        {
            EquippedWeapon = Held;
            EquippedIndex = -1;
            bPickaxeEquipped = false;
        }
        // Its use as players see it: the fire ability's montage (GA_Athena_Bandage's AnimMontageInfo: WrapBandages_Athena
        // at 0.75).
        UProperty* AbilityProperty = reinterpret_cast<UObject*>(ItemWeapon)->GetClass()->FindPropertyByName(FName(L"PrimaryFireAbility", FNAME_Find));
        UClass* AbilityClass = AbilityProperty ? reinterpret_cast<TAssetSubclassOf<UObject>*>(reinterpret_cast<uint8_t*>(ItemWeapon) + AbilityProperty->GetOffset_ForInternal())->LoadSynchronous() : nullptr;
        float PlayRate = 1.0f;
        if (UObject* Montage = AbilityClass ? ObjectOf(AbilityClass->GetDefaultObject(), L"AnimMontageInfo", L"MontageToPlay", &PlayRate, L"AnimPlayRate") : nullptr)
        {
            PlayReplicatedMontage(Montage, PlayRate);
        }
    }
    Log::Info(L"Battle Royale bots: {} starts using {} ({:.0f} health, {:.0f} shield, {:.1f} s)", BotName, NameOf(Best), Health, Shield, HealUntil - Now);
    // Sometimes it boxes itself in to heal (21.40's UFortAthenaAIBotEvaluator_Bunker, its HealingStatusKey).
    Bunker(Now, nullptr, false);
    return true;
#else
    return false;
#endif
}

void FCoreAthenaBotController::CancelHeal()
{
    if (!HealingItem)
    {
        return;
    }
    HealingItem = nullptr;
    HealUntil = 0.0f;
    PlayReplicatedMontage(nullptr, 0.0f, true);
    // Back to a gun (the item was in its hands), or the pickaxe.
    if (HasUsableWeapon())
    {
        EquipWeapon(BestWeaponIndex());
    }
    else if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        UAthenaPickaxeItemDefinition* Pickaxe = LoadoutPickaxe ? LoadoutPickaxe : DefaultPickaxe();
        if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
        {
            EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
            bPickaxeEquipped = true;
            EquippedIndex = -1;
        }
    }
}

void FCoreAthenaBotController::SetLandingSpot(const FVector& Spot, float Radius, float InJumpTime)
{
    bHasLandingSpot = true;
    LandingSpot = Spot;
    LandingRadius = Radius;
    JumpTime = InJumpTime;
}

void FCoreAthenaBotController::FollowTeammateDrop(const FVector& Where, bool bLanded, float Now)
{
    // Teammates drop together: on the bus, the bot jumps a moment after its player, aiming for the weapons around where
    // they are (JumpOffBus); falling, it heads for them, a few metres to its own side so the team doesn't land in a pile.
    const float Angle = static_cast<float>(BotIndex % 8) * 0.785398f;
    const FVector Beside(Where.X + std::cos(Angle) * 400.0f, Where.Y + std::sin(Angle) * 400.0f, Where.Z);
    if (State == ECoreAthenaBotState::InAircraft)
    {
        if (!bFollowingDrop)
        {
            bFollowingDrop = true;
            bHasLandingSpot = true;
            LandingSpot = Beside;
            LandingRadius = 2500.0f;
            JumpTime = std::min(JumpTime, Now + RandomRange(0.3f, 1.5f));
            Log::Info(L"Battle Royale bots: {} jumps after its player teammate", BotName);
        }
        return;
    }
    if (State == ECoreAthenaBotState::Skydiving)
    {
        bFollowingDrop = true;
        // Its height is the ground's once the player is down; until then the spot keeps the height it had.
        DropTarget = FVector(Beside.X, Beside.Y, bLanded ? Where.Z : DropTarget.Z);
    }
}

void FCoreAthenaBotController::OnPossesedPawnDied(AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortAthenaAIBotController::OnPossesedPawnDied. The bot's player state takes its death report as a
    // player's does (AFortPlayerControllerAthena's handler): who eliminated it (else itself) and how; it replicates
    // (OnRep_DeathInfo), which writes the clients' kill feed.
    StopFiring();
    EndReviveInteraction();
    ReviveTarget = nullptr;
    RevivePlayer = nullptr;
    ReviveDoneAt = 0.0f;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    // The death's effects (OnDeathPlayEffects: the elimination drone, BP_VictoryDrone) play on a client as the dead
    // pawn's actor channel closes torn off: AFortPlayerPawnAthena::TornOff (3.3 0xA34600), from UActorChannel::CleanUp
    // (0x205ACA0). The damage cue's death path doesn't play them for an Athena pawn (its PlayDying, 0xA2B5D0, leaves out
    // AFortPawn's, 0xE6EA70). The death tears the pawn off (0xA21190), the server replicates that and closes the
    // channel; but an RPC sent on the torn-off pawn afterwards opens a new channel to it (InternalProcessRemoteFunction,
    // 0x21F14E0, has no torn-off check), which the server closes again at once (the pawn is torn off), and the client
    // tears it off again: the drone once more (CoreV2 #254, #263: 3 or 4 drones for one death). Those RPCs are the
    // gameplay cues of the bot's ability system, whose avatar the body still is: its DBNO ability's bleed
    // (GE_Athena_DBNO_Bleed, every Default.DBNO.BleedTick s) on a bot killed while down, and hits on the body. A player's
    // death leaves no such ability running on the body; the bot's is ended here (the ability removes its bleed as it
    // ends), the bleed taken off with it, and the body takes no more hits (damage off, as the warmup island's pawns have
    // it); its movement stops.
    if (Pawn)
    {
        Pawn->bCanBeDamaged = false;
        Pawn->ForceNetUpdate();
        UAbilitySystemComponent* AbilitySystem = static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent);
        const FName DBNOTag(L"Gameplay.Action.Player.DBNOAthena", FNAME_Find);
        if (AbilitySystem && !DBNOTag.IsNone() && Natives::UAbilitySystemComponent_CancelAbilities)
        {
            FGameplayTagContainer Tags;
            Tags.GameplayTags.Add(FGameplayTag{ DBNOTag });
            Natives::UAbilitySystemComponent_CancelAbilities(AbilitySystem, &Tags, nullptr, nullptr);
        }
        static UClass* BleedEffect = LoadObject<UClass>(nullptr, L"/Game/Athena/MappedEffects/GE_Athena_DBNO_Bleed.GE_Athena_DBNO_Bleed_C");
        if (AbilitySystem && BleedEffect)
        {
            AbilitySystem->RemoveActiveGameplayEffectBySourceEffect(BleedEffect, nullptr, -1);
        }
    }
    if (AAIController* AIController = Controller.Get())
    {
        AIController->StopMovement();
    }
    bPathFollowing = false;
    bHasMoveGoal = false;
    if (PlayerState && AFortPlayerStateAthena::HasEliminator())
    {
        FDeathInfo& DeathInfo = PlayerState->DeathInfo;
        DeathInfo.FinisherOrDowner = KillerState ? KillerState : PlayerState;
        DeathInfo.bDBNO = false;
        if (Pawn && AFortPlayerStateAthena::HasToDeathCause())
        {
            DeathInfo.DeathCause = AFortPlayerStateAthena::ToDeathCause(Pawn, Pawn->DeathTags, false);
        }
        if (FDeathInfo::HasDistance())
        {
            AFortPlayerPawn* KillerPawn = KillerState && KillerState != PlayerState ? KillerState->GetCurrentPawn() : nullptr;
            DeathInfo.Distance = KillerPawn && Pawn ? KillerPawn->GetDistanceTo(Pawn) : 0.0f;
        }
        if (FDeathInfo::HasDeathLocation() && Pawn)
        {
            DeathInfo.DeathLocation = Pawn->GetTransform().GetLocation();
        }
        // A death stored as the game stores a player's (4.5 0xAE1C40): without it the player state isn't dead to the
        // clients (IsDead), so a bot teammate's squad bar entry kept its name and an empty health bar.
        if (FDeathInfo::HasInitialized())
        {
            DeathInfo.bInitialized = true;
        }
        PlayerState->ForceNetUpdate();
    }
    DropInventory();
    State = ECoreAthenaBotState::Dead;
    CurrentTarget = nullptr;
    LootTarget = nullptr;
    Log::Info(L"Battle Royale bots: {} died at ({:.0f}, {:.0f}, {:.0f}){}", BotName, LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z,
        KillerState ? std::format(L", eliminated by {}", KillerState->GetPlayerName().ToStdString()) : std::wstring(L" (nobody's elimination)"));
    // A playlist that brings its players back (Playground, CoreV2 #344): the bot stays in the match and doesn't spectate,
    // as a respawning player doesn't (Core's AFortPlayerControllerAthena death handler asks the same).
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    if (World && PlayerState && GameMode->IsRespawningAllowed(PlayerState))
    {
        RespawnTime = World->GetTimeSeconds() + RespawnDelay;
        Log::Info(L"Battle Royale bots: {} respawns in {:.0f} s, the playlist brings its players back", BotName, RespawnDelay);
        return;
    }
    // Out of the match, it spectates the player who eliminated it, as an eliminated player does (CoreV2 #251). One
    // eliminated by a bot, the storm, a fall or anything else just leaves the match (CoreV2 #255).
    if (World && Manager->FindPlayerToSpectate(this, KillerState))
    {
        OnBeginSpectating(KillerState, World->GetTimeSeconds());
    }
#endif
}

void FCoreAthenaBotController::RespawnPlayerAfterDeath(float Now)
{
#if WITH_SERVER_CODE
    // As AFortPlayerControllerAthena::RespawnPlayerAfterDeath brings a player back on such a playlist (CoreV2 #344): a new
    // pawn once the countdown is over, in the safe zones phase (PlayerCanRestart), skydiving from RespawnHeight over where
    // the old one died, with nothing but the pickaxe (its loot dropped where it fell).
    if (Now < RespawnTime)
    {
        return;
    }
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GamePhase > EAthenaGamePhase::GetSafeZones())
    {
        // The match is over: the bot stays dead.
        RespawnTime = -1.0f;
        return;
    }
    if (GameState->GamePhase != EAthenaGamePhase::GetSafeZones())
    {
        RespawnTime = Now + 1.0f;
        return;
    }
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    FVector DeathLocation = LastKnownPosition;
    if (PlayerState && FDeathInfo::HasDeathLocation() && AFortPlayerStateAthena::HasEliminator())
    {
        const FVector Stored = PlayerState->DeathInfo.DeathLocation;
        if (Stored.X != 0.0f || Stored.Y != 0.0f || Stored.Z != 0.0f)
        {
            DeathLocation = Stored;
        }
    }
    const FVector Start(DeathLocation.X, DeathLocation.Y, DeathLocation.Z + AFortGameModeAthena::RespawnHeight);

    // The body goes, as the restarted player's old pawn does.
    if (AFortPlayerPawnAthena* OldPawn = GetPawn())
    {
        if (AAIController* AIController = Controller.Get())
        {
            AIController->UnPossess();
        }
        if (!OldPawn->IsPendingKill())
        {
            OldPawn->K2_DestroyActor();
        }
    }
    PlayerBotPawn = nullptr;
    ClearInventory();
    CurrentTarget = nullptr;
    LastAttacker = nullptr;
    Downer = nullptr;
    DBNOStartedAt = 0.0f;
    NextDeathCheck = 0.0f;
    LastHealthShield = -1.0f;
    LastHurtTime = -100.0f;
    bDiving = false;
    if (!Manager->FindGroundNear(DeathLocation, 3000.0f, DropTarget))
    {
        DropTarget = DeathLocation;
    }
    if (!SpawnPawnAt(Start, RandomRange(0.0f, 360.0f), true))
    {
        RespawnTime = Now + 1.0f;
        return;
    }
    RespawnTime = -1.0f;
    // Alive again to the clients (IsDead reads DeathInfo's bInitialized, which the death set).
    if (PlayerState && AFortPlayerStateAthena::HasEliminator() && FDeathInfo::HasInitialized())
    {
        PlayerState->DeathInfo.bInitialized = false;
        PlayerState->ForceNetUpdate();
    }
    State = ECoreAthenaBotState::Skydiving;
    LandedTime = 0.0f;
    NextLogTime = Now + 3.0f;
    LastKnownPosition = Start;
    Log::Info(L"Battle Royale bots: {} respawns in the air at ({:.0f}, {:.0f}, {:.0f})", BotName, Start.X, Start.Y, Start.Z);
#endif
}

void FCoreAthenaBotController::OnBeginSpectating(AFortPlayerStateAthena* KillerState, float Now)
{
#if WITH_SERVER_CODE
    // A player's SpectateOnDeath starts 5 seconds after the death and looks again every 2 (1.9's timer, Core's
    // SpectateOnDeathSoon); the bot does the same, for a while of its own.
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (!PlayerState || !AFortPlayerStateAthena::HasSpectators())
    {
        return;
    }
    SpectateEndTime = Now + RandomRange(20.0f, 120.0f);
    NextSpectateCheckTime = Now + 5.0f;
    SpectatedPlayer = KillerState;
#endif
}

void FCoreAthenaBotController::TickSpectating(float Now)
{
#if WITH_SERVER_CODE
    if (!IsSpectating() || Now < NextSpectateCheckTime)
    {
        return;
    }
    NextSpectateCheckTime = Now + 2.0f;
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (!PlayerState || Now >= SpectateEndTime)
    {
        OnEndSpectating();
        return;
    }
    // The player it watched left (their player state is gone, its pointer stale): only its own side to clear, as the
    // game's EndSpectating would read the stale one.
    AFortPlayerStateAthena* Watched = SpectatedPlayer.Get();
    if (!Watched && static_cast<AFortPlayerStateAthena*>(PlayerState->SpectatingTarget))
    {
        PlayerState->SpectatingTarget = nullptr;
    }
    // Its eliminator while they're in the match; once they're out (or gone), it stops (CoreV2 #255: no other player).
    AFortPlayerStateAthena* Next = Manager->FindPlayerToSpectate(this, Watched);
    if (!Next)
    {
        OnEndSpectating();
        return;
    }
    if (Next == static_cast<AFortPlayerStateAthena*>(PlayerState->SpectatingTarget))
    {
        return;
    }
    PlayerState->BeginSpectating(Next);
    SpectatedPlayer = Next;
    Log::Info(L"Battle Royale bots: {} spectates {} ({} spectating, {:.0f} s left)", BotName, Next->GetPlayerName().ToStdString(),
        Next->Spectators.SpectatorArray.Num(), SpectateEndTime - Now);
#endif
}

void FCoreAthenaBotController::OnEndSpectating()
{
#if WITH_SERVER_CODE
    if (!IsSpectating())
    {
        return;
    }
    SpectateEndTime = -1.0f;
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (PlayerState && AFortPlayerStateAthena::HasSpectators())
    {
        if (!SpectatedPlayer.Get())
        {
            PlayerState->SpectatingTarget = nullptr;
        }
        PlayerState->EndSpectating();
        Log::Info(L"Battle Royale bots: {} stops spectating", BotName);
    }
    SpectatedPlayer = nullptr;
#endif
}
