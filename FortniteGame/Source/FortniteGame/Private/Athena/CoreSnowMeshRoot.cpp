#include "pch.h"
#include "Athena/CoreSnowMeshRoot.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "BackendRequest.h"
#include "Containers/Array.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameThread.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "MeshNetworkSubsystem.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Packed.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    const wchar_t* const SnowScriptingClassPath = L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C";
    const wchar_t* const PillarsClassPath = L"/Game/Athena/Prototype/Blueprints/Dopey/BP_DirtShell_Pillars.BP_DirtShell_Pillars_C";
    const wchar_t* const DirtShellPropClassPath = L"/Game/Athena/Prototype/Blueprints/Dopey/BP_DirtShellProp.BP_DirtShellProp_C";
    const wchar_t* const PillarDamageHandlerName = L"CoreRootPillarDamage";
    const wchar_t* const Next2ShownHandlerName = L"CoreHandleSnowNext2Shown";
    const wchar_t* const VoteConcludedHandlerName = L"CoreReportUnvaultedItem";
    // Core-Backend's (src/services/fortnite/unvaulting.js): the winner every 8.51 match takes after the event.
    const wchar_t* const UnvaultingPath = L"/fortnite/api/game/v2/unvaulting";

    // E_SnowStates::Complete (/Game/Athena/Prototype/Blueprints/White/E_SnowStates: Uninitialized, Disabled, WarmingUp,
    // Active, Complete).
    constexpr uint8_t SnowStateComplete = 4;

    TWeakObjectPtr<UObject> SnowScripting;

    // A Blueprint member of Object by name, when the build's class has it with this size.
    template <typename T>
    T* BlueprintMember(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property || static_cast<size_t>(Property->ElementSize) != sizeof(T))
        {
            return nullptr;
        }
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(Object) + Property->GetOffset_ForInternal());
    }

    bool CallBlueprintFunction(UObject* Object, const wchar_t* Name)
    {
        Reflection::FParams Params(Object ? Object->FindFunction(FName(Name, FNAME_Find)) : nullptr, true);
        if (!Params.Call(Object))
        {
            Log::Warning(L"Unvaulting: {} has no {}", Object ? Object->GetName().ToStdString() : std::wstring(L"(none)"), Name);
            return false;
        }
        return true;
    }

    const UFunction* DelegateSignature(UClass* Class, const wchar_t* PropertyName)
    {
        const UProperty* Property = Class ? Class->FindPropertyByName(FName(PropertyName, FNAME_Find)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
    }

    ULevelStreaming* FindStreamingLevel(UWorld* World, const wchar_t* ShortName)
    {
        for (ULevelStreaming* Streaming : static_cast<TArray<ULevelStreaming*>&>(World->StreamingLevels))
        {
            const FString Package = Streaming ? Streaming->GetWorldAssetPackageFName().ToString() : FString();
            if (!Package.IsEmpty() && std::wstring(*Package).ends_with(std::wstring(L"/") + ShortName))
            {
                return Streaming;
            }
        }
        return nullptr;
    }
}

