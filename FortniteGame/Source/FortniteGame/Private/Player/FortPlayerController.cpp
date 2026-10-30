#include "pch.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Abilities/FortAbilityKit.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "BackendRequest.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Homebase/FortHomebaseManager.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "FortSpottableActorInterface.h"
#include "FortTeamInfo.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "GameFramework/Character.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Online/FortMcpProfileAccount.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Pawns/FortPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/AssetPtr.h"
#include "UObject/UnrealType.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    // UFortAbilityKit::Gadgets holds object pointers on 1.1 and 1.2 and TAssetPtrs (0x20-byte entries) on 1.7.2.
    std::vector<UFortGadgetItemDefinition*> KitGadgets(UFortAbilityKit* Kit)
    {
        static const int32_t EntrySize = []
        {
            UClass* KitClass = UFortAbilityKit::StaticClass();
            const UProperty* Property = KitClass ? KitClass->FindPropertyByName(FName(L"Gadgets", FNAME_Find)) : nullptr;
            const UProperty* Inner = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UArrayProperty) ? static_cast<const UArrayProperty*>(Property)->Inner : nullptr;
            return Inner ? static_cast<int32_t>(Inner->ElementSize) : static_cast<int32_t>(sizeof(void*));
        }();

        std::vector<UFortGadgetItemDefinition*> Gadgets;
        const TArray<UFortGadgetItemDefinition*>& Entries = Kit->Gadgets;
        for (int32_t Index = 0; Index < Entries.Num(); ++Index)
        {
            const uintptr_t Entry = reinterpret_cast<uintptr_t>(Entries.GetData()) + static_cast<uintptr_t>(Index) * EntrySize;
            Gadgets.push_back(EntrySize == sizeof(FAssetPtr)
                ? reinterpret_cast<const TAssetPtr<UFortGadgetItemDefinition>*>(Entry)->LoadSynchronous()
                : *reinterpret_cast<UFortGadgetItemDefinition* const*>(Entry));
        }
        return Gadgets;
    }

    struct FHeroAbilityKitItems
    {
        TWeakObjectPtr<AFortPlayerController> PlayerController;
        std::vector<TWeakObjectPtr<UFortItemDefinition>> GrantedDefinitions;
        std::vector<FGuid> PendingQuickBarItems;
        std::vector<TWeakObjectPtr<UFortItemDefinition>> GrantedHomebaseGadgets;
    };

    std::unordered_map<const AFortPlayerController*, FHeroAbilityKitItems> HeroAbilityKitItems;

    FHeroAbilityKitItems& GetHeroAbilityKitItems(AFortPlayerController* PlayerController)
    {
        FHeroAbilityKitItems& Items = HeroAbilityKitItems[PlayerController];
        if (Items.PlayerController.Get() != PlayerController)
        {
            std::erase_if(HeroAbilityKitItems, [PlayerController](const auto& Entry) { return Entry.first != PlayerController && !Entry.second.PlayerController.Get(); });
            Items = FHeroAbilityKitItems{ TWeakObjectPtr<AFortPlayerController>(PlayerController) };
        }
        return Items;
    }

    int32_t AbilityKitItemCount(const FAbilityKitItem& KitItem, int32_t Owned)
    {
        const int32_t Quantity = KitItem.Quantity;
        const EFortReplenishmentType Replenishment = KitItem.Replenishment;
        if (Replenishment == EFortReplenishmentType::GetClampMin())
        {
            return Quantity - Owned;
        }
        if (Replenishment == EFortReplenishmentType::GetAdd())
        {
            return Quantity;
        }
        return Owned > 0 ? 0 : Quantity;
    }

    std::vector<UFortWorldItem*> AddHeroAbilityKitItem(AFortPlayerController* PlayerController, AFortInventory* Inventory, UFortWorldItemDefinition* ItemDefinition, int32_t Count, int32_t Level)
    {
        std::vector<UFortWorldItem*> Added;
        const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
        int32_t Remaining = Count;

        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
        {
            UFortWorldItem* Existing = Instances[Index];
            FFortItemEntry* ExistingEntry = Existing ? &static_cast<FFortItemEntry&>(Existing->ItemEntry) : nullptr;
            if (ExistingEntry && static_cast<UFortItemDefinition*>(ExistingEntry->ItemDefinition) == ItemDefinition && ExistingEntry->Count < MaxStackSize)
            {
                const int32_t Merged = std::min(Remaining, MaxStackSize - ExistingEntry->Count);
                Inventory->UpdateWorldItemCount(Existing, ExistingEntry->Count + Merged);
                Remaining -= Merged;
            }
        }

        while (Remaining > 0)
        {
            const int32_t StackCount = std::min(Remaining, MaxStackSize);
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Level));
            if (!WorldItem)
            {
                break;
            }

            WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
            Inventory->AddWorldItem(WorldItem);
            Added.push_back(WorldItem);
            Remaining -= StackCount;
        }
        return Added;
    }

    // Where a player's drop is tossed: 512 out from the pawn, level with it, up to 20 degrees to either side of where it
    // faces. The drop itself (ServerSpawnInventoryDrop_Implementation, AFortPlayerController slot 0xF38 on 1.7.2 and 0xF48
    // on 1.8) is stripped, so this follows the game's one toss from a player that is left: the death backpack's (1.7.2
    // 0x80DB20, 1.8 0x82DB00) throws it 512 out along FMath::VRandCone(Dir, 20 degrees, 20 degrees) (0x12A4CF0, 1.8
    // 0x12F3E60), and the toss (0x6622F0) sends a pickup with nowhere to go 512 out too. The backpack goes up off the
    // body; a drop goes the way the player faces, along the level part of the same cone. Every drop aimed straight ahead
    // came down in one spot, 136 to 143 out (#108): the toss falls short (TossOntoFloor), and its random fly time hardly
    // moves it. The cone's up and down is left out, as a drop aimed higher lands further out (up to 290 when measured),
    // which would carry two drops made from one spot facing different ways out of each other's CombineRadius.
    FVector GetDropTossLocation(const FTransform& PawnTransform)
    {
        constexpr float Pi = 3.1415926535897932f;
        constexpr float TossDistance = 512.0f;
        constexpr float ConeHalfAngle = 20.0f * Pi / 180.0f;

        // VRandCone's direction: Phi from Dir (wrapped into the cone), turned Theta round it; its side part is sin(Theta).
        const float Theta = 2.0f * Pi * FMath::FRand();
        const float Phi = std::fmod(std::acos(FMath::Clamp(2.0f * FMath::FRand() - 1.0f, -1.0f, 1.0f)), ConeHalfAngle);
        const float Yaw = PawnTransform.Rotation.Rotator().Yaw * (Pi / 180.0f) + std::atan2(std::sin(Phi) * std::sin(Theta), std::cos(Phi));
        const FVector Location = PawnTransform.GetLocation();
        return FVector(Location.X + std::cos(Yaw) * TossDistance, Location.Y + std::sin(Yaw) * TossDistance, Location.Z);
    }

    // Who may cheat (CoreV2 #138): the admin panel's Settings say "everyone", "admins" (the accounts it lists) or "off",
    // and the backend answers for one account at a time (Core-Backend's GET /fortnite/api/game/v2/cheats). An answer is
    // used for 30 seconds after it came, so a change in the panel reaches a running match within that; a cheat sent
    // after that waits for a new one, which takes a moment.
    constexpr auto CheatAnswerLifetime = std::chrono::seconds(30);
    // How many cheats one account can have waiting for an answer: more are refused until it comes.
    constexpr size_t MaxWaitingCheats = 16;

    struct FCheatPermission
    {
        // Until when the answer holds: a cheat after it asks again.
        std::chrono::steady_clock::time_point Expires{};
        bool bAsking = false;
        // Whether the backend has ever answered for this account, and its last answer.
        bool bKnown = false;
        bool bAllowed = false;
        // The backend's mode, "off" or "admins", for what a player who may not cheat is told.
        std::string Mode;
        // The cheats that came while it was being asked, run or refused once it answers.
        std::vector<std::pair<TWeakObjectPtr<AFortPlayerController>, std::wstring>> Waiting;
    };

    // By account id (or "name:" and the player's name when the player state has no account id).
    std::unordered_map<std::wstring, FCheatPermission> CheatPermissions;
    // The backend has no such route (one from before CoreV2 #138): every player may cheat, as before, and it isn't asked
    // again.
    bool bCheatPermissionsUnsupported = false;

    // An MCP account id is 32 hex digits (an invalid id's ToString is "INVALID").
    bool IsCheatAccountId(const std::wstring& Id)
    {
        return !Id.empty() && Id.size() <= 64 && std::all_of(Id.begin(), Id.end(), [](wchar_t Character) { return std::iswxdigit(Character) != 0; });
    }

    // Text for a URL's query: UTF-8, with everything but letters, digits and -_.~ %-escaped.
    std::wstring QueryEscape(const std::wstring& Text)
    {
        const int32_t Wide = static_cast<int32_t>(Text.size());
        std::string Utf8(static_cast<size_t>(std::max(WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), Wide, nullptr, 0, nullptr, nullptr), 0)), '\0');
        if (!Utf8.empty())
        {
            WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), Wide, Utf8.data(), static_cast<int32_t>(Utf8.size()), nullptr, nullptr);
        }
        constexpr const wchar_t* Hex = L"0123456789ABCDEF";
        std::wstring Result;
        for (const char Character : Utf8)
        {
            const unsigned char Byte = static_cast<unsigned char>(Character);
            if (Byte < 0x80 && (std::isalnum(Byte) || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~'))
            {
                Result.push_back(static_cast<wchar_t>(Byte));
                continue;
            }
            Result.push_back(L'%');
            Result.push_back(Hex[Byte >> 4]);
            Result.push_back(Hex[Byte & 0xF]);
        }
        return Result;
    }

    FString CheatsRefused(const std::string& Mode)
    {
        return FString(Mode == "admins" ? L"Cheats are only for this server's admins" : L"Cheats are turned off on this server");
    }

    // Runs the account's waiting cheats, or tells each player Refusal and runs none.
    void SettleWaitingCheats(FCheatPermission& Permission, bool bRun, const FString& Refusal)
    {
        std::vector<std::pair<TWeakObjectPtr<AFortPlayerController>, std::wstring>> Waiting = std::exchange(Permission.Waiting, {});
        for (const auto& [WeakPlayerController, Msg] : Waiting)
        {
            AFortPlayerController* PlayerController = WeakPlayerController.Get();
            if (!PlayerController)
            {
                continue;
            }
            if (bRun)
            {
                PlayerController->RunServerCheat(FString(Msg.c_str()));
            }
            else
            {
                UE_LOG(LogFort, Log, TEXT("ServerCheat: %s may not cheat, \"%s\" not run"), *GetNameSafe(PlayerController), Msg.c_str());
                PlayerController->ClientMessage(Refusal);
            }
        }
    }

    void AskCheatPermission(const std::wstring& Key, const std::wstring& AccountId, const std::wstring& DisplayName)
    {
        CheatPermissions[Key].bAsking = true;
        BackendRequest::GetJson(std::format(L"cheat permission of {}", DisplayName.empty() ? Key : DisplayName),
            L"/fortnite/api/game/v2/cheats?accountId=" + QueryEscape(AccountId) + L"&displayName=" + QueryEscape(DisplayName),
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Key](uint32_t Status, const std::string& Body)
        {
            // {accountId, displayName, mode, allowed}: allowed is true or false. Anything else (no answer, an error)
            // keeps the last answer, if there was one.
            const std::string Allowed = Status == 200 ? BackendRequest::JsonField(Body, "allowed") : std::string();
            const std::string Mode = Status == 200 ? BackendRequest::JsonField(Body, "mode") : std::string();
            GameThread::Enqueue([Key, Status, Allowed, Mode]()
            {
                FCheatPermission& Permission = CheatPermissions[Key];
                Permission.bAsking = false;
                if (Status == 404)
                {
                    if (!bCheatPermissionsUnsupported)
                    {
                        bCheatPermissionsUnsupported = true;
                        UE_LOG(LogFort, Warning, TEXT("ServerCheat: the backend doesn't say who may cheat (CoreV2 #138), so every player may"));
                    }
                    for (auto& Entry : CheatPermissions)
                    {
                        SettleWaitingCheats(Entry.second, true, FString());
                    }
                    return;
                }
                if (Allowed == "true" || Allowed == "false")
                {
                    const bool bAllowed = Allowed == "true";
                    if (!Permission.bKnown || Permission.bAllowed != bAllowed || Permission.Mode != Mode)
                    {
                        const std::wstring ModeText(Mode.begin(), Mode.end());
                        UE_LOG(LogFort, Log, TEXT("ServerCheat: %s %s (cheats: %s)"), Key.c_str(), bAllowed ? TEXT("may cheat") : TEXT("may not cheat"), ModeText.c_str());
                    }
                    Permission.bKnown = true;
                    Permission.bAllowed = bAllowed;
                    Permission.Mode = Mode;
                    Permission.Expires = std::chrono::steady_clock::now() + CheatAnswerLifetime;
                    SettleWaitingCheats(Permission, bAllowed, CheatsRefused(Mode));
                    return;
                }
                if (Permission.bKnown)
                {
                    // The last answer holds a while longer.
                    Permission.Expires = std::chrono::steady_clock::now() + CheatAnswerLifetime;
                    SettleWaitingCheats(Permission, Permission.bAllowed, CheatsRefused(Permission.Mode));
                    return;
                }
                // Never answered: nothing runs, and the next cheat asks again.
                SettleWaitingCheats(Permission, false, FString(L"Couldn't ask the backend whether you may cheat: try again in a moment"));
            });
        });
    }
}

