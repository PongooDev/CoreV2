#include "pch.h"
#include "Athena/AI/FortAthenaAIBotController.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Abilities/GameplayAbility.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "Athena/AI/FortServerBotManagerAthena.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingSMActor.h"
#include "Building/BuildingWall.h"
#include "Configuration.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "GameplayTagContainer.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/DataTable.h"
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
#include <cmath>
#include <format>

namespace
{
    // EMovementMode: walking on the ground.
    constexpr uint8_t MoveWalking = 1;
    // EFortMovementStyle.
    constexpr uint8_t StyleRunning = 0;
    constexpr uint8_t StyleSprinting = 3;
    // EPathFollowingStatus::Idle.
    constexpr uint8_t PathIdle = 0;

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
            static UDataTable* PlayerTable = LoadObject<UDataTable>(nullptr, L"/Game/Characters/Player/DataTables/CharacterAttributesPlayer.CharacterAttributesPlayer");
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
                if (UAthenaCharacterItemDefinition* Character = LoadObject<UAthenaCharacterItemDefinition>(nullptr, Path))
                {
                    Characters.push_back(Character);
                }
            }
            Log::Info(L"Battle Royale bots: {} default outfits for the bots", Characters.size());
        }
        return Characters.empty() ? nullptr : Characters[static_cast<size_t>(FMath::Rand()) % Characters.size()];
    }

    UAthenaGliderItemDefinition* DefaultGlider()
    {
        static UAthenaGliderItemDefinition* Glider = LoadObject<UAthenaGliderItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Gliders/DefaultGlider.DefaultGlider");
        return Glider;
    }

    UAthenaPickaxeItemDefinition* DefaultPickaxe()
    {
        static UAthenaPickaxeItemDefinition* Pickaxe = LoadObject<UAthenaPickaxeItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Pickaxes/DefaultPickaxe.DefaultPickaxe");
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

    EFortAthenaAIBotWeaponType WeaponTypeOf(UFortWeaponItemDefinition* Definition, int32_t Pellets)
    {
        const std::wstring Name = Definition ? Definition->GetName().ToStdString() : std::wstring();
        const auto Has = [&Name](const wchar_t* Part) { return Name.find(Part) != std::wstring::npos; };
        if (Has(L"Shotgun") || Pellets > 1)
        {
            return EFortAthenaAIBotWeaponType::Shotgun;
        }
        if (Has(L"Sniper"))
        {
            return EFortAthenaAIBotWeaponType::Sniper;
        }
        if (Has(L"Assault"))
        {
            return EFortAthenaAIBotWeaponType::AssaultRifle;
        }
        if (Has(L"Pistol_AutoHeavy") || Has(L"Pistol_Scavenger") || Has(L"SMG"))
        {
            return EFortAthenaAIBotWeaponType::SMG;
        }
        if (Has(L"Pistol"))
        {
            return EFortAthenaAIBotWeaponType::Pistol;
        }
        if (Has(L"Launcher"))
        {
            return EFortAthenaAIBotWeaponType::Launcher;
        }
        return EFortAthenaAIBotWeaponType::Other;
    }

    const wchar_t* WeaponTypeName(EFortAthenaAIBotWeaponType Type)
    {
        switch (Type)
        {
        case EFortAthenaAIBotWeaponType::Pistol: return L"pistol";
        case EFortAthenaAIBotWeaponType::SMG: return L"SMG";
        case EFortAthenaAIBotWeaponType::AssaultRifle: return L"assault rifle";
        case EFortAthenaAIBotWeaponType::Shotgun: return L"shotgun";
        case EFortAthenaAIBotWeaponType::Sniper: return L"sniper";
        case EFortAthenaAIBotWeaponType::Launcher: return L"launcher";
        default: return L"gun";
        }
    }

    // Core's EquipMaxDistance per kind 🟡 (8.30's values are in its bot blueprints, which don't ship).
    float EquipMaxDistanceOf(EFortAthenaAIBotWeaponType Type)
    {
        switch (Type)
        {
        case EFortAthenaAIBotWeaponType::Shotgun: return 1200.0f;
        case EFortAthenaAIBotWeaponType::SMG: return 2500.0f;
        case EFortAthenaAIBotWeaponType::Pistol: return 3500.0f;
        case EFortAthenaAIBotWeaponType::Launcher: return 4000.0f;
        case EFortAthenaAIBotWeaponType::AssaultRifle: return 6000.0f;
        case EFortAthenaAIBotWeaponType::Sniper: return 100000.0f;
        default: return 3000.0f;
        }
    }

    // How much a bot wants a kind of gun at a range band (0 close, under 10 m; 1 mid, under 35 m; 2 far) 🟡.
    float RangePreference(EFortAthenaAIBotWeaponType Type, int32_t Band)
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
    float TypePriority(EFortAthenaAIBotWeaponType Type)
    {
        switch (Type)
        {
        case EFortAthenaAIBotWeaponType::AssaultRifle:
        case EFortAthenaAIBotWeaponType::Shotgun: return 5.0f;
        case EFortAthenaAIBotWeaponType::SMG: return 4.0f;
        case EFortAthenaAIBotWeaponType::Sniper: return 3.0f;
        case EFortAthenaAIBotWeaponType::Pistol: return 2.0f;
        default: return 1.0f;
        }
    }

    float DamagePerSecond(const FFortAthenaAIBotWeapon& Weapon)
    {
        return Weapon.Damage * static_cast<float>(Weapon.Pellets) * std::max(Weapon.FiringRate, 0.1f);
    }

    // A bot carries up to this many guns (a player's five quickbar slots, two kept for healing items).
    constexpr size_t MaxWeapons = 3;
    constexpr size_t MaxConsumableKinds = 2;

    void ReadWeaponStats(FFortAthenaAIBotWeapon& Weapon)
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

    // Who killed the pawn: the newest of its damagers, through the weapon to its owner, as the death report names the
    // killer (8.30's bots learn it from the pawn's death delegate, which Core can't bind).
    AFortPlayerStateAthena* KillerOf(AFortPawn* Pawn)
    {
        TStructArray<FDamagerInfo>& Damagers = Pawn->Damagers;
        for (int32_t Index = Damagers.Num() - 1; Index >= 0; --Index)
        {
            AActor* Causer = Damagers[Index].DamageCauser;
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

AFortAthenaAIBotController::AFortAthenaAIBotController(UFortServerBotManagerAthena* InManager, AAIController* InController, int32_t InBotIndex, uint8_t InTeam)
    : Manager(InManager)
    , Controller(InController)
    , BotIndex(InBotIndex)
    , Team(InTeam)
{
}

AAIController* AFortAthenaAIBotController::GetController() const
{
    return Controller.Get();
}

AFortPlayerPawnAthena* AFortAthenaAIBotController::GetPawn() const
{
    return PlayerBotPawn.Get();
}

AFortPlayerStateAthena* AFortAthenaAIBotController::GetPlayerState() const
{
    AAIController* AIController = Controller.Get();
    return AIController ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(AIController->PlayerState))) : nullptr;
}

void AFortAthenaAIBotController::AssignDisplayName(const std::wstring& Name)
{
    // 21.40's AFortAthenaAIBotController::AssignDisplayName. The player state is the game's own: it's a bot
    // (APlayerState::bIsABot) on its team, and its name shows in the kill feed and above its head.
    BotName = Name;
    AFortPlayerStateAthena* State = GetPlayerState();
    if (!State)
    {
        return;
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
    State->bIsABot = true;
    State->TeamIndex = EFortTeam{ Team };
    State->ForceNetUpdate();
}

void AFortAthenaAIBotController::InitializeSkills(int32_t Level)
{
    // 21.40's AFortAthenaAIBotController::InitializeSkills.
    AIBotSkills = UFortServerBotManagerAthena::SkillsForLevel(Level);
}

bool AFortAthenaAIBotController::SpawnPawnAt(const FVector& Location, float Yaw, bool bSkydive)
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
    if (Pawn->HasCustomizationLoadout())
    {
        FFortAthenaLoadout& Loadout = Pawn->CustomizationLoadout;
        UAthenaCharacterItemDefinition* Character = RandomCharacter();
        HeroObject = Character ? reinterpret_cast<UObject*>(static_cast<UFortHeroType*>(Character->HeroDefinition)) : nullptr;
        Loadout.Character = Character;
        if (FFortAthenaLoadout::HasGlider())
        {
            Loadout.Glider = DefaultGlider();
        }
        Loadout.Pickaxe = DefaultPickaxe();
        AFortPlayerStateAthena* State = GetPlayerState();
        UFortHeroType* HeroType = Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr;
        if (State && HeroType && AFortPlayerState::CanSetHeroType())
        {
            if (UFortHero* Hero = UFortCheatManager::CreateLookHero(HeroType))
            {
                State->SetHeroType(Hero);
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
    const int32_t MovementAttributes = InitializeMovementAttributes(Pawn, HeroObject);
    static bool bLoggedAttributes = false;
    if (!bLoggedAttributes)
    {
        bLoggedAttributes = true;
        UFortMovementSet* Set = Pawn->MovementSet;
        Log::Info(L"Battle Royale bots: {} movement attributes from the hero's stats row (walk {:.0f}, run {:.0f}, sprint {:.0f})", MovementAttributes,
            Set ? static_cast<float>(Set->WalkSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->RunSpeed.CurrentValue) : -1.0f, Set ? static_cast<float>(Set->SprintSpeed.CurrentValue) : -1.0f);
    }
    Pawn->ForceNetUpdate();

    // The harvesting tool first (Core-held, 8.30's weapon list keeps the bot's weapons on its controller).
    bPickaxeEquipped = false;
    EquippedIndex = -1;
    EquippedWeapon = nullptr;
    UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
    if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
    {
        EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
        bPickaxeEquipped = EquippedWeapon.Get() != nullptr;
    }
    if (!WeaponList.empty())
    {
        EquipWeapon(BestWeaponIndex());
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

void AFortAthenaAIBotController::ApplyStartingHealth()
{
    // The admin panel's starting health and shield (8.30 has no bot health field: its bot pawn class's default).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FFortAthenaBotSettings& Settings = Manager->GetSettings();
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

void AFortAthenaAIBotController::EnterAircraft(float InJumpTime, float InLatestJumpTime)
{
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
    WeaponList.clear();
    Ammo.clear();
    Consumables.clear();
    Materials[0] = Materials[1] = Materials[2] = 0;
    HarvestTarget = nullptr;
    bHasLandingSpot = false;
    State = EFortAthenaAIBotState::InAircraft;
    JumpTime = InJumpTime;
    LatestJumpTime = InLatestJumpTime;
}

void AFortAthenaAIBotController::Destroy()
{
    StopFiring();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        Pawn->K2_DestroyActor();
    }
    if (AAIController* AIController = Controller.Get())
    {
        AIController->K2_DestroyActor();
    }
    PlayerBotPawn = nullptr;
    State = EFortAthenaAIBotState::Dead;
}

void AFortAthenaAIBotController::AddKill()
{
    ++Kills;
    bDanceOnKill = State == EFortAthenaAIBotState::Landed && FMath::FRand() < 0.5f;
    if (AFortPlayerStateAthena* PlayerState = GetPlayerState())
    {
        PlayerState->SetKillScore(Kills);
    }
}

void AFortAthenaAIBotController::Tick(float Now, float DeltaSeconds)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    switch (State)
    {
    case EFortAthenaAIBotState::Dead:
        return;
    case EFortAthenaAIBotState::InAircraft:
        JumpOffBus(Now);
        return;
    case EFortAthenaAIBotState::Warmup:
    {
        // A bot killed on the warmup island comes back, as players do.
        const bool bGone = !Pawn || Pawn->IsPendingKill() || Pawn->bIsDying;
        if (bGone)
        {
            if (WarmupRespawnTime < 0.0f)
            {
                WarmupRespawnTime = Now + 3.0f;
            }
            else if (Now >= WarmupRespawnTime)
            {
                WarmupRespawnTime = -1.0f;
                FVector Spot;
                if (Manager->FindGroundNear(Pawn ? LocationOf(Pawn) : LastKnownPosition, 2000.0f, Spot))
                {
                    SpawnPawnAt(FVector(Spot.X, Spot.Y, Spot.Z + 100.0f), RandomRange(0.0f, 360.0f), false);
                }
            }
            return;
        }
        LastKnownPosition = LocationOf(Pawn);
        if (Now < EmoteUntil)
        {
            return;
        }
        if (NextEmoteTime <= 0.0f)
        {
            NextEmoteTime = Now + RandomRange(8.0f, 30.0f);
        }
        if (Now >= NextEmoteTime)
        {
            NextEmoteTime = Now + RandomRange(20.0f, 45.0f);
            if (PlayEmote(Now, RandomRange(4.0f, 8.0f)))
            {
                return;
            }
        }
        Wander(Now);
        return;
    }
    default:
        break;
    }

    // Skydiving or on the ground: the pawn's death takes the bot out of the match.
    bool bDead = !Pawn || Pawn->IsPendingKill() || Pawn->bIsDying;
    if (!bDead && Now >= NextDeathCheck)
    {
        NextDeathCheck = Now + 0.5f;
        bDead = Pawn->IsDead();
    }
    if (bDead)
    {
        AFortPlayerStateAthena* BotKiller = Pawn ? Manager->LastBotAttackerOf(Pawn) : nullptr;
        AFortPlayerStateAthena* Killer = Pawn ? KillerOf(Pawn) : nullptr;
        // Bled out while down: the one who knocked it is its eliminator.
        Manager->RemoveFromAliveBots(this, Killer ? Killer : BotKiller ? BotKiller : Downer.Get());
        return;
    }
    if (State == EFortAthenaAIBotState::Skydiving)
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
        LastHealthShield = HealthShield;
    }
    // Down (squads): crawling to a teammate, bleeding out, or eliminated with the team.
    if (Pawn->bIsDBNO)
    {
        Downed(Now);
        TakeStormDamage(Now);
        return;
    }
    if (Now >= NextPerceptionTime)
    {
        NextPerceptionTime = Now + 0.25f;
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
        EmoteUntil = 0.0f;
    }
    if (HarvestTarget.Get() && IsInCombat(Now))
    {
        StopHarvesting();
    }
    if (!HealingItem && !HarvestTarget.Get())
    {
        ManageWeapon(Now);
        CheckWeaponFireResult(Now);
    }
    const wchar_t* Intent = L"attack";
    // Mid-revive, the bot keeps at it unless an enemy is close or shooting at it.
    if (ReviveDoneAt > 0.0f && ReviveTarget && !IsUnderThreat(Now) && ReviveTeammate(Now))
    {
        StopFiring();
        Intent = L"revive";
    }
    else if (HealingItem || !RangeAttack(Now))
    {
        StopFiring();
        // Healing first when safe, then loot close by, then a chest or ammo box, then loot farther away (21.40's _Loot
        // weighs them by distance). In the storm it runs; only outside the next circle, it takes the loot right by it
        // first, and a bot without a gun arms itself before it heads for the circle (a landing spot far from the first
        // circle otherwise sent it off empty-handed).
        FVector StormCenter;
        float StormRadius = 0.0f;
        const bool bInStorm = Manager->GetCurrentSafeZone(StormCenter, StormRadius) && Dist2D(LastKnownPosition, StormCenter) > StormRadius * 0.97f;
        const bool bArmed = HasUsableWeapon();
        // A bot stuck against a tree, rock or wall breaks through it first (TryToUnstuck chose it).
        Intent = bHarvestToUnstick && Harvest(Now) ? L"harvest" : ReviveTeammate(Now) ? L"revive" : Heal(Now) ? L"heal" : bInStorm && Storm(Now) ? L"storm" : Loot(Now, 1500.0f) ? L"loot" : !bArmed && Loot(Now, 4000.0f) ? L"loot" : !bArmed && SearchContainer(Now) ? L"search"
            : Storm(Now) ? L"storm" : SearchContainer(Now) ? L"search" : StayWithTeam(Now) ? L"regroup" : Harvest(Now) ? L"harvest" : Loot(Now, 7000.0f) ? L"loot" : L"wander";
        if (HarvestTarget.Get() && std::wstring_view(Intent) != L"harvest")
        {
            StopHarvesting();
        }
        if (Intent[0] == L'w')
        {
            Wander(Now);
        }
    }
    TryToUnstuck(Now);
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
        Log::Info(L"Battle Royale bots: {} at ({:.0f}, {:.0f}, {:.0f}) {}s, speed {:.0f} (max {:.0f}), mode {}, style {}, {}: {:.0f} health, {:.0f} shield, {} weapons, holding {} ({} loaded), target {}{}, {} shots ({} the game's), {} searched, {} jumps, {} doors, {} reloads, {} weapon changes, {} heals ({} healing items), {} swings, {}/{}/{} materials, {} shots stopped by walls", BotName,
            LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z, Intent, Velocity.Size(), MaxSpeedOf(Movement), Movement ? static_cast<int32_t>(Movement->MovementMode) : -1, static_cast<int32_t>(Pawn->CurrentMovementStyle),
            bPathFollowing ? std::format(L"on a path ({} paths, {} failed)", PathRequests, PathFailures) : std::format(L"steering ({} paths, {} failed)", PathRequests, PathFailures), Pawn->GetHealth(), Pawn->GetShield(),
            WeaponList.size(), EquippedWeapon.Get() ? NameOf(static_cast<UFortWeaponItemDefinition*>(EquippedWeapon.Get()->WeaponData)) : std::wstring(L"nothing"),
            EquippedWeapon.Get() ? static_cast<int32_t>(EquippedWeapon.Get()->AmmoCount) : 0, Target ? NameOf(Target) : std::wstring(L"none"), bHasVisualLineOfSightOnTarget ? L" (in sight)" : L"", ShotsFired, NativeShots, ContainersSearched, Jumps, DoorsOpened, Reloads, WeaponSwitches, Heals, Consumables.size(), Swings, Materials[0], Materials[1], Materials[2], BlockedShots);
    }
#endif
}

void AFortAthenaAIBotController::JumpOffBus(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_JumpOffBus: at the bot's moment along the path (EnterAircraft picked it), or
    // when the bus is about to leave the drop zone, the bot leaves the aircraft where it is, skydiving from the bus as a
    // player does (ExitAircraft's AFortPlayerPawnAthena::BeginSkydiving).
    if (Now < JumpTime && Now < LatestJumpTime)
    {
        return;
    }
    AActor* Aircraft = Manager->GetAircraft();
    const FVector BusLocation = Aircraft ? LocationOf(Aircraft) : LastKnownPosition + FVector(0.0f, 0.0f, 15000.0f);
    const FVector Start(BusLocation.X + RandomRange(-300.0f, 300.0f), BusLocation.Y + RandomRange(-300.0f, 300.0f), BusLocation.Z - 500.0f);

    // Where to land: loot near the bus's path, or a spot on the ground below it (21.40's _FreeFalling picks a POI).
    const std::vector<FFortAthenaBotLoot>& LootList = Manager->GetLoot();
    std::vector<FVector> Near;
    // -CoreBotHotDrop (testing): everyone lands around the first bot's spot.
    static FVector HotDrop;
    static bool bHasHotDrop = false;
    const bool bHotDrop = Configuration::Get().bBotHotDrop && bHasHotDrop;
    for (const FFortAthenaBotLoot& Entry : LootList)
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
    State = EFortAthenaAIBotState::Skydiving;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    Log::Info(L"Battle Royale bots: {} jumps from the bus at ({:.0f}, {:.0f}, {:.0f}) towards ({:.0f}, {:.0f}) {:.0f} m away (skydiving {})", BotName, Start.X, Start.Y, Start.Z,
        DropTarget.X, DropTarget.Y, Dist2D(Start, DropTarget) / 100.0f, Pawn && Pawn->bIsSkydiving ? L"yes" : L"no");
    NextLogTime = Now + 3.0f;
    LandedTime = 0.0f;
#endif
}

void AFortAthenaAIBotController::FreeFalling(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling: steer towards the landing spot while skydiving and under the
    // parachute, until the pawn walks.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    LastKnownPosition = Location;
    const FVector Direction = Direction2D(Location, DropTarget);
    const float Distance = Dist2D(Location, DropTarget);
    if (Distance > 300.0f)
    {
        Pawn->AddMovementInput(Direction, 1.0f, true);
    }
    if (AAIController* AIController = Controller.Get(); AIController && Now >= NextShotTime)
    {
        NextShotTime = Now + 1.0f;
        AIController->SetFocalPoint(FVector(DropTarget.X, DropTarget.Y, Location.Z));
    }

    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    const bool bWalking = Movement && Movement->MovementMode == MoveWalking;
    if (Now >= NextLogTime)
    {
        NextLogTime = Now + 5.0f;
        Log::Info(L"Battle Royale bots: {} falls at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot (skydiving {}, parachute {}, movement {})", BotName, Location.X, Location.Y, Location.Z,
            Distance / 100.0f, Pawn->bIsSkydiving ? L"yes" : L"no", Pawn->bIsParachuteOpen ? L"open" : L"closed", Movement ? static_cast<int32_t>(Movement->MovementMode) : -1);
    }
    if (bWalking && !Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen)
    {
        if (LandedTime <= 0.0f)
        {
            LandedTime = Now;
        }
        if (Now - LandedTime > 0.5f)
        {
            State = EFortAthenaAIBotState::Landed;
            NextLogTime = Now + 5.0f;
            Log::Info(L"Battle Royale bots: {} lands at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot", BotName, Location.X, Location.Y, Location.Z, Distance / 100.0f);
        }
    }
    else
    {
        LandedTime = 0.0f;
    }
#endif
}

void AFortAthenaAIBotController::UpdateCurrentTarget(float Now)
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
    };
    std::vector<FCandidate> Candidates;
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (!Other || reinterpret_cast<AFortPlayerPawnAthena*>(Other) == Pawn || Other->bIsDying || Other->IsPendingKill() || Manager->TeamOfPawn(Other) == Team)
        {
            continue;
        }
        const float Distance = (LocationOf(Other) - Location).Size();
        if (Distance <= AIBotSkills.SightRange)
        {
            // A downed enemy is finished once nobody standing is closer (sorted 20 m farther).
            Candidates.push_back({ Other, Distance + (Other->bIsDBNO ? 2000.0f : 0.0f) });
        }
    }
    std::sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

    // Damage: the newest damager is an enemy pawn within twice the sight range.
    AFortPlayerStateAthena* Attacker = KillerOf(Pawn);
    AFortPawn* AttackerPawn = nullptr;
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (Attacker && Other && reinterpret_cast<UObject*>(static_cast<APlayerState*>(Other->PlayerState)) == reinterpret_cast<UObject*>(Attacker) && Manager->TeamOfPawn(Other) != Team)
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

    if (NewTarget != CurrentTarget.Get())
    {
        CurrentTarget = NewTarget;
        TargetAcquiredTime = Now;
        if (NewTarget)
        {
            Log::Info(L"Battle Royale bots: {} spots {} {:.0f} m away", BotName, NameOf(reinterpret_cast<AFortPlayerStateAthena*>(static_cast<APlayerState*>(NewTarget->PlayerState)) ?
                reinterpret_cast<UObject*>(static_cast<APlayerState*>(NewTarget->PlayerState)) : reinterpret_cast<UObject*>(NewTarget)), (LocationOf(NewTarget) - Location).Size() / 100.0f);
        }
    }
    bHasVisualLineOfSightOnTarget = bInSight;
    CurrentAlertLevel = NewTarget ? EAlertLevel::Threatened : Attacker ? EAlertLevel::Alerted : EAlertLevel::Unaware;
#endif
}