void FCoreSnowMeshRoot::SetUp(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    // 8.50 ships an earlier BP_SnowScripting without the event (no ResetUTCTime, no RootPillarsConcluded): only 8.51's runs it.
    UClass* SnowClass = GameMode ? FindObject<UClass>(nullptr, SnowScriptingClassPath) : nullptr;
    if (!SnowClass || !SnowClass->FindFunctionByName(FName(L"ResetUTCTime", FNAME_Find)) || !SnowClass->FindFunctionByName(FName(L"RootPillarsConcluded", FNAME_Find)))
    {
        return;
    }
    // BindCalendarEvents skips these playlists itself (IsCreativeOrPlaygroundModePlaylist); their other mesh Blueprints
    // stay as they are.
    if (AFortPlayerControllerAthena::IsCreativeMatch(GameMode))
    {
        return;
    }
    const TArray<AActor*> Scriptings = UGameplayStatics::GetAllActorsOfClass(GameMode, SnowClass);
    UObject* Snow = Scriptings.Num() > 0 ? reinterpret_cast<UObject*>(Scriptings[0]) : nullptr;
    UMeshNetworkSubsystem* Mesh = Snow ? UMeshNetworkSubsystem::Get(GameMode) : nullptr;
    if (!Snow || !Mesh || !UMeshNetworkSubsystem::HasNodeType())
    {
        Log::Warning(L"Unvaulting: {}, so the event doesn't run", !Snow ? L"no BP_SnowScripting in the world" : L"no mesh network subsystem");
        return;
    }
    if (Mesh->GetMeshNetworkNodeType() != EMeshNetworkNodeType::Unknown)
    {
        Log::Info(L"Unvaulting: the server already is a mesh node ({}), so Core doesn't stand in for the root", static_cast<int32_t>(Mesh->NodeType));
        return;
    }
    SnowScripting = Snow;

    // What the root had replicated by the time a match joined, before the edge's Blueprint looks (its node type, below,
    // starts BindCalendarEvents): the five challenges complete, and the pillars not voted yet (ResetUTCTime, the
    // Blueprint's own: PillarsCompletedUTCTime = DefaultMetadataUTCTime and onPillarsCompletedSet).
    static_cast<AActor*>(reinterpret_cast<UObject*>(Snow))->FlushNetDormancy();
    bool* AllChallengesCompleted = BlueprintMember<bool>(Snow, L"AllChallengesCompleted");
    TArray<uint8_t>* ChallengeStates = BlueprintMember<TArray<uint8_t>>(Snow, L"ChallengeStates");
    if (AllChallengesCompleted)
    {
        *AllChallengesCompleted = true;
        CallBlueprintFunction(Snow, L"OnRep_AllChallengesCompleted");
    }
    if (ChallengeStates)
    {
        for (int32_t Index = 0; Index < ChallengeStates->Num(); ++Index)
        {
            (*ChallengeStates)[Index] = SnowStateComplete;
        }
    }
    const bool bReset = CallBlueprintFunction(Snow, L"ResetUTCTime");

    // The edge's own node, as AFortGameSession::SetMeshNodeType and its beacon's connection leave it, and the clients' copy.
    Mesh->SetConnectedToRoot(true);
    Mesh->SetMeshNetworkNodeType(EMeshNetworkNodeType::Edge);
    AFortGameStateAthena* GameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(GameMode->GetWorld()->GetGameState()));
    if (GameState && AFortGameStateAthena::HasMeshNetworkStatus())
    {
        FMeshNetworkStatus& Status = GameState->MeshNetworkStatus;
        Status.bEnabled = true;
        Status.bConnectedToRoot = true;
        Status.GameServerNodeType = static_cast<uint8_t>(EMeshNetworkNodeType::Edge);
    }
    // On an edge it hands the states to the pedestals.
    if (ChallengeStates)
    {
        CallBlueprintFunction(Snow, L"OnRep_ChallengeStates");
    }

    // The root's pillar bindings, once Next2 (the pillars' level, streamed for the vote) is in.
    UClass* PillarsClass = FindObject<UClass>(nullptr, PillarsClassPath);
    UClass* PropClass = FindObject<UClass>(nullptr, DirtShellPropClassPath);
    const UFunction* DamageSignature = DelegateSignature(PropClass, L"onDamageRecieved");
    UFunction* DamageHandler = PillarsClass && DamageSignature ? PillarsClass->AddNativeFunction(PillarDamageHandlerName, &execHandlePillarDamage, DamageSignature) : nullptr;
    ULevelStreaming* Next2 = FindStreamingLevel(GameMode->GetWorld(), L"Next2");
    UFunction* ShownHandler = reinterpret_cast<UObject*>(GameMode)->FindFunction(FName(Next2ShownHandlerName, FNAME_Find));
    if (Next2 && ShownHandler)
    {
        Next2->OnLevelShown.AddUnique(FScriptDelegate(reinterpret_cast<UObject*>(GameMode), ShownHandler->GetFName()));
    }
    const UFunction* ConcludedSignature = DelegateSignature(PillarsClass, L"onPillarsConcludedParent");
    if (PillarsClass && ConcludedSignature)
    {
        PillarsClass->AddNativeFunction(VoteConcludedHandlerName, &execHandleVoteConcluded, ConcludedSignature);
    }
    BindPillars(GameMode->GetWorld());
    AskUnvaultedItem();
    Log::Info(L"Unvaulting: Core stands in for the mesh root: the server is an Edge node connected to it, the challenges complete{}, the pillars {} (ResetUTCTime), "
        L"the vote {}", AllChallengesCompleted ? L"" : L" (no AllChallengesCompleted)", bReset ? L"not voted yet" : L"UNSET",
        DamageHandler && Next2 && ShownHandler ? L"counted as the root counts it once Next2 is in" : L"only by the Blueprint's 150 s AutoCompleteVote (no Next2 or no pillar classes)");