bool AFortPlayerController::InitializeMcpProfile(const void* OnComplete)
{
    const bool bInitialized = InitializeMcpProfileOG(this, OnComplete);

#if WITH_SERVER_CODE
    // Cheats are on for every player from the moment they join (#65, #69), pawn or not: Possess grants it too, but a
    // player without a pawn (Battle Royale before its spawn was restored) never got one, and every "cheat" was dropped.
    if (Role == ROLE_Authority && !CheatManager)
    {
        AddCheats(true);
        UE_LOG(LogFort, Log, TEXT("InitializeMcpProfile: %s has cheat manager %s"), *GetNameSafe(this), *GetNameSafe(static_cast<UObject*>(CheatManager)));
    }

    if (Role == ROLE_Authority && !QuickBars)
    {
        SpawnQuickBars();
        UWorld* World = GetWorld();
        AFortGameModeAthena* AthenaGameMode = World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
        if (AthenaGameMode)
        {
            if (AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)))
            {
                AthenaController->SetupBackpackSize();
            }
            GiveStartingItems(AthenaGameMode->StartingItems);
        }
        else if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
        {
            InitializeInventory(GameData->DefaultInventoryList);
        }
    }
#endif

    return bInitialized;
}

void AFortPlayerController::SetRotatePawnToCamera(bool bNewRotatePawnToCamera)
{
    Reflection::FParams Params(StaticFunction_SetRotatePawnToCamera());
    Params.Set(L"bNewRotatePawnToCamera", bNewRotatePawnToCamera);
    Params.Call(this);
}

bool AFortPlayerController::GetRotatePawnToCamera()
{
    Reflection::FParams Params(StaticFunction_GetRotatePawnToCamera());
    return Params.Call(this) ? Params.Take<bool>(L"ReturnValue") : true;
}

void AFortPlayerController::ServerSetPartyOwner(const FUniqueNetIdRepl& PartyOwnerUniqueId)
{
    // Called on the server, the RPC runs its own _Implementation (1.7.2 0x828360): the player state's
    // PartyOwnerUniqueId, then the game session's lobby beacon (0x6E8360).
    Reflection::FParams Params(StaticFunction_ServerSetPartyOwner());
    Params.Set(L"PartyOwnerUniqueId", PartyOwnerUniqueId);
    Params.Call(this);
}

void AFortPlayerController::Possess(APawn* InPawn)
{
    const bool bAthenaPossess = AthenaPossessOG && Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this));
    (bAthenaPossess ? AthenaPossessOG : PossessOG)(this, InPawn);

#if WITH_SERVER_CODE
    // The server forwards a player's voice only to players whose voice handshake it completed (UNetConnection's
    // ShouldReplicateVoicePacketFrom, 1.7.2 0x223C880: the receiving controller's MuteList.bHasVoiceHandshakeCompleted),
    // which AGameModeBase::UpdateGameplayMuteList does on login. Core-Backend #54's testers never got the handshake on
    // their clients, so it is completed here too if it hasn't been.
    if (Role == ROLE_Authority && Finder::ResolveVoiceHandshake())
    {
        uint8_t& bHandshakeCompleted = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(this) + Natives::APlayerController_VoiceHandshakeCompletedOffset);
        if (!bHandshakeCompleted)
        {
            Log::Info(L"Voice chat: {}'s voice handshake wasn't completed; Core completes it so they hear voice", GetName().ToStdString());
            bHandshakeCompleted = 1;
        }
    }

    // UpdateRotation only turns the pawn to the camera (FaceRotation) while this is on. The DBNO death montage's
    // AnimNotifyState_DisablePawnRotation turns it off and can't turn it back on once the dying pawn has lost its
    // controller; only ClientRestart_Implementation re-arms it, and that runs on the owning client. Without this the
    // respawned pawn keeps its spawn facing on the server, so moving any other way is backpedalling there (#64).
    if (Role == ROLE_Authority && MyFortPawn && !GetRotatePawnToCamera())
    {
        SetRotatePawnToCamera(true);
        UE_LOG(LogFort, Log, TEXT("Possess: %s had pawn-to-camera rotation off on the server, turned it back on"), *GetNameSafe(this));
    }

    // Cheats are on for every player (#65): ServerCheat_Implementation runs the client's "cheat <command>" on this
    // controller's cheat manager, which the game mode never allows on a dedicated server.
    if (Role == ROLE_Authority && !CheatManager)
    {
        AddCheats(true);
        UE_LOG(LogFort, Log, TEXT("Possess: %s has cheat manager %s"), *GetNameSafe(this), *GetNameSafe(static_cast<UObject*>(CheatManager)));
    }

    AFortPlayerState* FortPlayerState = Role == ROLE_Authority && MyFortPawn ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
    if (FortPlayerState && !FortPlayerState->bHasStartedPlaying)
    {
        FortPlayerState->bHasStartedPlaying = true;
        FortPlayerState->ForceNetUpdate();
    }

    // A disguise (#134, "cheat disguise") possesses a pawn of another kind, a husk or another player pawn class, and gives
    // the player's own pawn back after: the player keeps their hero, attributes and items through it, so only what the
    // possessed pawn needs to be played runs (its character parts, and a player pawn's equipped item).
    const bool bDisguisePossess = UFortCheatManager::bDisguisePossess;
    AFortPlayerPawn* const PossessedPlayerPawn = Role == ROLE_Authority ? Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))) : nullptr;
    if (Role == ROLE_Authority && InPawn && !PossessedPlayerPawn)
    {
        UE_LOG(LogFort, Log, TEXT("Possess: %s plays %s, not a player pawn"), *GetNameSafe(this), *GetNameSafe(InPawn));
        return;
    }

    AFortPlayerControllerAthena* AthenaController = !bDisguisePossess && Role == ROLE_Authority && MyFortPawn && AFortGameModeAthena::GetCurrent(this) ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)) : nullptr;
    UFortHero* AthenaHero = AthenaController ? AthenaController->SetupHero() : nullptr;

    // 1.8: no player's pawn reaches the clients with an empty glider (#80). Its loadout was copied from the controller's
    // by the Possess above, and replicates once this returns.
    if (AthenaController)
    {
        AthenaController->FillEmptyLoadout(Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))));
    }

    if (AFortPlayerPawn* PlayerPawn = PossessedPlayerPawn)
    {
        // A pawn takes the hero's character parts (its skeletal mesh and anim class) when the player state's parts change
        // (1.8 0x88C280, after OnRep_CharacterParts). Save the World sets the hero before the pawn exists, so the server's
        // pawn kept its class defaults; PlayerPawn_Ninja's have no mesh or anim class, so without this its montages and
        // root motion never ran on the server (#102, #103). Battle Royale's SetupHero above already sets them on this pawn.
        AFortPlayerState* PartsState = !AthenaController ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
        if (PartsState)
        {
            PartsState->OnRep_CharacterParts();
        }

        // A disguise keeps the player's health and shield (the player state's attributes) as they were.
        if (!bDisguisePossess)
        {
            PlayerPawn->ResetGameplayAttributes();
        }
        if (AthenaController)
        {
            UFortHeroType* HeroType = AthenaHero ? AthenaHero->GetHeroTypeBP() : nullptr;
            UE_LOG(LogFort, Log, TEXT("Possess: %s plays %s with health %.0f/%.0f, shield %.0f/%.0f"), *GetNameSafe(this), *GetNameSafe(HeroType),
                PlayerPawn->GetHealth(), PlayerPawn->GetMaxHealth(), PlayerPawn->GetShield(), PlayerPawn->GetMaxShield());
        }
    }

    // "cheat skin" (#134, #136): after a respawn, the look the player chose instead of the one the hero (Battle Royale's
    // SetupHero above) put back. Only its look: the attributes above stay the player's own hero's.
    if (PossessedPlayerPawn && !bDisguisePossess)
    {
        UFortCheatManager::ReapplySkin(this);
    }

    // 1.8: the backpack size and the pickaxe come with the loadout, made when the pawn first spawns.
    if (AthenaController && OverriddenBackpackSize <= 0)
    {
        AthenaController->SetupBackpackSize();
    }
    if (AthenaController)
    {
        AthenaController->GiveLoadoutPickaxe();
    }

    AFortQuickBars* Bars = QuickBars;
    if (Role == ROLE_Authority && Bars && MyFortPawn)
    {
        // Battle Royale's inventory is the game mode's StartingItems (with its own pickaxe) and what the player loots: no
        // hero gadgets, harvesting tool from the account's attribute or homebase gadgets. The native homebase effects skip
        // Athena too (UFortRegisteredPlayerInfo 0x5DC040, through the Athena check 0x5EAF90).
        if (!bDisguisePossess && !AFortGameModeAthena::GetCurrent(this))
        {
            AddHeroAbilityKitGadgetsToQuickBars();
            ApplyHarvestingTool();
            ApplyHomebaseGadgets();
        }

        // A Battle Royale player spawns holding the harvesting tool (slot 0) when the building bar has the focus. Both
        // bars start focused on slot 0, and adding an item to a bar's focused slot focuses that bar
        // (ServerAddItemInternal_Implementation, 1.8 0x693DF0), which unfocuses the other one (-1). 1.7.2's first
        // starting item is the pickaxe; 1.8's are the building pieces, its pickaxe coming later from the loadout, so the
        // wall was held (#106) and the activation of the primary bar's focused slot (-1) did nothing. A player who died
        // or boarded the aircraft in build mode spawns with the pickaxe too.
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        int32_t ActivatedSlot = PrimaryQuickBar.CurrentFocusedSlot;
        if (AthenaController && ActivatedSlot < 0)
        {
            UE_LOG(LogFort, Log, TEXT("Possess: %s had the building bar focused, it holds the harvesting tool"), *GetNameSafe(this));
            ActivatedSlot = 0;
        }
        Bars->ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), ActivatedSlot, 0.0f, false);

        // A pawn leaving the aircraft is already skydiving here (Battle Royale's Possess, 1.7.2 0x441AD0, exits the
        // aircraft). The activation above only asks the client to equip the slot's item (ClientExecuteInventoryItem,
        // 0x8058A0), which needs the client to have the new pawn and nothing blocking a change of equipment. Skydiving only
        // holsters the pawn's current weapon (0x44DDA0) and landing draws it again (0x41AB80), so without one the player
        // landed empty-handed (#82). Equip the focused item now, as the client's request would.
        AFortPlayerPawnAthena* AthenaPawn = AthenaController ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))) : nullptr;
        const int32_t FocusedSlot = PrimaryQuickBar.CurrentFocusedSlot;
        TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
        if (AthenaPawn && AthenaPawn->bIsSkydiving && !static_cast<AFortWeapon*>(AthenaPawn->CurrentWeapon) && FocusedSlot >= 0 && FocusedSlot < Slots.Num())
        {
            const TArray<FGuid>& SlotItems = Slots[FocusedSlot].Items;
            if (SlotItems.Num() > 0)
            {
                ServerExecuteInventoryItem_Implementation(SlotItems[0]);
                UE_LOG(LogFort, Log, TEXT("Possess: %s left the aircraft holding %s"), *GetNameSafe(this), *GetNameSafe(static_cast<AFortWeapon*>(AthenaPawn->CurrentWeapon)));
            }
        }
    }