bool AFortAthenaAIBotController::HasAmmoFor(const FFortAthenaAIBotWeapon& Weapon) const
{
    const auto Found = Ammo.find(Weapon.AmmoDefinition);
    return Weapon.LoadedAmmo > 0 || (Found != Ammo.end() && Found->second > 0);
}

bool AFortAthenaAIBotController::HasUsableWeapon() const
{
    return std::any_of(WeaponList.begin(), WeaponList.end(), [this](const FFortAthenaAIBotWeapon& Weapon) { return HasAmmoFor(Weapon); });
}

int32_t AFortAthenaAIBotController::BestWeaponIndex() const
{
    // Out of a fight: the weapon for a mid-range fight, the likeliest next one.
    const int32_t Index = WeaponIndexForRange(2000.0f);
    return Index >= 0 ? Index : WeaponList.empty() ? -1 : 0;
}

int32_t AFortAthenaAIBotController::WeaponIndexForRange(float Distance) const
{
    const int32_t Band = Distance < 1000.0f ? 0 : Distance < 3500.0f ? 1 : 2;
    int32_t Best = -1;
    float BestScore = -1.0f;
    for (size_t Index = 0; Index < WeaponList.size(); ++Index)
    {
        const FFortAthenaAIBotWeapon& Weapon = WeaponList[Index];
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

int32_t AFortAthenaAIBotController::WeaponSlotFor(UFortWeaponItemDefinition* Definition) const
{
    // One gun of each kind, the better one kept (its damage a second from the stats row); with every slot full, a gun of
    // a more wanted kind (assault rifle and shotgun, then SMG, sniper, pistol) replaces the least wanted one.
    if (!Definition)
    {
        return -2;
    }
    const FFortAthenaAIBotWeapon& Found = GetWeaponInfo(Definition);
    for (size_t Index = 0; Index < WeaponList.size(); ++Index)
    {
        const FFortAthenaAIBotWeapon& Held = WeaponList[Index];
        if (Held.WeaponDefinition == Definition)
        {
            return -2;
        }
        if (Held.Type == Found.Type)
        {
            return DamagePerSecond(Found) > DamagePerSecond(Held) * 1.1f ? static_cast<int32_t>(Index) : -2;
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

const FFortAthenaAIBotWeapon& AFortAthenaAIBotController::GetWeaponInfo(UFortWeaponItemDefinition* Definition)
{
    static std::map<UFortWeaponItemDefinition*, FFortAthenaAIBotWeapon> Cache;
    auto [Found, bNew] = Cache.try_emplace(Definition);
    if (bNew && Definition)
    {
        FFortAthenaAIBotWeapon& Weapon = Found->second;
        Weapon.WeaponDefinition = Definition;
        Weapon.AmmoDefinition = static_cast<TAssetPtr<UFortWorldItemDefinition>&>(Definition->AmmoData).LoadSynchronous();
        ReadWeaponStats(Weapon);
        Log::Info(L"Battle Royale bots: {}: {} ({:.0f} damage x{}, {:.2f} shots/s, {} a magazine, {:.2f} s reload{}, equipped within {:.0f} m)", NameOf(Definition), WeaponTypeName(Weapon.Type),
            Weapon.Damage, Weapon.Pellets, Weapon.FiringRate, Weapon.ClipSize, Weapon.ReloadTime, Weapon.bReloadIndividualBullets ? L" by the shell" : L"", std::min(Weapon.EquipMaxDistance, 99999.0f) / 100.0f);
    }
    return Found->second;
}

const FFortAthenaAIBotConsumable* AFortAthenaAIBotController::GetConsumableInfo(UFortItemDefinition* Definition)
{
    // A consumable is a ranged weapon that is its own ammo (Athena_Bandage's AmmoData is Athena_Bandage); what it gives
    // and how long it takes are its fire ability's blueprint defaults (the game's ability can't run for a bot: it isn't
    // an inventory owner, the same refusal as its guns).
    static std::map<UFortItemDefinition*, std::unique_ptr<FFortAthenaAIBotConsumable>> Cache;
    auto [Found, bNew] = Cache.try_emplace(Definition);
    if (!bNew)
    {
        return Found->second.get();
    }
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
    auto Info = std::make_unique<FFortAthenaAIBotConsumable>();
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

bool AFortAthenaAIBotController::WantsPickup(UFortItemDefinition* Definition, bool bWeapon, bool bConsumable)
{
    if (bWeapon)
    {
        return WeaponSlotFor(Cast<UFortWeaponItemDefinition>(Definition)) != -2;
    }
    if (bConsumable)
    {
        const FFortAthenaAIBotConsumable* Info = GetConsumableInfo(Definition);
        const auto Held = Consumables.find(Definition);
        return Info && (Held != Consumables.end() ? Held->second < Info->MaxStackSize : Consumables.size() < MaxConsumableKinds);
    }
    const auto Reserve = Ammo.find(Definition);
    return std::any_of(WeaponList.begin(), WeaponList.end(), [Definition](const FFortAthenaAIBotWeapon& Weapon) { return Weapon.AmmoDefinition == Definition; })
        && (Reserve == Ammo.end() || Reserve->second < 120);
}

void AFortAthenaAIBotController::EquipWeapon(int32_t Index)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Index < 0 || Index >= static_cast<int32_t>(WeaponList.size()))
    {
        return;
    }
    StopFiring();
    CancelReload();
    FFortAthenaAIBotWeapon& Entry = WeaponList[static_cast<size_t>(Index)];
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
#endif
}

bool AFortAthenaAIBotController::IsUnderThreat(float Now) const
{
    AFortPawn* Target = CurrentTarget.Get();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    return Now - LastHurtTime < 2.0f || (Target && Pawn && bHasVisualLineOfSightOnTarget && (LocationOf(Target) - LocationOf(Pawn)).Size() < 2000.0f);
}

bool AFortAthenaAIBotController::IsInCombat(float Now) const
{
    return (CurrentTarget.Get() && bHasVisualLineOfSightOnTarget) || Now - LastHurtTime < 4.0f;
}

void AFortAthenaAIBotController::StartReload(float Now)
{
    if (EquippedIndex < 0 || EquippedIndex >= static_cast<int32_t>(WeaponList.size()))
    {
        return;
    }
    const FFortAthenaAIBotWeapon& Entry = WeaponList[static_cast<size_t>(EquippedIndex)];
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

void AFortAthenaAIBotController::CancelReload()
{
    if (ReloadIndex >= 0)
    {
        PlayReplicatedMontage(nullptr, 0.0f, true);
    }
    ReloadIndex = -1;
    ReloadUntil = 0.0f;
}

void AFortAthenaAIBotController::ManageWeapon(float Now)
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
    FFortAthenaAIBotWeapon& Entry = WeaponList[static_cast<size_t>(EquippedIndex)];
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
            const FFortAthenaAIBotWeapon& Next = WeaponList[static_cast<size_t>(ForRange)];
            // A loaded weapon that suits the range as well stays out: only a clearly better one is worth the swap.
            const int32_t Band = Distance < 1000.0f ? 0 : Distance < 3500.0f ? 1 : 2;
            if (Entry.LoadedAmmo == 0 || RangePreference(Next.Type, Band) > RangePreference(Entry.Type, Band) + 0.5f || Distance > Entry.EquipMaxDistance)
            {
                NextWeaponSwitch = Now + 1.5f;
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
            const FFortAthenaAIBotWeapon& Other = WeaponList[Index];
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

void AFortAthenaAIBotController::FireShot(AFortPawn* Target, float Distance)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortWeapon* Weapon = EquippedWeapon.Get();
    if (!Pawn || !Target)
    {
        return;
    }
    const FFortAthenaAIBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;

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

    // The hit: the shot's line, from the bot's eyes through its aim point (the target plus FAIBotSkills' error), passes
    // within the target's capsule, and nothing stands between them.
    const FVector Location = LocationOf(Pawn);
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    const FVector TargetLocation = LocationOf(Target);
    const FVector Center(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 20.0f);
    const FVector Aim = Center + AimError;
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
    if (Held && Held->Pellets > 1)
    {
        // A shotgun's pellets: all of them close up, fewer with the distance 🟡.
        Damage *= static_cast<float>(Held->Pellets) * std::clamp(1.0f - (Distance - 400.0f) / 2000.0f, 0.25f, 1.0f);
    }
    Manager->ApplyBotDamage(this, Target, Damage, Held ? Held->WeaponDefinition : nullptr);
#endif
}

void AFortAthenaAIBotController::StopFiring()
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

bool AFortAthenaAIBotController::ActivateWeaponFire(AFortWeapon* Weapon)
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

void AFortAthenaAIBotController::ReleaseWeaponFire()
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

void AFortAthenaAIBotController::PlayWeaponFireCue(AFortWeapon* Weapon, const FVector& ImpactLocation)
{
#if WITH_SERVER_CODE
    // The game's fire ability can't run for a bot: its cost check wants an inventory owner ("has a durability cost but
    // isn't being activated by an inventory owner"), which 8.30's AFortAthenaAIBotController is (it owns an
    // AFortInventory) and a stock AAIController isn't. So Core fires the shot and sends what the ability would show: its
    // WeaponFireCue, executed on every client through the pawn's multicast, as GA_Ranged_GenericDamage does.
    static UFunction* Multicast = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawnAthena:NetMulticast_InvokeGameplayCueExecuted_WithParams");
    static UScriptStruct* CueParametersStruct = FindObject<UScriptStruct>(nullptr, L"/Script/GameplayAbilities.GameplayCueParameters");
    static std::map<const UObject*, FName> CueTags;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    FGameplayAbilitySpec* Spec = AbilitySystem && AFortWeapon::HasPrimaryAbilitySpecHandle() ? AbilitySystem->FindAbilitySpecFromHandle(Weapon->PrimaryAbilitySpecHandle) : nullptr;
    const UObject* Ability = Spec ? reinterpret_cast<const UObject*>(static_cast<UGameplayAbility*>(Spec->Ability)) : nullptr;
    if (!Multicast || !CueParametersStruct || !Ability)
    {
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

    Reflection::FParams Params(Multicast, true);
    Params.Set(L"GameplayCueTag", FGameplayTag{ Found->second });
    uint8_t* CueParameters = static_cast<uint8_t*>(Params.GetAddress(L"GameplayCueParameters"));
    if (!CueParameters)
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
    Params.Call(reinterpret_cast<UObject*>(Pawn));
#endif
}

void AFortAthenaAIBotController::CheckWeaponFireResult(float Now)
{
    // The first shot of a weapon class shows whether the game's fire works for a bot: its magazine went down.
    if (WeaponFireCheckTime <= 0.0f || Now < WeaponFireCheckTime)
    {
        return;
    }
    WeaponFireCheckTime = 0.0f;
    AFortWeapon* Weapon = EquippedWeapon.Get();
    const FFortAthenaAIBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
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

bool AFortAthenaAIBotController::RangeAttack(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's _RangeAttack (a ranged weapon at a target in sight; the destination is the target) and 21.40's
    // _MeleeAttack (the harvesting tool, close up). Aim: FAIBotSkills' reaction time before the first shot, an error that
    // grows with the distance, and shots spaced by MaxTimeBetweenShots +- DeviationTimeBetweenShots.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPawn* Target = CurrentTarget.Get();
    AAIController* AIController = Controller.Get();
    if (!Target || !AIController || Target->bIsDying || !bHasVisualLineOfSightOnTarget)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    const FVector TargetLocation = LocationOf(Target);
    const float Distance = (TargetLocation - Location).Size();
    const bool bArmed = HasUsableWeapon();
    if (!bArmed && Distance > 400.0f)
    {
        return false;
    }
    // Unarmed with a weapon to grab nearby, a player goes for the weapon unless the enemy is on top of it or hitting
    // it (bots that landed together otherwise all brawled with their pickaxes).
    if (!bArmed && Distance > 200.0f && !Manager->LastBotAttackerOf(reinterpret_cast<AFortPawn*>(Pawn)) && LootTarget.Get() && Dist2D(LocationOf(LootTarget.Get()), Location) < 2000.0f)
    {
        return false;
    }

    // The aim, refreshed every shot.
    const float ErrorScale = AIBotSkills.AimErrorAt10m * std::max(Distance, 300.0f) / 1000.0f;
    if (Now >= NextShotTime || AimError.SizeSquared() == 0.0f)
    {
        AimError = FVector(RandomRange(-1.0f, 1.0f), RandomRange(-1.0f, 1.0f), RandomRange(-0.6f, 0.6f)) * ErrorScale;
    }
    AIController->SetFocalPoint(FVector(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 20.0f) + AimError);

    // Keep a fighting distance: close in from afar, back off when too close (a shotgun or the pickaxe wants it close).
    if (!bArmed)
    {
        if (!bPickaxeEquipped)
        {
            UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
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
        if (Now >= NextStrafeChange)
        {
            NextStrafeChange = Now + RandomRange(0.6f, 1.6f);
            StrafeSign = FMath::FRand() < 0.7f ? -StrafeSign : StrafeSign;
        }
        const FVector Toward = Direction2D(Location, TargetLocation);
        SetMovementStyle(false);
        Pawn->AddMovementInput(FVector(-Toward.Y * StrafeSign, Toward.X * StrafeSign, 0.0f), 0.8f, false);
    }

    if (bFiring && Now >= FireUntil)
    {
        StopFiring();
    }
    const bool bReady = Now >= TargetAcquiredTime + AIBotSkills.MaxTrackingReactionTime && Now >= NextShotTime && ReloadUntil <= 0.0f;
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
        const FFortAthenaAIBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
        const float WeaponInterval = bPickaxeEquipped ? 0.8f : Held && Held->FiringRate > 0.0f ? 1.0f / Held->FiringRate : 0.5f;
        NextShotTime = Now + std::max(WeaponInterval, AIBotSkills.MaxTimeBetweenShots + RandomRange(-AIBotSkills.DeviationTimeBetweenShots, AIBotSkills.DeviationTimeBetweenShots));
    }
    return true;
#else
    return false;
#endif
}

bool AFortAthenaAIBotController::Loot(float Now, float MaxDistance)
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
    if (!Target && Now >= NextLootSearch)
    {
        NextLootSearch = Now + 1.0f;
        float Best = 7000.0f;
        for (const FFortAthenaBotLoot& Entry : Manager->GetLoot())
        {
            AFortPickup* Pickup = Entry.Pickup.Get();
            if (!Pickup || Pickup->bPickedUp || IsUnreachable(Pickup))
            {
                continue;
            }
            FFortItemEntry& PickupEntry = Pickup->PrimaryPickupItemEntry;
            const bool bWanted = WantsPickup(PickupEntry.ItemDefinition, Entry.bWeapon, Entry.bConsumable);
            const float Distance = (Entry.Location - Location).Size();
            if (bWanted && Distance < Best && std::fabs(Entry.Location.Z - Location.Z) < 800.0f)
            {
                Best = Distance;
                Target = Pickup;
            }
        }
        if (Target)
        {
            LootTargetTime = Now;
            LastLootPursuit = Now;
        }
        LootTarget = Target;
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
    if (Now - LootTargetTime > 20.0f)
    {
        MarkUnreachable(Target);
        LootTarget = nullptr;
        return false;
    }
    if (Dist2D(PickupLocation, Location) < 200.0f && std::fabs(PickupLocation.Z - Location.Z) < 250.0f)
    {
        TakePickup(Target);
        LootTarget = nullptr;
        return true;
    }
    // Right above or below it (another floor, a roof): out of reach for a bot without a path.
    if (Dist2D(PickupLocation, Location) < 120.0f)
    {
        MarkUnreachable(Target);
        LootTarget = nullptr;
        return false;
    }
    MoveTowards(PickupLocation, Now, 50.0f);
    return true;
#else
    return false;
#endif
}

bool AFortAthenaAIBotController::PlayEmote(float Now, float Duration)
{
#if WITH_SERVER_CODE
    static std::vector<UFortMontageItemDefinitionBase*> EmoteAssets;
    static bool bLoaded = false;
    if (!bLoaded)
    {
        bLoaded = true;
        for (const wchar_t* Path : { L"/Game/Athena/Items/Cosmetics/Dances/EID_DanceMoves.EID_DanceMoves", L"/Game/Athena/Items/Cosmetics/Dances/EID_Floss.EID_Floss",
                 L"/Game/Athena/Items/Cosmetics/Dances/EID_Fresh.EID_Fresh", L"/Game/Athena/Items/Cosmetics/Dances/EID_BreakDance.EID_BreakDance" })
        {
            if (UFortMontageItemDefinitionBase* Emote = LoadObject<UFortMontageItemDefinitionBase>(nullptr, Path))
            {
                EmoteAssets.push_back(Emote);
            }
        }
    }
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = GameData ? GameData->EmoteGameplayAbility.LoadSynchronous() : nullptr;
    UGameplayAbility* Ability = AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()) ? static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject()) : nullptr;
    if (!AbilitySystem || !Ability || EmoteAssets.empty())
    {
        return false;
    }
    UFortMontageItemDefinitionBase* Emote = EmoteAssets[static_cast<size_t>(FMath::Rand()) % EmoteAssets.size()];
    Emote->Animation.LoadSynchronous();
    StopMoving();
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
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::TakePickup(AFortPickup* Pickup)
{
#if WITH_SERVER_CODE
    // Into the bot's own inventory (8.30 keeps it in the controller's AFortInventory; Core's brain holds it), and the
    // pickup is gone as when a player takes it.
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    UFortItemDefinition* Definition = Entry.ItemDefinition;
    const int32_t Count = std::max(static_cast<int32_t>(Entry.Count), 1);
    if (const FFortAthenaAIBotConsumable* Consumable = GetConsumableInfo(Definition))
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
        FFortAthenaAIBotWeapon Weapon = GetWeaponInfo(WeaponDefinition);
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
            const FFortAthenaAIBotWeapon& Replaced = WeaponList[static_cast<size_t>(Slot)];
            Log::Info(L"Battle Royale bots: {} swaps its {} for {}", BotName, NameOf(Replaced.WeaponDefinition), NameOf(WeaponDefinition));
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

bool AFortAthenaAIBotController::Storm(float Now)
{
    // 8.30's _Storm: the safe zone's phase and state feed the behaviour tree, which heads for the next circle.
    FVector Center;
    float Radius = 0.0f;
    if (!Manager->GetSafeZone(Center, Radius))
    {
        return false;
    }
    const FVector Location = LocationOf(GetPawn());
    if (Dist2D(Location, Center) <= Radius * 0.95f)
    {
        return false;
    }
    // A point well inside the circle on the bot's side of it.
    const FVector Inward = Direction2D(Center, Location);
    const FVector Goal(Center.X + Inward.X * Radius * 0.4f, Center.Y + Inward.Y * Radius * 0.4f, Location.Z);
    MoveTowards(Goal, Now, 300.0f);
    return true;
}

void AFortAthenaAIBotController::Wander(float Now)
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    if (!bHasWanderGoal || Dist2D(Location, WanderGoal) < 200.0f || Now >= NextWanderTime)
    {
        const float Range = State == EFortAthenaAIBotState::Warmup ? 800.0f : 2500.0f;
        FVector Center;
        float Radius = 0.0f;
        FVector Around = Location;
        if (State != EFortAthenaAIBotState::Warmup && Manager->GetSafeZone(Center, Radius) && Dist2D(Location, Center) > Radius * 0.5f)
        {
            Around = FVector(Location.X + (Center.X - Location.X) * 0.3f, Location.Y + (Center.Y - Location.Y) * 0.3f, Location.Z);
        }
        const float Angle = RandomRange(0.0f, 6.2831853f);
        const float Distance = RandomRange(Range * 0.3f, Range);
        WanderGoal = FVector(Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, Location.Z);
        bHasWanderGoal = true;
        NextWanderTime = Now + RandomRange(5.0f, 9.0f);
    }
    MoveTowards(WanderGoal, Now, 150.0f);
}

void AFortAthenaAIBotController::MoveTowards(const FVector& Destination, float Now, float AcceptanceRadius)
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
    // Sprinting to get there, running while it fights.
    SetMovementStyle(!bFiring && !(CurrentTarget.Get() && bHasVisualLineOfSightOnTarget));
    if (FollowPath(Destination, Now, AcceptanceRadius))
    {
        bHasMoveGoal = true;
        MoveGoal = Destination;
        return;
    }
    if (!bHasMoveGoal || Dist2D(MoveGoal, Destination) > 150.0f)
    {
        MoveGoal = Destination;
        bHasMoveGoal = true;
        if (!bFiring && !CurrentTarget.Get())
        {
            AIController->SetFocalPoint(FVector(Destination.X, Destination.Y, Location.Z + 60.0f));
        }
    }
    // Facing the way it goes: Battle Royale's pawn moves slower sideways and backwards (its directional speed curve).
    if (!bFiring && !CurrentTarget.Get() && Now >= NextFaceTime)
    {
        NextFaceTime = Now + 0.25f;
        const FVector Direction = Direction2D(Location, Destination);
        AIController->SetFocalPoint(FVector(Location.X + Direction.X * 1000.0f, Location.Y + Direction.Y * 1000.0f, Location.Z + 60.0f));
    }
    FVector Direction = Direction2D(Location, Destination);
    SteerAroundObstacles(Direction, Now);
    Pawn->AddMovementInput(Direction, 1.0f, false);
#endif
}

void AFortAthenaAIBotController::StopMoving()
{
    bHasMoveGoal = false;
    if (bPathFollowing)
    {
        bPathFollowing = false;
        if (AAIController* AIController = Controller.Get())
        {
            AIController->StopMovement();
        }
    }
}

bool AFortAthenaAIBotController::FollowPath(const FVector& Destination, float Now, float AcceptanceRadius)
{
#if WITH_SERVER_CODE
    // 21.40's _Movement with a navmesh: AAIController::MoveToLocation with pathfinding (a partial path where the tiles
    // around the bot end), followed by the engine's path following; when the bot is off the navmesh (its tiles not built
    // yet) or no path comes back, it steers for a while (21.40's OnGoalOffNavMesh).
    AAIController* AIController = Controller.Get();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Manager->HasNavigation() || State != EFortAthenaAIBotState::Landed || Now < PathFailedUntil || !AIController || !Pawn)
    {
        if (bPathFollowing)
        {
            StopMoving();
        }
        return false;
    }
    if (!bPathFollowing || Dist2D(PathGoal, Destination) > 250.0f || Now >= NextPathRequest)
    {
        UWorld* World = Manager->GetGameMode()->GetWorld();
        if (!UAthenaNavSystem::IsOnNavMesh(World, LocationOf(Pawn)))
        {
            PathFailedUntil = Now + 2.0f;
            bPathFollowing = false;
            return false;
        }
        const EPathFollowingRequestResult::Type Result = AIController->MoveToLocation(Destination, AcceptanceRadius, false, true, true, false, true);
        ++PathRequests;
        NextPathRequest = Now + 4.0f;
        if (Result == EPathFollowingRequestResult::Failed)
        {
            ++PathFailures;
            PathFailedUntil = Now + 3.0f;
            bPathFollowing = false;
            return false;
        }
        if (PathRequests == 1)
        {
            Log::Info(L"Battle Royale bots: {} follows its first path, to ({:.0f}, {:.0f}, {:.0f}) {:.0f} m away", BotName, Destination.X, Destination.Y, Destination.Z, Dist2D(LocationOf(Pawn), Destination) / 100.0f);
        }
        bPathFollowing = Result == EPathFollowingRequestResult::RequestSuccessful;
        PathGoal = Destination;
        // The path following turns the pawn along its path (the Move focus) unless the fight's focus outranks it.
        if (bPathFollowing && !bFiring && !CurrentTarget.Get())
        {
            AIController->ClearFocus();
        }
        return true;
    }
    // The path ended short of the goal (a partial one, or it was blocked): steer, then ask again.
    if (AIController->GetMoveStatus() == PathIdle)
    {
        bPathFollowing = false;
        PathFailedUntil = Now + 1.5f;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::SteerAroundObstacles(FVector& Direction, float Now)
{
#if WITH_SERVER_CODE
    // Without a path: a trace ahead at knee height. Something low (the head-height trace is clear) is jumped; a wall or
    // a rock turns the bot to the nearest clear heading for a moment.
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
    UObject* Obstacle = nullptr;
    if (!IsBlocked(Pawn, Knee, Knee + Direction * 130.0f, &Obstacle))
    {
        return;
    }
    // A closed door: opened as a player opens it (the floor loot of 3.0's houses is behind them).
    if (ABuildingWall* Wall = Obstacle ? Cast<ABuildingWall>(Obstacle) : nullptr; Wall && Wall->IsDoor() && !Wall->bDoorCollisionDisabled)
    {
        Wall->OpenDoor();
        ++DoorsOpened;
        return;
    }
    static int32_t LoggedObstacles = 0;
    if (Obstacle && LoggedObstacles++ < 12)
    {
        Log::Info(L"Battle Royale bots: {} is held up by {} ({})", BotName, NameOf(Obstacle), NameOf(Obstacle->GetClass()));
    }
    if (!IsBlocked(Pawn, Head, Head + Direction * 130.0f))
    {
        if (Now >= NextJumpTime)
        {
            NextJumpTime = Now + 1.2f;
            static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->Jump();
            ++Jumps;
        }
        return;
    }
    for (const float Angle : { 40.0f, -40.0f, 75.0f, -75.0f, 110.0f, -110.0f })
    {
        const FVector Turned = RotateYaw(Direction, Angle);
        if (!IsBlocked(Pawn, Knee, Knee + Turned * 220.0f) && !IsBlocked(Pawn, Head, Head + Turned * 220.0f))
        {
            AvoidDirection = Turned;
            AvoidUntil = Now + 0.7f;
            Direction = Turned;
            return;
        }
    }
    AvoidDirection = Direction * -1.0f;
    AvoidUntil = Now + 1.0f;
    Direction = AvoidDirection;
#endif
}

void AFortAthenaAIBotController::SetMovementStyle(bool bSprint)
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const uint8_t Style = bSprint ? StyleSprinting : StyleRunning;
    if (!Pawn || MovementStyle == Style)
    {
        return;
    }
    MovementStyle = Style;
    Pawn->CurrentMovementStyle = Style;
}

void AFortAthenaAIBotController::LogMovementDiagnostics()
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

bool AFortAthenaAIBotController::IsUnreachable(const void* Object) const
{
    return Object && std::find(Unreachable.begin(), Unreachable.end(), Object) != Unreachable.end();
}

void AFortAthenaAIBotController::MarkUnreachable(const void* Object)
{
    if (Object && !IsUnreachable(Object))
    {
        Unreachable.push_back(Object);
    }
}

bool AFortAthenaAIBotController::SearchContainer(float Now)
{
#if WITH_SERVER_CODE
    // Containers are worth it while the bot wants weapons or ammo for its guns.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    bool bWants = WeaponList.size() < 2 || Consumables.empty();
    for (const FFortAthenaAIBotWeapon& Weapon : WeaponList)
    {
        const auto Found = Ammo.find(Weapon.AmmoDefinition);
        bWants = bWants || Found == Ammo.end() || Found->second < 60;
    }
    ABuildingContainer* Target = ContainerTarget.Get();
    if (Target && (Target->IsPendingKill() || Target->bDestroyed || Target->IsAlreadySearched(reinterpret_cast<AFortPawn*>(Pawn))))
    {
        Manager->OnContainerSearched(Target);
        Target = nullptr;
        ContainerTarget = nullptr;
        SearchUntil = 0.0f;
    }
    if (!Pawn || (!bWants && !Target))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    if (!Target && Now >= NextContainerSearch)
    {
        NextContainerSearch = Now + 2.0f;
        float Best = 5000.0f;
        for (const FFortAthenaBotContainer& Entry : Manager->GetContainers())
        {
            ABuildingContainer* Container = Entry.Container.Get();
            const float Distance = Dist2D(Entry.Location, Location) * (Entry.bChest ? 0.8f : 1.0f);
            if (Container && !IsUnreachable(Container) && Distance < Best && std::fabs(Entry.Location.Z - Location.Z) < 800.0f)
            {
                Best = Distance;
                Target = Container;
            }
        }
        if (Target)
        {
            ContainerTargetTime = Now;
        }
        ContainerTarget = Target;
    }
    if (!Target)
    {
        return false;
    }
    if (Now - ContainerTargetTime > 25.0f && SearchUntil <= 0.0f)
    {
        MarkUnreachable(Target);
        ContainerTarget = nullptr;
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
            if (AAIController* AIController = Controller.Get())
            {
                AIController->SetFocalPoint(ContainerLocation);
            }
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
        MarkUnreachable(Target);
        ContainerTarget = nullptr;
        return false;
    }
    MoveTowards(ContainerLocation, Now, 100.0f);
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::EquipPickaxe()
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
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

bool AFortAthenaAIBotController::SwingAtBuilding(ABuildingSMActor* Building, float& OutDamage)
{
    OutDamage = 0.0f;
    AFortWeapon* Pickaxe = bPickaxeEquipped ? EquippedWeapon.Get() : nullptr;
    return Pickaxe && DamageBuilding(Building, Pickaxe, OutDamage);
}

bool AFortAthenaAIBotController::DamageBuilding(ABuildingSMActor* Building, AFortWeapon* Pickaxe, float& OutDamage)
{
#if WITH_SERVER_CODE
    // The pickaxe's hit as its ability applies it (GAB_Melee_ImpactCombo_Athena's GE_Melee_GenericDamage), made on the
    // bot's ability system and applied to the building's. Its execution, UFortDamageFormulaExecutionCalculation (3.1
    // 0xC49090), takes the base damage from the context's damage source: FFortGameplayEffectContext::DamageSourceObject
    // (an IFortDamageSourceInterface; AFortWeapon's reads its WeaponData's stats row, so the pickaxe's FFortMeleeWeaponStats
    // give a building its environment damage). A context with no damage source deals 0, which is what phase 4 saw; the
    // ability fills it with the weapon, and so does Core here, with the bot's pickaxe.
    static UClass* DamageEffect = LoadObject<UClass>(nullptr, L"/Game/Abilities/Weapons/Melee/GE_Melee_GenericDamage.GE_Melee_GenericDamage_C");
    static UFunction* MakeContext = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:MakeEffectContext");
    static UFunction* MakeSpec = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:MakeOutgoingSpec");
    static UFunction* ApplyToTarget = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemComponent:BP_ApplyGameplayEffectSpecToTarget");
    static UScriptStruct* FortContext = FindObject<UScriptStruct>(nullptr, L"/Script/FortniteGame.FortGameplayEffectContext");
    static UProperty* DamageSourceProperty = FortContext ? FortContext->FindPropertyByName(FName(L"DamageSourceObject", FNAME_Find)) : nullptr;
    OutDamage = 0.0f;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    UAbilitySystemComponent* Source = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    UAbilitySystemComponent* Target = Building->AbilitySystemComponent;
    if (!Target)
    {
        Target = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(reinterpret_cast<AActor*>(Building));
    }
    static int32_t Logged = 0;
    if (!DamageEffect || !MakeContext || !MakeSpec || !ApplyToTarget || !DamageSourceProperty || !Pickaxe || !Source || !Target)
    {
        if (Logged++ < 3)
        {
            Log::Info(L"Battle Royale bots: {} can't hit {}: effect {}, functions {}, damage source {}, pickaxe {}, the bot's ability system {}, the building's {}", BotName, NameOf(reinterpret_cast<UObject*>(Building)),
                NameOf(DamageEffect), MakeContext && MakeSpec && ApplyToTarget ? L"found" : L"missing", DamageSourceProperty ? L"found" : L"missing", NameOf(reinterpret_cast<UObject*>(Pickaxe)),
                NameOf(reinterpret_cast<UObject*>(Source)), NameOf(reinterpret_cast<UObject*>(Target)));
        }
        return false;
    }
    const float Before = Building->GetHealth();
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
    *reinterpret_cast<FWeakObjectPtr*>(ContextData + DamageSourceProperty->GetOffset_ForInternal()) = reinterpret_cast<UObject*>(Pickaxe);
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
    if (!Apply.CopyStruct(L"SpecHandle", Handle) || !Apply.Set(L"Target", Target) || !Apply.Call(reinterpret_cast<UObject*>(Source)))
    {
        return false;
    }
    OutDamage = Before - Building->GetHealth();
    if (Logged++ < 6)
    {
        Log::Info(L"Battle Royale bots: {} hits {} with {}: health {:.0f} -> {:.0f}", BotName, NameOf(reinterpret_cast<UObject*>(Building)), NameOf(static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponData)), Before, Building->GetHealth());
    }
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::StopHarvesting()
{
    HarvestTarget = nullptr;
    bHarvestToUnstick = false;
    if (bPickaxeEquipped && HasUsableWeapon())
    {
        EquipWeapon(BestWeaponIndex());
    }
}

void AFortAthenaAIBotController::OnResourcesHarvested(ABuildingSMActor* Building, UFortItemDefinition* ResourceItem, int32_t Count, bool bDestroyed)
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

bool AFortAthenaAIBotController::FindBuildingInTheWay(float Now)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || !bHasMoveGoal || IsInCombat(Now))
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    FVector Direction = MoveGoal - Location;
    Direction.Z = 0.0f;
    if (Direction.Size() < 1.0f)
    {
        return false;
    }
    Direction = Direction * (1.0f / Direction.Size());
    // Ahead, and a little to each side, at knee and chest height.
    for (const float Yaw : { 0.0f, -25.0f, 25.0f })
    {
        for (const float Height : { -30.0f, 30.0f })
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
    return false;
#else
    return false;
#endif
}

bool AFortAthenaAIBotController::Harvest(float Now)
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
    if (AAIController* AIController = Controller.Get())
    {
        AIController->SetFocalPoint(HarvestPoint);
    }
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

bool AFortAthenaAIBotController::PlayReplicatedMontage(UObject* Montage, float PlayRate, bool bStop)
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

bool AFortAthenaAIBotController::IsDBNO() const
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    return Pawn && Pawn->bIsDBNO;
}

bool AFortAthenaAIBotController::CanGoDBNO() const
{
    // Battle Royale's DBNO (GAB_AthenaDBNO): in a team playlist a player whose teammate is still up goes down instead of
    // out; a team all down is eliminated.
    if (Manager->GetTeamSize() <= 1 || !IsAlive() || IsDBNO())
    {
        return false;
    }
    for (AFortAthenaAIBotController* Teammate : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() == EFortAthenaAIBotState::Landed && !Teammate->IsDBNO())
        {
            return true;
        }
    }
    return false;
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

void AFortAthenaAIBotController::EnterDBNO(AFortPlayerStateAthena* InDowner, float Now)
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
    StopFiring();
    CancelReload();
    CancelHeal();
    if (HarvestTarget.Get())
    {
        StopHarvesting();
    }
    StopMoving();
    EmoteUntil = 0.0f;
    ReviveTarget = nullptr;
    ReviveDoneAt = 0.0f;
    Pawn->bIsDBNO = true;
    static const float InitialHealth = AthenaGameDataValue(L"Default.DBNO.InitialHealth", 100.0f);
    Pawn->SetHealth(std::max(InitialHealth, 1.0f));
    if (Natives::AFortPlayerPawn_UpdateDBNOInteractCollision)
    {
        Natives::AFortPlayerPawn_UpdateDBNOInteractCollision(reinterpret_cast<AFortPawn*>(Pawn));
    }
    UAbilitySystemComponent* AbilitySystem = static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent);
    static UClass* DBNOAbility = LoadObject<UClass>(nullptr, L"/Game/Abilities/NPC/Generic/GAB_AthenaDBNO.GAB_AthenaDBNO_C");
    bool bAbility = false;
    if (AbilitySystem && DBNOAbility && DBNOAbility->IsChildOf(UGameplayAbility::StaticClass()))
    {
        if (!AbilitySystem->FindAbilitySpecFromClass(DBNOAbility))
        {
            UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
            std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
            SpecStruct->InitializeStruct(SpecStorage.data());
            FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
            FGameplayAbilitySpecHandle NewHandle;
            NewHandle.GenerateNewHandle();
            Spec.Handle = NewHandle;
            Spec.Ability = static_cast<UGameplayAbility*>(DBNOAbility->GetDefaultObject());
            Spec.Level = 1;
            Spec.InputID = INDEX_NONE;
            AbilitySystem->GiveAbility(Spec);
            SpecStruct->DestroyStruct(SpecStorage.data());
        }
        AController* DownerController = InDowner ? Cast<AController>(reinterpret_cast<UObject*>(InDowner->GetOwner())) : nullptr;
        bAbility = SendGameplayEvent(reinterpret_cast<AActor*>(Pawn), L"Event.Damage.DiedAthena", reinterpret_cast<UObject*>(DownerController));
    }
    Downer = InDowner;
    DBNOStartedAt = Now;
    NextBleedTime = Now + 1.0f;
    LastDBNOHealth = Pawn->GetHealth();
    ++TimesDowned;
    Log::Info(L"Battle Royale bots: {} is down (DBNO){}, {:.0f} health, the DBNO ability {}", BotName, InDowner ? std::format(L", knocked by {}", InDowner->GetPlayerName().ToStdString()) : std::wstring(),
        Pawn->GetHealth(), bAbility ? L"triggered" : L"not triggered");
#endif
}

void AFortAthenaAIBotController::Downed(float Now)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn)
    {
        return;
    }
    StopFiring();
    // The team all down: eliminated (the last one knocked is everyone's finisher).
    bool bTeammateUp = false;
    AFortAthenaAIBotController* Nearest = nullptr;
    float NearestDistance = 1e9f;
    const FVector Location = LocationOf(Pawn);
    for (AFortAthenaAIBotController* Teammate : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() != EFortAthenaAIBotState::Landed || Teammate->IsDBNO())
        {
            continue;
        }
        bTeammateUp = true;
        const float Distance = Dist2D(LocationOf(Teammate->GetPawn()), Location);
        if (Distance < NearestDistance)
        {
            NearestDistance = Distance;
            Nearest = Teammate;
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
    if (!bTeammateUp)
    {
        Eliminate(L"Gameplay.Damage.DBNOBleedOut", L"no teammate left standing");
        return;
    }
    // Bleeding: GE_Athena_DBNO_Bleed (AthenaGameData Default.DBNO.BleedDamage every Default.DBNO.BleedTick s) when the
    // ability applied it; Core bleeds the pawn the same when its health hasn't moved.
    if (Now >= NextBleedTime)
    {
        static const float BleedTick = std::max(AthenaGameDataValue(L"Default.DBNO.BleedTick", 1.0f), 0.1f);
        static const float BleedDamage = AthenaGameDataValue(L"Default.DBNO.BleedDamage", 1.0f);
        NextBleedTime = Now + BleedTick;
        const float Health = Pawn->GetHealth();
        if (Health >= LastDBNOHealth - 0.01f && Health > 0.0f)
        {
            Pawn->SetHealth(Health - BleedDamage);
        }
        LastDBNOHealth = Pawn->GetHealth();
    }
    if (Pawn->GetHealth() <= 0.5f)
    {
        Eliminate(L"Gameplay.Damage.DBNOBleedOut", L"bled out");
        return;
    }
    // Crawling to the nearest teammate standing, who comes to revive it.
    if (Nearest && NearestDistance > 250.0f)
    {
        MoveTowards(LocationOf(Nearest->GetPawn()), Now, 200.0f);
    }
    else
    {
        StopMoving();
    }
#endif
}

bool AFortAthenaAIBotController::ReviveTeammate(float Now)
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
    AFortAthenaAIBotController* Target = ReviveTarget;
    // A revive goes on with enemies about, but not with one close by in sight or while being shot at.
    const bool bThreat = IsUnderThreat(Now);
    const bool bTargetValid = Target && Target->IsAlive() && Target->IsDBNO() && Target->GetPawn();
    if (!bTargetValid || bThreat)
    {
        if (Target && ReviveDoneAt > 0.0f)
        {
            Log::Info(L"Battle Royale bots: {} stops reviving {}", BotName, Target->GetBotName());
        }
        ReviveTarget = nullptr;
        ReviveDoneAt = 0.0f;
        Target = nullptr;
        if (bThreat)
        {
            return false;
        }
    }
    const FVector Location = LocationOf(Pawn);
    if (!Target)
    {
        float Best = 6000.0f;
        for (AFortAthenaAIBotController* Teammate : Manager->TeammatesOf(this))
        {
            const float Distance = Teammate->IsDBNO() && Teammate->GetPawn() ? Dist2D(LocationOf(Teammate->GetPawn()), Location) : 1e9f;
            if (Distance < Best)
            {
                Best = Distance;
                Target = Teammate;
            }
        }
        if (!Target)
        {
            return false;
        }
        ReviveTarget = Target;
        ReviveDoneAt = 0.0f;
    }
    AFortPlayerPawnAthena* Downed = Target->GetPawn();
    const FVector DownedLocation = LocationOf(Downed);
    if (Dist2D(DownedLocation, Location) > 180.0f)
    {
        ReviveDoneAt = 0.0f;
        MoveTowards(DownedLocation, Now, 120.0f);
        return true;
    }
    StopMoving();
    if (AAIController* AIController = Controller.Get())
    {
        AIController->SetFocalPoint(DownedLocation);
    }
    if (ReviveDoneAt <= 0.0f)
    {
        const float ReviveTime = NumberOf(reinterpret_cast<UObject*>(Downed), L"ReviveFromDBNOTime", 10.0f);
        ReviveDoneAt = Now + std::max(ReviveTime, 1.0f);
        Log::Info(L"Battle Royale bots: {} revives {} ({:.0f} s)", BotName, Target->GetBotName(), ReviveTime);
        return true;
    }
    if (Now < ReviveDoneAt)
    {
        return true;
    }
    ReviveTarget = nullptr;
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
    Target->OnRevived();
    ++Revives;
    Log::Info(L"Battle Royale bots: {} revived {} ({}), {:.0f} health", BotName, Target->GetBotName(), bNative ? L"the game's ReviveFromDBNO" : L"Core's, the game's refused", Downed->GetHealth());
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::OnRevived()
{
    Downer = nullptr;
    DBNOStartedAt = 0.0f;
    LastHurtTime = -100.0f;
    StopMoving();
}

bool AFortAthenaAIBotController::StayWithTeam(float Now)
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
    FVector Goal;
    for (AFortAthenaAIBotController* Teammate : Manager->TeammatesOf(this))
    {
        if (Teammate->GetState() != EFortAthenaAIBotState::Landed || Teammate->IsDBNO() || !Teammate->GetPawn())
        {
            continue;
        }
        const FVector Other = LocationOf(Teammate->GetPawn());
        const float Distance = Dist2D(Other, Location);
        if (Distance < Best)
        {
            Best = Distance;
            Goal = Other;
        }
    }
    if (Best > 2500.0f && Best < 40000.0f)
    {
        MoveTowards(Goal, Now, 800.0f);
        return true;
    }
    return false;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::TryToUnstuck(float Now)
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
    if (bMoving && Dist2D(Location, LastStuckCheckLocation) < 60.0f)
    {
        ++StuckChecks;
        static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->Jump();
        ++Jumps;
        if (StuckChecks == 2 && bPathFollowing)
        {
            // The path doesn't get it anywhere: steer for a while.
            StopMoving();
            PathFailedUntil = Now + 4.0f;
        }
        if (StuckChecks == 2 && !HarvestTarget.Get() && FindBuildingInTheWay(Now))
        {
            // Something it can break is in the way: the pickaxe, as a stuck player does.
            StuckChecks = 0;
            LastStuckCheckLocation = Location;
            return;
        }
        if (StuckChecks >= 3)
        {
            StuckChecks = 0;
            MarkUnreachable(LootTarget.Get());
            MarkUnreachable(ContainerTarget.Get());
            LootTarget = nullptr;
            ContainerTarget = nullptr;
            SearchUntil = 0.0f;
            NextContainerSearch = Now + 6.0f;
            bHasWanderGoal = true;
            const float Angle = RandomRange(0.0f, 6.2831853f);
            WanderGoal = FVector(Location.X + std::cos(Angle) * 700.0f, Location.Y + std::sin(Angle) * 700.0f, Location.Z);
            NextWanderTime = Now + 3.0f;
        }
    }
    else
    {
        StuckChecks = 0;
    }
    LastStuckCheckLocation = Location;
#endif
}