#endif
}

void FCoreSnowMeshRoot::AddNatives()
{
    UClass* AthenaClass = AFortGameModeAthena::StaticClass();
    if (AthenaClass)
    {
        AthenaClass->AddNativeFunction(Next2ShownHandlerName, &execHandleNext2Shown);
    }
}

void FCoreSnowMeshRoot::BindPillars(UWorld* World)
{
#if WITH_SERVER_CODE
    UObject* Snow = SnowScripting.Get();
    UClass* PillarsClass = Snow && World ? FindObject<UClass>(nullptr, PillarsClassPath) : nullptr;
    UFunction* RootConcluded = Snow ? Snow->FindFunction(FName(L"RootPillarsConcluded", FNAME_Find)) : nullptr;
    if (!PillarsClass)
    {
        return;
    }
    const FName DamageHandler(PillarDamageHandlerName, FNAME_Find);
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(World, PillarsClass))
    {
        UObject* Pillars = reinterpret_cast<UObject*>(Actor);
        TArray<UObject*>* Props = Pillars ? BlueprintMember<TArray<UObject*>>(Pillars, L"DirtShellProps") : nullptr;
        int32_t Bound = 0;
        if (Props && !DamageHandler.IsNone() && Pillars->FindFunction(DamageHandler))
        {
            for (UObject* Prop : *Props)
            {
                if (FMulticastScriptDelegate* Damaged = Prop ? BlueprintMember<FMulticastScriptDelegate>(Prop, L"onDamageRecieved") : nullptr)
                {
                    Damaged->AddUnique(FScriptDelegate(Pillars, DamageHandler));
                    ++Bound;
                }
            }
        }
        FMulticastScriptDelegate* Concluded = BlueprintMember<FMulticastScriptDelegate>(Pillars, L"onPillarsConcludedParent");
        if (Concluded && RootConcluded)
        {
            Concluded->AddUnique(FScriptDelegate(Snow, RootConcluded->GetFName()));
        }
        const FName ReportHandler(VoteConcludedHandlerName, FNAME_Find);
        if (Concluded && !ReportHandler.IsNone() && Pillars->FindFunction(ReportHandler))
        {
            Concluded->AddUnique(FScriptDelegate(Pillars, ReportHandler));
        }
        Log::Info(L"Unvaulting: {}: the root counts the hits on {} of its {} pillars, and {}", Pillars->GetName().ToStdString(), Bound, Props ? Props->Num() : 0,
            Concluded && RootConcluded ? L"its vote sets the unvaulted item (RootPillarsConcluded)" : L"its vote sets no unvaulted item (no RootPillarsConcluded)");
    }
#endif
}

void FCoreSnowMeshRoot::HandlePillarDamage(UObject* Pillars, int32_t Damage, UObject* DirtProp)
{
#if WITH_SERVER_CODE
    // The edge's PropDamageRecieved sends trunc(Damage * DamageMultiplier) as DamageProp<index>; the root's
    // onDamageAdded hands it to UpdateHealth.
    TArray<UObject*>* Props = Pillars ? BlueprintMember<TArray<UObject*>>(Pillars, L"DirtShellProps") : nullptr;
    int32_t Index = -1;
    for (int32_t Each = 0; Props && DirtProp && Each < Props->Num() && Index < 0; ++Each)
    {
        Index = (*Props)[Each] == DirtProp ? Each : -1;
    }
    if (Index < 0)
    {
        return;
    }
    const float* Multiplier = BlueprintMember<float>(Pillars, L"DamageMultiplier");
    const int32_t Applied = static_cast<int32_t>(static_cast<float>(Damage) * (Multiplier ? *Multiplier : 1.0f));
    Reflection::FParams Params(Pillars->FindFunction(FName(L"UpdateHealth", FNAME_Find)), true);
    if (Params.Set(L"Damage", Applied) && Params.Set(L"DirtIndex", static_cast<uint8_t>(Index)))
    {
        Params.Call(Pillars);
    }
#endif
}