#endif
}

void AFortPlayerController::SpawnQuickBars()
{
#if WITH_SERVER_CODE
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.Owner = this;
    QuickBars = GetWorld()->SpawnActor<AFortQuickBars>(AFortQuickBars::StaticClass(), SpawnInfo);
    SetupQuickBars();
#endif
}

void AFortPlayerController::SetupQuickBars()
{
#if WITH_SERVER_CODE
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UWorld* World = GetWorld();
    AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    if (!QuickBars)
    {
        return;
    }

    if (GameMode && GameMode->OverridesQuickBars())
    {
        QuickBars->SetupQuickbar(EFortQuickBars::GetPrimary(), GameMode->GetQuickBarDefinition(EFortQuickBars::GetPrimary()));
        QuickBars->SetupQuickbar(EFortQuickBars::GetSecondary(), GameMode->GetQuickBarDefinition(EFortQuickBars::GetSecondary()));
    }
    else if (GameData)
    {
        QuickBars->SetupQuickbar(EFortQuickBars::GetPrimary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetPrimary()));
        QuickBars->SetupQuickbar(EFortQuickBars::GetSecondary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetSecondary()));
    }
#endif
}

void AFortPlayerController::InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items)
{
#if WITH_SERVER_CODE
    if (!WorldInventory)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr)
    {
        WorldManager->RestorePlayerQuickBars(this);
    }

    AFortInventory* Inventory = WorldInventory;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        TryAddToQuickBar(Instances[Index]);
    }

    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        const FItemDefinitionAndCount& Entry = Items[Index];
        const TAssetPtr<UFortItemDefinition>& EntryDefinition = Entry.ItemDefinition;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(EntryDefinition.LoadSynchronous());
        if (ItemDefinition && ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
        {
            continue;
        }
        if (ItemDefinition && GetItemQuantity(ItemDefinition) <= 0)
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(this, ItemDefinition, Entry.Count, false);
        }
    }
#endif
}

void AFortPlayerController::GiveStartingItems(const TStructArray<FItemAndCount>& Items)
{
#if WITH_SERVER_CODE
    for (int32_t Index = 0; WorldInventory && Index < Items.Num(); ++Index)
    {
        const FItemAndCount& Entry = Items[Index];
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.Item));
        if (ItemDefinition && Entry.Count > 0 && GetItemQuantity(ItemDefinition) <= 0)
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(this, ItemDefinition, Entry.Count, false);
        }
    }
#endif
}

void AFortPlayerController::TryAddToQuickBar(UFortWorldItem* Item)
{
#if WITH_SERVER_CODE
    if (!QuickBars || !Item)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid QuickBars '%s' or Item '%s'"), *GetNameSafe(static_cast<AFortQuickBars*>(QuickBars)), *GetNameSafe(Item));
        return;
    }

    FFortItemEntry& ItemEntry = Item->ItemEntry;
    UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
    if (!ItemDefinition)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid ItemDefinition for Item '%s'"), *GetNameSafe(Item));
        return;
    }

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    AFortQuickBars* Bars = QuickBars;
    if (Bars->ContainsItem(ItemGuid))
    {
        UE_LOG(LogFort, Log, TEXT("TryAddToQuickBar did not add item '%s' with definition '%s' because it is already on the quickbar"), *GetNameSafe(Item), *GetNameSafe(ItemDefinition));
        return;
    }

    const EFortItemType ItemType = ItemDefinition->ItemType;
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = Bars->GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            const TArray<EFortItemType>& AcceptedItemTypes = Bars->GetSlotData(Bar, SlotIndex).AcceptedItemTypes;
            if (AcceptedItemTypes.Contains(ItemType) && Bars->GetSlot(Bar, SlotIndex).Items.Num() == 0)
            {
                Bars->ServerAddItemInternal(ItemGuid, Bar, SlotIndex);
                return;
            }
        }
    }
#endif
}

void AFortPlayerController::TrySetHeroImplementation(UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
#if WITH_SERVER_CODE
    // Battle Royale plays one of the controller's DefaultHeroes (HID_Commando_Athena_*), which its GetOrInitializeHero
    // (1.7.2 0x42E5E0, called by the game mode when the player joins, 0x587C70) makes: their GE_Map_Athena_Vitality_Health
    // overrides MaxHealth and the shield cap to 100 and GE_Map_Athena_StartingShields the shield to 0. The account's Save
    // the World hero, which the MCP profile (0x5CAB40) and ServerSetHero (0x827E30) also set here, would bring its own
    // stats into the match instead. Possess sets the hero again (AFortPlayerControllerAthena::SetupHero).
    // 1.8 plays the hero of the loadout's character (#106), which its GetOrInitializeHero (0x4347F0) makes only while
    // the controller has no hero (0x2144), so a Save the World hero set first would also keep the loadout from being
    // made. The loadout is made when the pawn first spawns (AFortGameMode::RestartPlayer, 0x5965F0, under Athena's
    // 0x4044C0, before the pawn is spawned and possessed), once the athena profile the player's join requested has
    // loaded: made now, without it, the player would wear a random character for the whole match.
    AFortGameModeAthena* AthenaGameMode = Role == ROLE_Authority ? AFortGameModeAthena::GetCurrent(this) : nullptr;
    AFortPlayerControllerAthena* AthenaController = AthenaGameMode && NewHero ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)) : nullptr;
    if (AthenaController && AthenaController->PlaysBattleRoyaleHeroes() && !AthenaController->IsDefaultHero(NewHero))
    {
        UE_LOG(LogFort, Log, TEXT("TrySetHeroImplementation: %s plays a Battle Royale hero, not %s"), *GetName(), *NewHero->GetName());
        if (AthenaController->DefaultHeroes.Num() > 0)
        {
            AthenaController->GetOrInitializeHero();
        }
        return;
    }
#endif

    TrySetHeroImplementationOG(this, NewHero, bSendToServer, bUnused);

#if WITH_SERVER_CODE
    // Hero ability kit items are Save the World gadgets and kit items.
    if (!AthenaGameMode)
    {
        ApplyHeroAbilityKitItems(NewHero);
    }
#endif
}