void AFortAthenaAIBotController::TakeStormDamage(float Now)
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
    if (!Pawn || !Manager->GetCurrentSafeZone(Center, Radius))
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

void AFortAthenaAIBotController::DropItem(UFortItemDefinition* Definition, int32_t Count, int32_t LoadedAmmo)
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
    const FVector Spot(Location.X + RandomRange(-80.0f, 80.0f), Location.Y + RandomRange(-80.0f, 80.0f), Location.Z);
    AFortPickup::CreatePickup(World, *Entry, &Spot);
    EntryStruct->DestroyStruct(Entry);
#endif
}

void AFortAthenaAIBotController::DropInventory()
{
#if WITH_SERVER_CODE
    // 8.30's bots drop their loot as players do: each weapon (with its magazine), the ammo and the healing items, where
    // the bot fell.
    HealingItem = nullptr;
    for (const FFortAthenaAIBotWeapon& Weapon : WeaponList)
    {
        DropItem(Weapon.WeaponDefinition, 1, Weapon.LoadedAmmo);
    }
    for (const auto& [Definition, Count] : Ammo)
    {
        DropItem(Definition, Count, 0);
    }
    for (const auto& [Definition, Count] : Consumables)
    {
        DropItem(Definition, Count, 0);
    }
    if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
    {
        DropItem(GameData->WoodItemDefinition, Materials[0], 0);
        DropItem(GameData->StoneItemDefinition, Materials[1], 0);
        DropItem(GameData->MetalItemDefinition, Materials[2], 0);
    }
    Materials[0] = Materials[1] = Materials[2] = 0;
    HarvestTarget = nullptr;
    WeaponList.clear();
    Ammo.clear();
    Consumables.clear();
#endif
}

