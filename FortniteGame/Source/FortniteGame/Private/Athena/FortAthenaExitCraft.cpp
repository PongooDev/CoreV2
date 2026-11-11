#include "pch.h"
#include "Athena/FortAthenaExitCraft.h"
#include "Athena/FortAthenaExitCraftInfo.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "FortSpecialActorReplicationInfo.h"
#include "Logging/LogMacros.h"
#include "Misc/CoreMiscDefines.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogAthenaExitCraft, Log, All);

void AFortAthenaExitCraft::RegisterCraftSpecialActor()
{
#if WITH_SERVER_CODE
    // The van's icon: what its spawner's RegisterSpawnerSpecialActor (5.40 0xBC4A10) does for the spawner's, with the
    // craft info's Craft half. On the server, once the game state has its list; the id is "<tag>_<name>"
    // ("%s_%s") and the init data the build's own, with the icons' brushes and scales copied in.
    UFortAthenaExitCraftInfo* Info = ExitCraftInfo;
    const FGameplayTag CraftTag = Info ? static_cast<FGameplayTag>(Info->SpecialActorCraftTag) : FGameplayTag();
    if (Role != ROLE_Authority || !CraftTag.IsValid() || !AFortSpecialActorReplicationInfo::ResolveNatives())
    {
        return;
    }
    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(World->GetGameState()) : nullptr;
    AFortSpecialActorReplicationInfo* SpecialActorData = GameState ? static_cast<AFortSpecialActorReplicationInfo*>(GameState->SpecialActorData) : nullptr;
    UScriptStruct* BrushStruct = FSlateBrush::StaticStruct();
    if (!SpecialActorData || !BrushStruct || BrushStruct->GetStructureSize() != static_cast<int32_t>(sizeof(FSpecialActorInitData::MinimapIconBrush)))
    {
        UE_LOG(LogAthenaExitCraft, Warning, TEXT("RegisterCraftSpecialActor: %s has no special actor list to go on"), *GetName());
        return;
    }

    const FSlateBrush& MinimapBrush = Info->CraftMinimapIconBrush;
    const FSlateBrush& CompassBrush = Info->CraftCompassIconBrush;
    FSpecialActorInitData InitData;
    InitData.SpecialActorRef = this;
    InitData.UniqueID = FName(*FString::Printf(TEXT("%s_%s"), *CraftTag.TagName.ToString(), *GetName()));
    InitData.CategoryTag = CraftTag;
    BrushStruct->CopyScriptStruct(InitData.MinimapIconBrush, &MinimapBrush);
    InitData.MinimapScale = Info->CraftMinimapIconScale;
    BrushStruct->CopyScriptStruct(InitData.CompassIconBrush, &CompassBrush);
    InitData.CompassScale = Info->CraftCompassIconScale;
    if (SpecialActorData->AddActorToReplicationList(InitData) != INDEX_NONE)
    {
        CraftSpecialActorID = InitData.UniqueID;
    }
#endif
}