void AFortPlayerController::ApplyHeroAbilityKitItems(UFortHero* Hero)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(static_cast<APlayerState*>(PlayerState));
    if (Role != ROLE_Authority || !Inventory || !Hero || !FortPlayerState)
    {
        return;
    }

    std::vector<UFortAbilityKit*> Kits;
    const auto AddKit = [&Kits](UFortAbilityKit* Kit)
    {
        if (Kit && std::find(Kits.begin(), Kits.end(), Kit) == Kits.end())
        {
            Kits.push_back(Kit);
        }
    };

    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState->AbilitySystemComponent;
    if (UFortHeroType* HeroType = Hero->GetHeroTypeBP(); HeroType && AbilitySystemComponent)
    {
        TStructArray<FHeroAbilityKit>& HeroTypeKits = HeroType->AbilityKits;
        for (int32_t Index = 0; Index < HeroTypeKits.Num(); ++Index)
        {
            FHeroAbilityKit& HeroTypeKit = HeroTypeKits[Index];
            if (AbilitySystemComponent->HasAllMatchingGameplayTags(HeroTypeKit.RequiredGPTags))
            {
                const TAssetPtr<UFortAbilityKit>& InherentAbilityKit = HeroTypeKit.InherentAbilityKit;
                AddKit(InherentAbilityKit.LoadSynchronous());
            }
        }
    }
    for (UFortAbilityKit* Kit : Hero->SpecializationAbilityKits)
    {
        AddKit(Kit);
    }

    std::vector<UFortItemDefinition*> Definitions;
    std::vector<UFortWorldItemDefinition*> Gadgets;
    std::vector<const FAbilityKitItem*> KitItems;
    const auto AddDefinition = [&Definitions](UFortItemDefinition* Definition)
    {
        if (!Definition || std::find(Definitions.begin(), Definitions.end(), Definition) != Definitions.end())
        {
            return false;
        }
        Definitions.push_back(Definition);
        return true;
    };
    for (UFortAbilityKit* Kit : Kits)
    {
        for (UFortGadgetItemDefinition* Gadget : KitGadgets(Kit))
        {
            if (AddDefinition(Gadget))
            {
                Gadgets.push_back(Gadget);
            }
        }

        TStructArray<FAbilityKitItem>& Items = Kit->Items;
        for (int32_t Index = 0; Index < Items.Num(); ++Index)
        {
            const FAbilityKitItem& KitItem = Items[Index];
            UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(KitItem.Item));
            if (ItemDefinition && ItemDefinition->ItemType != EFortItemType::GetWeaponHarvest() && AddDefinition(ItemDefinition))
            {
                KitItems.push_back(&KitItem);
            }
        }
    }

    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    std::vector<UFortItemDefinition*> PreviousDefinitions;
    for (const TWeakObjectPtr<UFortItemDefinition>& GrantedDefinition : Granted.GrantedDefinitions)
    {
        if (UFortItemDefinition* Definition = GrantedDefinition.Get())
        {
            PreviousDefinitions.push_back(Definition);
        }
    }

    std::vector<FGuid> StaleItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (Definition && std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end() && std::find(Definitions.begin(), Definitions.end(), Definition) == Definitions.end())
        {
            StaleItems.push_back(Entry->ItemGuid);
        }
    }
    for (const FGuid& ItemGuid : StaleItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    const auto WasGranted = [&PreviousDefinitions](UFortItemDefinition* Definition)
    {
        return std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end();
    };

    const int32_t Level = std::max(Hero->GetLevel(), 1);
    for (UFortWorldItemDefinition* Gadget : Gadgets)
    {
        if (WasGranted(Gadget) || GetItemQuantity(Gadget) > 0)
        {
            continue;
        }
        for (UFortWorldItem* GadgetItem : AddHeroAbilityKitItem(this, Inventory, Gadget, 1, Level))
        {
            FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
            Granted.PendingQuickBarItems.push_back(GadgetEntry.ItemGuid);
        }
    }

    for (const FAbilityKitItem* KitItem : KitItems)
    {
        UFortWorldItemDefinition* ItemDefinition = static_cast<UFortWorldItemDefinition*>(static_cast<UFortItemDefinition*>(KitItem->Item));
        if (WasGranted(ItemDefinition))
        {
            continue;
        }

        const int32_t Count = AbilityKitItemCount(*KitItem, GetItemQuantity(ItemDefinition));
        if (Count > 0)
        {
            AddHeroAbilityKitItem(this, Inventory, ItemDefinition, Count, Level);
        }
    }

    Granted.GrantedDefinitions.clear();
    for (UFortItemDefinition* Definition : Definitions)
    {
        Granted.GrantedDefinitions.emplace_back(Definition);
    }

    if (QuickBars && GetPawn())
    {
        AddHeroAbilityKitGadgetsToQuickBars();
    }
#endif
}

void AFortPlayerController::ApplyHarvestingTool()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortQuickBars* Bars = QuickBars;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Role != ROLE_Authority || !Inventory || !Bars || !GameData)
    {
        return;
    }

    TArray<TAssetPtr<UFortWorldItemDefinition>>& HarvestingTools = GameData->HarvestingTools;
    if (HarvestingTools.Num() == 0)
    {
        return;
    }

    int32_t Level = 0;
    if (const UFortPlayerAttrSet* PlayerAttrSet = AbilitySystemComponent ? AbilitySystemComponent->GetSet<UFortPlayerAttrSet>() : nullptr)
    {
        const FFortGameplayAttributeData& HarvestingToolAttribute = PlayerAttrSet->HarvestingTool;
        Level = std::clamp(static_cast<int32_t>(HarvestingToolAttribute.GetCurrentValue()), 0, HarvestingTools.Num() - 1);
    }
    UFortWorldItemDefinition* HarvestingTool = HarvestingTools[Level].LoadSynchronous();
    if (!HarvestingTool)
    {
        UE_LOG(LogFort, Warning, TEXT("ApplyHarvestingTool: no harvesting tool for level %d"), Level);
        return;
    }

    bool bHasHarvestingTool = false;
    std::vector<FGuid> ReplacedItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (!Definition || Definition->ItemType != EFortItemType::GetWeaponHarvest())
        {
            continue;
        }

        if (Definition == HarvestingTool)
        {
            bHasHarvestingTool = true;
        }
        else
        {
            ReplacedItems.push_back(Entry->ItemGuid);
        }
    }
    if (ReplacedItems.empty() && bHasHarvestingTool)
    {
        return;
    }

    int32_t QuickBarSlot = -1;
    FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
    TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
    for (int32_t SlotIndex = 0; SlotIndex < Slots.Num() && QuickBarSlot < 0 && !ReplacedItems.empty(); ++SlotIndex)
    {
        TArray<FGuid>& SlotItems = Slots[SlotIndex].Items;
        for (const FGuid& SlotItem : SlotItems)
        {
            if (SlotItem == ReplacedItems.front())
            {
                QuickBarSlot = SlotIndex;
                break;
            }
        }
    }

    for (const FGuid& ItemGuid : ReplacedItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    if (bHasHarvestingTool)
    {
        return;
    }

    UFortWorldItem* WorldItem = Cast<UFortWorldItem>(HarvestingTool->CreateTemporaryItemInstanceBP(1, 1));
    if (!WorldItem)
    {
        return;
    }

    WorldItem->SetOwningControllerForTemporaryItem(this);
    Inventory->AddWorldItem(WorldItem);
    FFortItemEntry& WorldItemEntry = WorldItem->ItemEntry;
    if (QuickBarSlot >= 0)
    {
        Bars->ServerAddItemInternal(WorldItemEntry.ItemGuid, EFortQuickBars::GetPrimary(), QuickBarSlot);
    }
    else
    {
        TryAddToQuickBar(WorldItem);
    }
#endif
}

void AFortPlayerController::ApplyHomebaseGadgets()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortQuickBars* Bars = QuickBars;
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    UFortMcpProfileAccount* AccountProfile = PlayerInfo ? static_cast<UFortMcpProfileAccount*>(PlayerInfo->AccountProfile) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Role != ROLE_Authority || !Inventory || !Bars || !AccountProfile || !GameData || !Finder::ResolveHomebaseGadgetsLive())
    {
        return;
    }

    const FFortSavedModeLoadout ModeLoadout = AccountProfile->GetModeLoadout(FString(L"Default"));
    const TArray<FString>& SelectedGadgets = ModeLoadout.SelectedGadgets;
    const TAssetPtr<UFortHomebaseManager>& HomebaseManagerPtr = GameData->HomebaseManager;
    UFortHomebaseManager* HomebaseManager = SelectedGadgets.Num() > 0 ? HomebaseManagerPtr.LoadSynchronous() : nullptr;
    if (!HomebaseManager)
    {
        return;
    }

    TStructArray<FFortItemEntry> SelectableGadgets;
    HomebaseManager->GetSelectableGadgetItems(AccountProfile, Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface(PlayerInfo), SelectableGadgets);
    TArray<UFortAbilityKit*> Kits;
    HomebaseManager->GetOwnedNodeAbilityKits(AccountProfile, Kits);

    std::vector<int32_t> GadgetSlots;
    const int32_t NumSlots = Bars->GetQuickBar(EFortQuickBars::GetPrimary()).Slots.Num();
    for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
    {
        const TArray<EFortItemType>& AcceptedItemTypes = Bars->GetSlotData(EFortQuickBars::GetPrimary(), SlotIndex).AcceptedItemTypes;
        if (AcceptedItemTypes.Contains(EFortItemType::GetHomebaseGadget()))
        {
            GadgetSlots.push_back(SlotIndex);
        }
    }

    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    for (int32_t Index = 0; Index < SelectedGadgets.Num(); ++Index)
    {
        const FString& Selected = SelectedGadgets[Index];
        if (Selected.IsEmpty())
        {
            continue;
        }

        const FFortItemEntry* GadgetEntry = nullptr;
        for (int32_t EntryIndex = 0; EntryIndex < SelectableGadgets.Num() && !GadgetEntry; ++EntryIndex)
        {
            const UFortItemDefinition* Definition = SelectableGadgets[EntryIndex].ItemDefinition;
            if (Definition && _wcsicmp(*Definition->GetPersistentName(), *Selected) == 0)
            {
                GadgetEntry = &SelectableGadgets[EntryIndex];
            }
        }
        UFortWorldItemDefinition* Gadget = GadgetEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(GadgetEntry->ItemDefinition)) : nullptr;
        if (!Gadget)
        {
            UE_LOG(LogFort, Warning, TEXT("ApplyHomebaseGadgets: %s selected gadget '%s' is not unlocked"), *GetName(), *Selected);
            continue;
        }

        const bool bWasGranted = std::any_of(Granted.GrantedHomebaseGadgets.begin(), Granted.GrantedHomebaseGadgets.end(), [Gadget](const TWeakObjectPtr<UFortItemDefinition>& Definition) { return Definition.Get() == Gadget; });
        if (bWasGranted || GetItemQuantity(Gadget) > 0)
        {
            continue;
        }
        Granted.GrantedHomebaseGadgets.emplace_back(Gadget);

        const int32_t Level = GadgetEntry->Level;
        const int32_t Slot = Index < static_cast<int32_t>(GadgetSlots.size()) && Bars->GetSlot(EFortQuickBars::GetPrimary(), GadgetSlots[Index]).Items.Num() == 0 ? GadgetSlots[Index] : -1;
        for (UFortWorldItem* GadgetItem : AddHeroAbilityKitItem(this, Inventory, Gadget, 1, Level))
        {
            FFortItemEntry& ItemEntry = GadgetItem->ItemEntry;
            Bars->ServerAddItemInternal(ItemEntry.ItemGuid, EFortQuickBars::GetPrimary(), Slot);
        }

        for (UFortAbilityKit* Kit : Kits)
        {
            const std::vector<UFortGadgetItemDefinition*> KitGadgetDefinitions = Kit ? KitGadgets(Kit) : std::vector<UFortGadgetItemDefinition*>{};
            if (std::none_of(KitGadgetDefinitions.begin(), KitGadgetDefinitions.end(), [Gadget](UFortGadgetItemDefinition* KitGadget) { return static_cast<UFortItemDefinition*>(KitGadget) == Gadget; }))
            {
                continue;
            }

            TStructArray<FAbilityKitItem>& Items = Kit->Items;
            for (int32_t ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
            {
                const FAbilityKitItem& KitItem = Items[ItemIndex];
                UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(KitItem.Item));
                const int32_t Count = ItemDefinition && ItemDefinition->ItemType != EFortItemType::GetWeaponHarvest() ? AbilityKitItemCount(KitItem, GetItemQuantity(ItemDefinition)) : 0;
                if (Count > 0)
                {
                    AddHeroAbilityKitItem(this, Inventory, ItemDefinition, Count, Level);
                }
            }
        }
        UE_LOG(LogFort, Log, TEXT("ApplyHomebaseGadgets: %s gets '%s' at level %d, quickbar slot %d"), *GetName(), *Selected, Level, Slot);
    }

    for (int32_t Index = 0; Index < SelectableGadgets.Num(); ++Index)
    {
        FFortItemEntry::StaticStruct()->DestroyStruct(&SelectableGadgets[Index]);
    }
    FMemory::Free(SelectableGadgets.GetData());