void FCoreSnowMeshRoot::AskUnvaultedItem()
{
#if WITH_SERVER_CODE
    // The root kept the vote's winner in the mesh's metadata (key SnowScripting, UnvaultName) for the rest of the season:
    // a match after the event had it from the start. Core-Backend keeps the first Core server's (ReportUnvaultedItem)
    // and answers it, or Epic's Drum Gun until one reports, only after the event; during it each match votes itself.
    // Core's own requests carry no build in their user agent: the backend is told it.
    BackendRequest::GetJson(L"Unvaulting item", std::wstring(UnvaultingPath) + L"?build=" + std::to_wstring(Packed::Changelist()), FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret,
        [](uint32_t Status, const std::string& Body)
    {
        const std::string Item = Status == 200 ? BackendRequest::JsonField(Body, "item") : std::string();
        const std::string Source = Status == 200 ? BackendRequest::JsonField(Body, "source") : std::string();
        GameThread::Enqueue([Status, Item, Source]()
        {
            UObject* Snow = SnowScripting.Get();
            if (!Snow || Item.empty() || Item == "null")
            {
                Log::Info(L"Unvaulting: {}", Status == 200 ? L"nothing unvaulted yet (before or during the event)" : L"the backend has no unvaulted item for this match");
                return;
            }
            // The Blueprint's own (SetUnvaultItemName): UnvaultedItemName, whose OnRep on an edge (SetLootWeights) puts the
            // item's GW.* weight on the event mutator for the match's loot rolls.
            Reflection::FParams Params(Snow->FindFunction(FName(L"SetUnvaultItemName", FNAME_Find)), true);
            const std::wstring Name(Item.begin(), Item.end());
            if (Params.Set(L"Name", FName(Name.c_str())) && Params.Call(Snow))
            {
                Log::Info(L"Unvaulting: the island has unvaulted {} ({})", Name, std::wstring(Source.begin(), Source.end()));
            }
        });
    }, true);
#endif
}

void FCoreSnowMeshRoot::ReportUnvaultedItem(FName UnvaultedItem)
{
#if WITH_SERVER_CODE
    static bool bReported = false;
    if (bReported || UnvaultedItem.IsNone())
    {
        return;
    }
    bReported = true;
    const std::wstring Name = UnvaultedItem.ToString().ToStdString();
    Log::Info(L"Unvaulting: the vote unvaulted {}, reported for the matches after the event", Name);
    BackendRequest::PostJson(L"Unvaulting vote", UnvaultingPath, "{\"item\":\"" + std::string(Name.begin(), Name.end()) + "\",\"build\":" + std::to_string(Packed::Changelist()) + "}",
        FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret);
#endif
}

DEFINE_FUNCTION(FCoreSnowMeshRoot::execHandleVoteConcluded)
{
    P_GET_PROPERTY(UNameProperty, UnvaultedItem);
    P_FINISH;
    P_NATIVE_BEGIN;
    ReportUnvaultedItem(UnvaultedItem);
    P_NATIVE_END;
}

DEFINE_FUNCTION(FCoreSnowMeshRoot::execHandleNext2Shown)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    BindPillars(Context ? Context->GetWorld() : nullptr);
    P_NATIVE_END;
}

DEFINE_FUNCTION(FCoreSnowMeshRoot::execHandlePillarDamage)
{
    P_GET_PROPERTY(UIntProperty, Damage);
    P_GET_OBJECT(UObject, DirtProp);
    P_FINISH;
    P_NATIVE_BEGIN;
    HandlePillarDamage(Context, Damage, DirtProp);
    P_NATIVE_END;
}