bool AFortAthenaAIBotController::Heal(float Now)
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
        const FFortAthenaAIBotConsumable* Info = GetConsumableInfo(HealingItem);
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
        const FFortAthenaAIBotConsumable* Info = Count > 0 ? GetConsumableInfo(Definition) : nullptr;
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
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::CancelHeal()
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
        UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
        if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
        {
            EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
            bPickaxeEquipped = true;
            EquippedIndex = -1;
        }
    }
}

void AFortAthenaAIBotController::SetLandingSpot(const FVector& Spot, float Radius, float InJumpTime)
{
    bHasLandingSpot = true;
    LandingSpot = Spot;
    LandingRadius = Radius;
    JumpTime = InJumpTime;
}

void AFortAthenaAIBotController::OnPossesedPawnDied(AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortAthenaAIBotController::OnPossesedPawnDied. The bot's player state takes its death report as a
    // player's does (AFortPlayerControllerAthena's handler): who eliminated it (else itself) and how; it replicates
    // (OnRep_DeathInfo), which writes the clients' kill feed.
    StopFiring();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (PlayerState && AFortPlayerStateAthena::HasEliminator())
    {
        FDeathInfo& DeathInfo = PlayerState->DeathInfo;
        DeathInfo.FinisherOrDowner = KillerState ? KillerState : PlayerState;
        DeathInfo.bDBNO = false;
        if (Pawn && Natives::AFortPlayerPawn_GetDeathCause)
        {
            DeathInfo.DeathCause = Natives::AFortPlayerPawn_GetDeathCause(Pawn, Pawn->DeathTags, false);
        }
        PlayerState->ForceNetUpdate();
    }
    DropInventory();
    State = EFortAthenaAIBotState::Dead;
    CurrentTarget = nullptr;
    LootTarget = nullptr;
    Log::Info(L"Battle Royale bots: {} died at ({:.0f}, {:.0f}, {:.0f}){}", BotName, LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z,
        KillerState ? std::format(L", eliminated by {}", KillerState->GetPlayerName().ToStdString()) : std::wstring(L" (nobody's elimination)"));
#endif
}