#endif
}

void AFortPlayerController::AddHeroAbilityKitGadgetsToQuickBars()
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    AFortInventory* Inventory = WorldInventory;
    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    const std::vector<FGuid> Pending = std::move(Granted.PendingQuickBarItems);
    Granted.PendingQuickBarItems.clear();
    for (const FGuid& ItemGuid : Pending)
    {
        UFortWorldItem* GadgetItem = Bars && Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        if (!GadgetItem || Bars->ContainsItem(ItemGuid))
        {
            continue;
        }

        FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
        UFortWorldItemDefinition* Gadget = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(GadgetEntry.ItemDefinition));
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        const int32_t PreferredSlot = Gadget ? static_cast<int32_t>(Gadget->PreferredQuickbarSlot) : -1;
        const TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
        Bars->ServerAddItemInternal(ItemGuid, EFortQuickBars::GetPrimary(), PreferredSlot >= 0 && PreferredSlot < Slots.Num() ? PreferredSlot : -1);
    }
#endif
}

void AFortPlayerController::ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    if (UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition)))
    {
        ItemDefinition->ServerExecute(WorldItem, this);
    }
#endif
}

void AFortPlayerController::ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    AFortPawn* Pawn = MyFortPawn;
    if (!WorldItem || !Pawn || Count <= 0)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (!ItemDefinition || !ItemDefinition->bCanBeDropped)
    {
        return;
    }

    const int32_t DropCount = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    WorldItem->SetDurability(ItemEntry.Durability);
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    EntryStruct->InitializeStruct(DropEntry);
    EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
    DropEntry->Count = DropCount;

    if (RemoveInventoryItem(ItemGuid, DropCount, false))
    {
        const FTransform PawnTransform = Pawn->GetTransform();
        const FVector Location = PawnTransform.GetLocation();
        if (AFortPickup* Pickup = SpawnDropPickup(*DropEntry, &Location, nullptr))
        {
            Pickup->PawnWhoDroppedPickup = Pawn;
            Pickup->TossPickup(GetDropTossLocation(PawnTransform), Pawn, 0);
        }
    }
    EntryStruct->DestroyStruct(DropEntry);
#endif
}

void AFortPlayerController::ServerSpotActor_Implementation(AActor* NewlySpottedActor)
{
#if WITH_SERVER_CODE
    UClass* const SpottableActorInterface = UFortSpottableActorInterface::StaticClass();
    if (!NewlySpottedActor || !Finder::ResolveSpottingLive(SpottableActorInterface))
    {
        return;
    }

    IFortSpottableActorInterface* Spottable = static_cast<IFortSpottableActorInterface*>(Natives::UObject_GetInterfaceAddress(NewlySpottedActor, SpottableActorInterface));
    if (!Spottable || !Spottable->CanBeSpottedBy(this))
    {
        return;
    }

    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    if (AFortTeamInfo* Team = FortPlayerState ? static_cast<AFortTeamInfo*>(FortPlayerState->PlayerTeam) : nullptr)
    {
        Team->AddSpottedActor(this, NewlySpottedActor);
    }

    AFortPawn* SpotterPawn = MyFortPawn;
    AFortPawn* SpottedPawn = Cast<AFortPawn>(NewlySpottedActor);
    if (SpotterPawn && SpottedPawn)
    {
        SpotterPawn->TriggerFeedbackEvent(SpottedPawn->SpottedEvent, SpotterPawn, SpottedPawn, 0.f);
    }
#endif
}

void AFortPlayerController::ServerRemoveInventoryStateValue_Implementation(const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    ServerRemoveInventoryStateValue_ImplementationOG(this, ItemGuid, StateType);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }

    for (AFortInventory* Inventory : { static_cast<AFortInventory*>(WorldInventory), static_cast<AFortInventory*>(OutpostInventory) })
    {
        if (UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr)
        {
            static_cast<FFortItemEntry&>(WorldItem->ItemEntry).RemoveStateValue(StateType);
            return;
        }
    }
#endif
}

AFortPickup* AFortPlayerController::SpawnDropPickup(const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation)
{
    if (Natives::AFortPlayerController_SpawnDropPickupIndex < 0 && !Finder::ResolvePlayerDropPickupLive())
    {
        return nullptr;
    }
    return CallVirtual<AFortPickup*>(this, Natives::AFortPlayerController_SpawnDropPickupIndex, &ItemEntry, SpawnLocation, SpawnRotation);
}

bool AFortPlayerController::RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    const bool bRemoved = RemoveInventoryItemOG(InventoryOwner, ItemGuid, Count, bForceRemoveFromQuickBars);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return bRemoved;
    }

    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return false;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Remaining = ItemEntry.Count - Count;
    UFortAmmoItemDefinition* AmmoDefinition = Cast<UFortAmmoItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (Count >= 0 && Remaining <= 0 && AmmoDefinition && AmmoDefinition->RegenCooldown > 0.0f)
    {
        Inventory->UpdateWorldItemCount(WorldItem, 0);
    }
    else if (Count < 0 || Remaining <= 0)
    {
        if (AFortQuickBars* Bars = QuickBars)
        {
            Bars->ServerRemoveItemInternal(ItemGuid, false, bForceRemoveFromQuickBars);
        }
        Inventory->RemoveWorldItem(WorldItem);
    }
    else
    {
        Inventory->UpdateWorldItemCount(WorldItem, Remaining);
    }
    return true;
#else
    return bRemoved;
#endif
}

bool AFortPlayerController::ModDurability(const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
#if WITH_SERVER_CODE
    if (Role == ROLE_Authority && !(DurabilityMod < 0.0f && CheatManager && bInfiniteDurability))
    {
        AFortInventory* Inventory = WorldInventory;
        UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = WorldItem ? Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP()) : nullptr;
        if (!ItemDefinition || !WorldItem->HasDurability())
        {
            return false;
        }

        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        const float MaxDurability = ItemDefinition->GetMaxDurability(WorldItem->GetLevel());
        const float NewDurability = std::clamp(bForceSet ? DurabilityMod : ItemEntry.Durability + DurabilityMod, 0.0f, std::max(MaxDurability, 0.0f));
        WorldItem->SetDurability(NewDurability);
        if (NewDurability <= 0.0f)
        {
            HandleItemZeroDurability(ItemGuid);
        }
        return true;
    }
#endif
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    return ModDurabilityOG(InventoryOwner, ItemGuid, DurabilityMod, bForceSet);
}

bool AFortPlayerController::HandleItemZeroDurability(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    const bool bEquipped = Weapon && static_cast<FGuid&>(Weapon->ItemEntryGuid) == ItemGuid;
    if (bEquipped)
    {
        Weapon->DestroyedDueToDurability();
    }
    const bool bRemoved = RemoveInventoryItem(ItemGuid, -1, true);
    if (bEquipped && Weapon->IsA(AFortWeaponRanged::StaticClass()))
    {
        GameThread::Enqueue([PlayerController = TWeakObjectPtr<AFortPlayerController>(this), BrokenWeapon = TWeakObjectPtr<AFortWeapon>(Weapon)]()
        {
            AFortPlayerController* Controller = PlayerController.Get();
            AFortWeapon* Broken = BrokenWeapon.Get();
            AFortPawn* ControllerPawn = Controller ? static_cast<AFortPawn*>(Controller->MyFortPawn) : nullptr;
            if (Broken && ControllerPawn && static_cast<AFortWeapon*>(ControllerPawn->CurrentWeapon) == Broken)
            {
                Controller->EndWeaponAbilities(Broken);
            }
        });
    }
    return bRemoved;
#else
    return false;
#endif
}

void AFortPlayerController::ServerDisassembleInventoryItem_Implementation(const FGuid& ItemGuid)
{
    DisassembleInventoryItem(ItemGuid);
}

bool AFortPlayerController::DisassembleInventoryItem(const FGuid& ItemGuid)
{
    if (!Natives::AFortPlayerController_DisassembleInventoryItem(this, ItemGuid))
    {
        return false;
    }

#if WITH_SERVER_CODE
    AFortInventory* Inventory = Role == ROLE_Authority ? static_cast<AFortInventory*>(WorldInventory) : nullptr;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    UFortWorldItemDefinition* ItemDefinition = WorldItem ? Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP()) : nullptr;
    if (!ItemDefinition)
    {
        return true;
    }

    TStructArray<FFortItemEntry> Results;
    ItemDefinition->GetDisassemblyResults(Results, WorldItem->GetNormalizedDurability());
    if (RemoveInventoryItem(ItemGuid, 1, false))
    {
        AFortPawn* Pawn = MyFortPawn;
        for (int32_t Index = 0; Index < Results.Num(); ++Index)
        {
            FFortItemEntry& Result = Results[Index];
            UFortWorldItemDefinition* ResultDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Result.ItemDefinition));
            const int32_t Count = Result.Count;
            if (!ResultDefinition || Count <= 0)
            {
                continue;
            }

            const int32_t Given = Inventory->AddItemStacks(Result, Count - Inventory->GetOverflowAmount(Result, false), -1, true, true,
                [Inventory](UFortWorldItem* Existing, int32_t Added)
                {
                    FFortItemEntry& ExistingEntry = Existing->ItemEntry;
                    Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
                    ExistingEntry.AddNewItemCount(Added, false);
                },
                [&](int32_t StackCount, bool bOverflow) -> UFortWorldItem*
                {
                    UFortWorldItem* NewItem = Cast<UFortWorldItem>(ResultDefinition->CreateTemporaryItemInstanceBP(StackCount, Result.Level));
                    if (!NewItem)
                    {
                        return nullptr;
                    }

                    FFortItemEntry& NewEntry = NewItem->ItemEntry;
                    NewEntry.inventory_overflow_date = bOverflow;
                    NewItem->SetOwningControllerForTemporaryItem(this);
                    Inventory->AddWorldItem(NewItem);
                    NewEntry.SetNewItemCount(StackCount, false);
                    if (!bOverflow)
                    {
                        TryAddToQuickBar(NewItem);
                    }
                    return NewItem;
                });

            if (Given < Count && Pawn)
            {
                Result.Count = Count - Given;
                const FVector Location = Pawn->GetTransform().GetLocation();
                if (AFortPickup* Pickup = AFortPickup::CreatePickup(GetWorld(), Result, &Location))
                {
                    Pickup->TossPickup(Location, Pawn, 0);
                }
            }
        }
    }

    for (int32_t Index = 0; Index < Results.Num(); ++Index)
    {
        FFortItemEntry::StaticStruct()->DestroyStruct(&Results[Index]);
    }
    FMemory::Free(Results.GetData());
#endif
    return true;
}

void AFortPlayerController::EndWeaponAbilities(AFortWeapon* Weapon)
{
#if WITH_SERVER_CODE
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    if (!AbilitySystemComponent || !Weapon)
    {
        return;
    }

    std::vector<TWeakObjectPtr<UGameplayAbility>> ActiveInstances;
    FGameplayAbilitySpecContainer& Container = AbilitySystemComponent->ActivatableAbilities;
    TStructArray<FGameplayAbilitySpec>& Specs = Container.Items;
    for (int32_t Index = 0; Index < Specs.Num(); ++Index)
    {
        FGameplayAbilitySpec& Spec = Specs[Index];
        UObject* SourceObject = Spec.SourceObject;
        const uint8_t ActiveCount = Spec.ActiveCount;
        if (SourceObject != Weapon || ActiveCount == 0)
        {
            continue;
        }
        TArray<UGameplayAbility*>& ReplicatedInstances = Spec.ReplicatedInstances;
        TArray<UGameplayAbility*>& NonReplicatedInstances = Spec.NonReplicatedInstances;
        for (TArray<UGameplayAbility*>* Instances : { &ReplicatedInstances, &NonReplicatedInstances })
        {
            for (UGameplayAbility* Instance : *Instances)
            {
                if (Instance)
                {
                    ActiveInstances.emplace_back(Instance);
                }
            }
        }
    }

    for (const TWeakObjectPtr<UGameplayAbility>& Instance : ActiveInstances)
    {
        if (UGameplayAbility* Ability = Instance.Get())
        {
            Ability->K2_EndAbility();
        }
    }
#endif
}

void AFortPlayerController::HookVTables()
{
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;

    const int32_t PossessIndex = Finder::ImplementationIndex(AController::StaticFunction_Possess());
    void* const PlayerControllerPossess = PlayerControllerVTable && PossessIndex >= 0 ? PlayerControllerVTable[PossessIndex] : nullptr;
    if (PlayerControllerPossess)
    {
        PossessOG = reinterpret_cast<decltype(PossessOG)>(PlayerControllerPossess);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[PossessIndex] == PlayerControllerPossess)
            {
                Hooks::SwapVTable(L"AFortPlayerController::Possess", VTable, PossessIndex, reinterpret_cast<void*>(&PossessHK));
            }
        }

        // Battle Royale's controller overrides it (1.7.2 0x441AD0: this class's, then the pawn leaves the aircraft if the
        // player is in it), so the loop above never reached it and nothing here ran on Battle Royale: not the hero's
        // attributes (#73), not the quickbar's activation (#82). Its classes get the same hook, which calls their own.
        UClass* AthenaClass = AFortPlayerControllerAthena::StaticClass();
        UObject* AthenaDefault = AthenaClass ? AthenaClass->GetDefaultObject(false) : nullptr;
        void** AthenaVTable = AthenaDefault ? *reinterpret_cast<void***>(AthenaDefault) : nullptr;
        void* const AthenaPossess = AthenaVTable ? AthenaVTable[PossessIndex] : nullptr;
        if (AthenaPossess && AthenaPossess != PlayerControllerPossess && AthenaPossess != reinterpret_cast<void*>(&PossessHK))
        {
            AthenaPossessOG = reinterpret_cast<decltype(AthenaPossessOG)>(AthenaPossess);
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
                void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
                if (VTable && VTable[PossessIndex] == AthenaPossess)
                {
                    Hooks::SwapVTable(L"AFortPlayerControllerAthena::Possess", VTable, PossessIndex, reinterpret_cast<void*>(&PossessHK));
                }
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::Possess slot not found");
    }
    if (UFunction* ActivateSlotFunction = StaticFunction_ActivateSlot(); ActivateSlotFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        ActivateSlotFunction->SetNativeFunc(&execActivateSlot);
        Log::Info(L"Hook AFortPlayerController::ActivateSlot: native");
    }
    if (UFunction* GivePlayerAmmoFunction = StaticFunction_GivePlayerAmmo(); GivePlayerAmmoFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        GivePlayerAmmoFunction->SetNativeFunc(&execGivePlayerAmmo);
        Log::Info(L"Hook AFortPlayerController::GivePlayerAmmo: native");
    }
    if (UFunction* SwapQuickBarFocusFunction = StaticFunction_SwapQuickBarFocus(); SwapQuickBarFocusFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        SwapQuickBarFocusFunction->SetNativeFunc(&execSwapQuickBarFocus);
        Log::Info(L"Hook AFortPlayerController::SwapQuickBarFocus: native");
    }

    if (Finder::ResolveInventoryOwnerLive(StaticFunction_ServerRemoveInventoryItem(), PlayerControllerDefault, Member_WorldInventory.GetOffsetChecked()))
    {
        const ptrdiff_t InterfaceOffset = Natives::AFortPlayerController_InventoryOwnerInterface;
        const int32_t RemoveIndex = Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex;
        void* const PlayerControllerRemove = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[RemoveIndex];
        RemoveInventoryItemOG = reinterpret_cast<decltype(RemoveInventoryItemOG)>(PlayerControllerRemove);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
            if (InterfaceVTable && InterfaceVTable[RemoveIndex] == PlayerControllerRemove)
            {
                Hooks::SwapVTable(L"AFortPlayerController::RemoveInventoryItem", InterfaceVTable, RemoveIndex, reinterpret_cast<void*>(&RemoveInventoryItemHK));
            }
        }

        if (ResolveDurability())
        {
            const int32_t ModIndex = Natives::IFortInventoryOwnerInterface_ModDurabilityIndex;
            void* const PlayerControllerMod = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[ModIndex];
            ModDurabilityOG = reinterpret_cast<decltype(ModDurabilityOG)>(PlayerControllerMod);
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
                void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
                if (InterfaceVTable && InterfaceVTable[ModIndex] == PlayerControllerMod)
                {
                    Hooks::SwapVTable(L"AFortPlayerController::ModDurability", InterfaceVTable, ModIndex, reinterpret_cast<void*>(&ModDurabilityHK));
                }
            }
        }
    }

    if (const int32_t DisassembleIndex = Finder::ImplementationIndex(StaticFunction_ServerDisassembleInventoryItem());
        PlayerControllerVTable && DisassembleIndex >= 0 && Finder::ResolveDisassemblyLive(StaticFunction_ServerDisassembleInventoryItem(), PlayerControllerDefault))
    {
        void* const PlayerControllerDisassemble = PlayerControllerVTable[DisassembleIndex];
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[DisassembleIndex] == PlayerControllerDisassemble)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerDisassembleInventoryItem_Implementation", VTable, DisassembleIndex, reinterpret_cast<void*>(&ServerDisassembleInventoryItem_ImplementationHK));
            }
        }
    }

    if (const int32_t StateValueIndex = Finder::ImplementationIndex(StaticFunction_ServerRemoveInventoryStateValue()); PlayerControllerVTable && StateValueIndex >= 0)
    {
        void* const PlayerControllerRemoveStateValue = PlayerControllerVTable[StateValueIndex];
        ServerRemoveInventoryStateValue_ImplementationOG = reinterpret_cast<decltype(ServerRemoveInventoryStateValue_ImplementationOG)>(PlayerControllerRemoveStateValue);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[StateValueIndex] == PlayerControllerRemoveStateValue)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerRemoveInventoryStateValue_Implementation", VTable, StateValueIndex, reinterpret_cast<void*>(&ServerRemoveInventoryStateValue_ImplementationHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerRemoveInventoryStateValue_Implementation slot not found");
    }

    if (const int32_t CheatIndex = Finder::ImplementationIndex(StaticFunction_ServerCheat());
        PlayerControllerVTable && CheatIndex >= 0 && Finder::ResolvePlayerControllerConsoleCommand(reinterpret_cast<uintptr_t>(PlayerControllerVTable[CheatIndex])))
    {
        void* const PlayerControllerCheat = PlayerControllerVTable[CheatIndex];
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[CheatIndex] == PlayerControllerCheat)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerCheat_Implementation", VTable, CheatIndex, reinterpret_cast<void*>(&ServerCheat_ImplementationHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerCheat_Implementation slot not found");
    }

    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerExecuteInventoryItem(), reinterpret_cast<void*>(&ServerExecuteInventoryItem_ImplementationHK), L"AFortPlayerController::ServerExecuteInventoryItem_Implementation" },
             std::tuple{ StaticFunction_ServerSpawnInventoryDrop(), reinterpret_cast<void*>(&ServerSpawnInventoryDrop_ImplementationHK), L"AFortPlayerController::ServerSpawnInventoryDrop_Implementation" },
             std::tuple{ StaticFunction_ServerSpotActor(), reinterpret_cast<void*>(&ServerSpotActor_ImplementationHK), L"AFortPlayerController::ServerSpotActor_Implementation" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        if (Index < 0)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }

    // Leave Match and Return To Lobby send ServerReturnToMainMenu, and the client waits for the ClientReturnToMainMenu it
    // answers with. 1.8's Battle Royale controller overrides it with the empty stub (1.8 0x4070898 slot 0x230, where 1.7.2's
    // has this class's 0x827A10), so the client got no answer and gave up only after its 30 s timeout (#79). Those classes
    // get this class's, which answers.
    if (const int32_t ReturnIndex = Finder::ImplementationIndex(StaticFunction_ServerReturnToMainMenu());
        PlayerControllerVTable && ReturnIndex >= 0 && PlayerControllerVTable[ReturnIndex] != Natives::SharedEmptyFunction)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[ReturnIndex] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerReturnToMainMenu_Implementation", VTable, ReturnIndex, PlayerControllerVTable[ReturnIndex]);
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerReturnToMainMenu_Implementation slot not found");
    }

    HookBuildingVTables();
    HookEmoteVTables();
}

int32_t AFortPlayerController::GetItemQuantity(const UFortItemDefinition* ItemDefinition)
{
    AFortInventory* Inventory = WorldInventory;
    if (!Inventory || !ItemDefinition)
    {
        return 0;
    }

    int32_t Quantity = 0;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry& Entry = Instances[Index]->ItemEntry;
        if (static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == ItemDefinition)
        {
            Quantity += Entry.Count;
        }
    }
    return Quantity;
}

ptrdiff_t AFortPlayerController::CriticalMatchBonusOffset()
{
    return Member_bShouldReceiveCriticalMatchBonus.GetOffsetChecked();
}

ptrdiff_t AFortPlayerController::QuickBarsOffset()
{
    return Member_QuickBars.GetOffsetChecked();
}

bool AFortPlayerController::ResolveDurability()
{
    return Finder::ResolveDurabilityLive(StaticFunction_ServerModDurability(), StaticClass()->GetDefaultObject(false), FFortItemEntry::DurabilityOffset(),
        AFortWeapon::PendingDestroyDueToDurabilityOffset(), AFortWeapon::PendingDestroyDueToDurabilityMask());
}

UFortQuestManager* AFortPlayerController::GetQuestManager() const
{
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    return PlayerInfo ? PlayerInfo->GetCampaignQuestManager() : nullptr;
}

UFortSchematicItem* AFortPlayerController::GetSchematicForItem(const UFortItemDefinition* ItemDefinition)
{
    if (!Natives::AFortPlayerController_GetSchematicForItem && !Finder::ResolveCraftingLive())
    {
        return nullptr;
    }
    return Natives::AFortPlayerController_GetSchematicForItem(this, ItemDefinition);
}

void AFortPlayerController::ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    if (AFortQuickBars* Bars = QuickBars)
    {
        Bars->ServerActivateSlotInternal(InQuickBar, Slot, ActivateDelay, bUpdatePreviousFocusedSlot);
    }
}

DEFINE_FUNCTION(AFortPlayerController::execActivateSlot)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_GET_PROPERTY(UIntProperty, Z_Param_Slot);
    P_GET_PROPERTY(UFloatProperty, Z_Param_ActivateDelay);
    P_GET_UBOOL(Z_Param_bUpdatePreviousFocusedSlot);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->ActivateSlot(EFortQuickBars{ Z_Param_InQuickBar }, Z_Param_Slot, Z_Param_ActivateDelay, Z_Param_bUpdatePreviousFocusedSlot);
    P_NATIVE_END;
}

void AFortPlayerController::GivePlayerAmmo(int32_t AmmoCount)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Role == ROLE_Authority && Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    if (!Weapon || AmmoCount <= 0)
    {
        return;
    }

    const int32_t BulletsPerClip = Weapon->GetBulletsPerClip();
    const int32_t Refunded = std::min(AmmoCount, BulletsPerClip - static_cast<int32_t>(Weapon->AmmoCount));
    if (Refunded > 0)
    {
        Weapon->ModAmmo(Refunded);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerController::execGivePlayerAmmo)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_AmmoCount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->GivePlayerAmmo(Z_Param_AmmoCount);
    P_NATIVE_END;
}

void AFortPlayerController::SwapQuickBarFocus(EFortQuickBars InQuickBar)
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    if (Bars && Bars->GetCurrentQuickBar() != InQuickBar)
    {
        Bars->SwapQuickBarFocus(InQuickBar, -1);
    }
#endif
}

void AFortPlayerController::ClientReportDamagedResourceBuilding(ABuildingSMActor* BuildingSMActor, uint8_t PotentialResourceType, int32_t PotentialResourceCount, bool bDestroyed, bool bJustHitWeakspot)
{
    // 1.2 has no thunk for this client RPC left; send it through ProcessEvent.
    Reflection::FParams Params(StaticFunction_ClientReportDamagedResourceBuilding());
    Params.Set(L"BuildingSMActor", BuildingSMActor);
    Params.Set(L"PotentialResourceType", PotentialResourceType);
    Params.Set(L"PotentialResourceCount", PotentialResourceCount);
    Params.Set(L"bDestroyed", bDestroyed);
    Params.Set(L"bJustHitWeakspot", bJustHitWeakspot);
    Params.Call(this);
}

DEFINE_FUNCTION(AFortPlayerController::execSwapQuickBarFocus)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->SwapQuickBarFocus(EFortQuickBars{ Z_Param_InQuickBar });
    P_NATIVE_END;
}

void AFortPlayerController::Cheat(const FString& Msg)
{
    // The native Cheat (Cert 0x740C20, 1.2 0x140737A00, 1.7.2 0x7F7C40) sends the command through the matchmaking lobby
    // beacon whenever the client still has one (UFortGameInstance::Matchmaking->LobbyBeaconClient), which a Save the
    // World player keeps for the whole match. The server's ALobbyBeaconClient::ServerCheat_Implementation is empty in
    // these builds, so every "cheat" was dropped without a word (#75). It goes to this controller's ServerCheat instead.
    //
    // "fly 3000" (#129): this game predicts the pawn's movement, so it moves at the speed the server will. Nothing the
    // server could replicate holds the speed, so the speed this player sends waits here for the server's ClientCheatFly
    // or ClientCheatGhost (UFortCheatManager::OnCheatMovement). The server refuses a speed for another player
    // ("/Name/"), so none is kept for one.
    //
    // "disguiseview" (#137) is how this player sees their own disguise, which only their game knows: it stays here. So do
    // "togglebloom" and "togglerecoil" (#152), the spread and recoil of this player's shots, which only their game
    // simulates, and "copytargetpath" (#152), which puts what they look at on their clipboard.
    if (UFortCheatManager::RunDisguiseViewCommand(this, Msg) || UFortCheatManager::RunClientCheatCommand(this, Msg))
    {
        return;
    }
    FString Command;
    FString TargetName;
    FString MovementCommand;
    FString MovementError;
    float MoveSpeed = 0.0f;
    const bool bTargeted = UFortCheatManager::SplitCheatTarget(Msg, Command, TargetName);
    if (UFortCheatManager::ParseCheatMovement(bTargeted ? Command : Msg, MovementCommand, MoveSpeed, MovementError))
    {
        UFortCheatManager::CheatMoveSpeed = bTargeted || !MovementError.IsEmpty() ? 0.0f : MoveSpeed;
    }
    ServerCheat(Msg);
}

DEFINE_FUNCTION(AFortPlayerController::execCheat)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_Msg);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->Cheat(Z_Param_Msg);
    P_NATIVE_END;
}

void AFortPlayerController::ServerCheat(const FString& Msg)
{
    Reflection::FParams Params(StaticFunction_ServerCheat());
    Params.Set(L"Msg", Msg);
    Params.Call(this);
}

void AFortPlayerController::ServerCheat_Implementation(const FString& Msg)
{
#if WITH_SERVER_CODE
    // Who may cheat is the backend's (CoreV2 #138): its admin panel lets every player, only the accounts it lists, or
    // nobody. It is asked for the player who typed the command, so "<command> /Name/" on another player needs the one
    // who typed it to be allowed. A player who may not is told so and nothing runs, Core's own commands included. A
    // backend from before it (404) leaves cheats on for every player, as they were.
    if (bCheatPermissionsUnsupported)
    {
        RunServerCheat(Msg);
        return;
    }
    APlayerState* const State = PlayerState;
    std::wstring AccountId = State ? State->UniqueId.ToString().ToStdString() : std::wstring();
    if (!IsCheatAccountId(AccountId))
    {
        AccountId.clear();
    }
    const std::wstring DisplayName = State ? State->GetPlayerName().ToStdString() : std::wstring();
    std::wstring Key = AccountId;
    if (Key.empty())
    {
        Key = L"name:" + DisplayName;
        std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Character) { return static_cast<wchar_t>(std::towlower(Character)); });
    }

    FCheatPermission& Permission = CheatPermissions[Key];
    if (Permission.bKnown && !Permission.bAsking && std::chrono::steady_clock::now() < Permission.Expires)
    {
        if (!Permission.bAllowed)
        {
            UE_LOG(LogFort, Log, TEXT("ServerCheat: %s may not cheat, \"%s\" not run"), *GetNameSafe(this), *Msg);
            ClientMessage(CheatsRefused(Permission.Mode));
            return;
        }
        RunServerCheat(Msg);
        return;
    }
    if (Permission.Waiting.size() >= MaxWaitingCheats)
    {
        ClientMessage(FString(L"Still asking the backend whether you may cheat: try again in a moment"));
        return;
    }
    Permission.Waiting.emplace_back(TWeakObjectPtr<AFortPlayerController>(this), Msg.ToStdString());
    if (!Permission.bAsking)
    {
        AskCheatPermission(Key, AccountId, DisplayName);
    }
#endif
}

void AFortPlayerController::RunServerCheat(const FString& Msg)
{
#if WITH_SERVER_CODE
    // Cheats are on for every player the backend allows (#65, #69, #138); a controller that came another way gets its
    // cheat manager here.
    if (!CheatManager)
    {
        AddCheats(true);
    }
    UE_LOG(LogFort, Log, TEXT("ServerCheat: %s runs \"%s\" (cheat manager %s)"), *GetNameSafe(this), *Msg, *GetNameSafe(static_cast<UObject*>(CheatManager)));

    // "<command> /Name/" runs the command as another player (#122): their controller's cheat manager runs it, so it gives
    // them the items, moves their pawn, and so on. The token comes off before the command parses its arguments.
    FString Command = Msg;
    FString TargetName;
    AFortPlayerController* Runner = this;
    if (UFortCheatManager::SplitCheatTarget(Msg, Command, TargetName))
    {
        FString Error;
        Runner = UFortCheatManager::FindCheatTarget(this, TargetName, Error);
        if (!Runner)
        {
            ClientMessage(Error);
            return;
        }
        if (!Runner->CheatManager)
        {
            Runner->AddCheats(true);
        }
    }

    // Core's own command lists the cheats it supports (#101).
    const std::wstring Line = Command.ToStdString();
    const size_t NameEnd = Line.find_first_of(L" \t");
    const std::wstring Name = Line.substr(0, NameEnd);
    if (Name.size() == 10 && _wcsicmp(Name.c_str(), L"corecheats") == 0)
    {
        if (UFortCheatManager* FortCheatManager = Cast<UFortCheatManager>(static_cast<UObject*>(CheatManager)))
        {
            FortCheatManager->ListCoreCheats(FString(NameEnd == std::wstring::npos ? L"" : Line.c_str() + NameEnd));
        }
        return;
    }

    // Core's own commands, which no exec function of these builds has, run on the player they run on:
    // "spawnactor <class or path> [scale | x y z]" (#132) spawns an actor in front of them at a scale, "skin <HID or CID>"
    // (or "hero"; #134, #136) gives them a hero's look, "disguise <class> [scale]" and "undisguise" (#134, #136) turn them
    // into a prop or a pawn, "tpactor <class, name or path> [n]" (#146) puts them on top of the nearest actor that matches,
    // "nocooldown [on | off]" (#148; also for the game's ToggleNoCoolDown, which sets a flag nothing reads) ends their
    // ability and gadget cooldowns, "infinitemag [on | off]" (#149; also for ToggleInfiniteAmmo, whose flag it sets) keeps
    // their magazine full, "repair [all]" (#150) restores their weapon's durability, "firerate <multiplier | reset>"
    // (#150) changes how fast their guns fire, "weapondamage <multiplier | reset>" (#152) how much damage their weapons
    // do, and "dance [EID]" (#152) plays an emote.
    using FCoreCommand = void (UFortCheatManager::*)(const FString&);
    static constexpr std::pair<const wchar_t*, FCoreCommand> CoreCommands[] = {
        { L"spawnactor", &UFortCheatManager::SpawnActor },
        { L"tpactor", &UFortCheatManager::TpActor },
        { L"nocooldown", &UFortCheatManager::NoCooldown },
        { L"togglenocooldown", &UFortCheatManager::NoCooldown },
        { L"infinitemag", &UFortCheatManager::InfiniteMag },
        { L"toggleinfiniteammo", &UFortCheatManager::InfiniteMag },
        { L"repair", &UFortCheatManager::Repair },
        { L"firerate", &UFortCheatManager::FireRate },
        { L"weapondamage", &UFortCheatManager::WeaponDamage },
        { L"dance", &UFortCheatManager::Dance },
        { L"skin", &UFortCheatManager::Skin },
        { L"hero", &UFortCheatManager::Skin },
        { L"disguise", &UFortCheatManager::Disguise },
        { L"undisguise", &UFortCheatManager::Undisguise },
    };
    for (const auto& [CommandName, Run] : CoreCommands)
    {
        if (_wcsicmp(Name.c_str(), CommandName) != 0)
        {
            continue;
        }
        if (UFortCheatManager* FortCheatManager = Cast<UFortCheatManager>(static_cast<UObject*>(Runner->CheatManager)))
        {
            AFortPlayerController* const PreviousReplyTo = UFortCheatManager::ReplyTo;
            if (Runner != this)
            {
                UFortCheatManager::ReplyTo = this;
            }
            (FortCheatManager->*Run)(FString(NameEnd == std::wstring::npos ? L"" : Line.c_str() + NameEnd));
            UFortCheatManager::ReplyTo = PreviousReplyTo;
        }
        else
        {
            ClientMessage(FString::Printf(TEXT("%s needs Fortnite's cheat manager"), CommandName));
        }
        return;
    }

    // "fly 3000" and "ghost 3000" (#129): the speed comes off before the console parses the command, and the pawn's Fly
    // or Ghost body moves it at that speed (UFortCheatManager::OnCheatMovement). The pawn's own client predicts its
    // movement at the speed its player typed, so a speed only goes with the player's own pawn.
    FString MovementCommand;
    FString MovementError;
    float MoveSpeed = 0.0f;
    if (UFortCheatManager::ParseCheatMovement(Command, MovementCommand, MoveSpeed, MovementError))
    {
        if (!MovementError.IsEmpty())
        {
            ClientMessage(MovementError);
            return;
        }
        if (MoveSpeed > 0.0f && !TargetName.IsEmpty())
        {
            ClientMessage(FString::Printf(TEXT("A speed only works on your own pawn: %s's game wouldn't know it (\"cheat %s /%s/\" works at their own speed)"),
                *TargetName, *MovementCommand, *TargetName));
            return;
        }
        Command = MovementCommand;
    }

    // The native body runs ConsoleCommand(Msg, true), which writes the output to the server's log and replies with an
    // empty ClientMessage. Collecting it instead sends the player what the command printed, "Command not recognized"
    // included; the cheats' own replies ("God mode on") are ClientMessages of their own. Core's cheats answer the player
    // who typed the command when it runs on someone else; the game's own go to the player it ran on.
    AFortPlayerController* const PreviousReplyTo = UFortCheatManager::ReplyTo;
    if (Runner != this)
    {
        UFortCheatManager::ReplyTo = this;
    }
    UFortCheatManager::CheatMoveSpeed = MoveSpeed;
    FString Result = Runner->ConsoleCommand(Command, false);
    UFortCheatManager::CheatMoveSpeed = 0.0f;
    UFortCheatManager::ReplyTo = PreviousReplyTo;
    while (!Result.IsEmpty() && (Result.EndsWith(L"\n") || Result.EndsWith(L"\r")))
    {
        Result = Result.Left(Result.Len() - 1);
    }
    if (!Result.IsEmpty())
    {
        UE_LOG(LogFort, Log, TEXT("ServerCheat: %s"), *Result);
        ClientMessage(Result);
    }
    if (Runner != this)
    {
        APlayerState* TargetState = Runner->PlayerState;
        const FString RunnerName = TargetState ? TargetState->GetPlayerName() : GetNameSafe(Runner);
        UE_LOG(LogFort, Log, TEXT("ServerCheat: %s ran \"%s\" on %s"), *GetNameSafe(this), *Command, *RunnerName);
        ClientMessage(FString::Printf(TEXT("Ran \"%s\" on %s"), *Command, *RunnerName));
    }
#endif
}

void AFortPlayerController::OnRep_CheatMovement()
{
    // The native notify (1.7.2 0x81B850) runs this game's own cheat manager's Ghost, Fly or Walk, which answers "You feel
    // much lighter" a second time (the server's already did) and only calls the pawn's ClientCheat* again, which the
    // server has already sent. It moves the pawn the same way, without the reply (#100).
    if (ACharacter* const Character = Cast<ACharacter>(GetPawn()))
    {
        Character->ApplyCheatMovement(bCheatGhost ? ECheatMovement::Ghost : bCheatFly ? ECheatMovement::Fly : ECheatMovement::Walk);
    }
}

DEFINE_FUNCTION(AFortPlayerController::execOnRep_CheatMovement)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->OnRep_CheatMovement();
    P_NATIVE_END;
}

AFortPlayerController* AFortPlayerController::FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner)
{
    if (!InventoryOwner || Natives::AFortPlayerController_InventoryOwnerInterface < 0)
    {
        return nullptr;
    }
    return reinterpret_cast<AFortPlayerController*>(reinterpret_cast<uint8_t*>(InventoryOwner) - Natives::AFortPlayerController_InventoryOwnerInterface);
}

bool AFortPlayerController::RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->RemoveInventoryItem(ItemGuid, Count, bForceRemoveFromQuickBars);
}

bool AFortPlayerController::ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->ModDurability(ItemGuid, DurabilityMod, bForceSet);
}

void AFortPlayerController::ServerDisassembleInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerDisassembleInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::PossessHK(AFortPlayerController* This, APawn* InPawn)
{
    This->Possess(InPawn);
}

void AFortPlayerController::TrySetHeroImplementationHK(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
    This->TrySetHeroImplementation(NewHero, bSendToServer, bUnused);
}

void AFortPlayerController::ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerExecuteInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor)
{
    This->ServerSpotActor_Implementation(NewlySpottedActor);
}

void AFortPlayerController::ServerCheat_ImplementationHK(AFortPlayerController* This, const FString& Msg)
{
    This->ServerCheat_Implementation(Msg);
}

void AFortPlayerController::ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    This->ServerRemoveInventoryStateValue_Implementation(ItemGuid, StateType);
}

void AFortPlayerController::ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerSpawnInventoryDrop_Implementation(ItemGuid, Count);
}

void AFortPlayerController::HookClientNatives()
{
    if (UFunction* CheatFunction = StaticFunction_Cheat(); CheatFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        CheatFunction->SetNativeFunc(&execCheat);
        Log::Info(L"Hook AFortPlayerController::Cheat: native");
    }
    if (UFunction* CheatMovementFunction = StaticFunction_OnRep_CheatMovement(); CheatMovementFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        CheatMovementFunction->SetNativeFunc(&execOnRep_CheatMovement);
        Log::Info(L"Hook AFortPlayerController::OnRep_CheatMovement: native");
    }
}

void AFortPlayerController::Hook()
{
    Hooks::Detour(L"AFortPlayerController::InitializeMcpProfile", reinterpret_cast<void*>(Natives::AFortPlayerController_InitializeMcpProfile), &InitializeMcpProfileHK, &InitializeMcpProfileOG);
    Hooks::Detour(L"AFortPlayerController::TrySetHeroImplementation", reinterpret_cast<void*>(Natives::AFortPlayerController_TrySetHeroImplementation), &TrySetHeroImplementationHK, &TrySetHeroImplementationOG);
}

bool AFortPlayerController::InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete)
{
    return This->InitializeMcpProfile(OnComplete);
}
