#pragma once

#include "Containers/Array.h"
#include "Math/Vector.h"
#include "UObject/Object.h"
#include "UObject/ScriptDelegates.h"

class AActor;
class UFunction;

class UEnvQueryInstanceBlueprintWrapper : public UObject
{
    DefineUnrealClass(UEnvQueryInstanceBlueprintWrapper, AIModule)
    DefineUFunction(GetResultsAsActors)
    DefineUFunction(GetResultsAsLocations)
    DefineUFunction(SetNamedParam)

public:
    // What the wrapper broadcasts once its query is done (its OnQueryFinished, the query's finish delegate: the result is
    // in, then it leaves its manager's GCShieldedWrappers): FEQSQueryDoneSignature(QueryInstance, QueryStatus). The game's
    // blueprints wait on it (5.30's Trap_Floor_Defender_Master binds its EQS_DefenderPadTeleportIn query's).
    DefineUProperty(FMulticastScriptDelegate, OnQueryFinishedEvent)
    static bool HasOnQueryFinishedEvent() { return StaticClass() && Member_OnQueryFinishedEvent.IsAvailable(); }
    // OnQueryFinishedEvent's FEQSQueryDoneSignature, the parameters a native bound to it takes; null without one.
    static const UFunction* GetQueryFinishedSignature();

    TArray<AActor*> GetResultsAsActors();
    // The items' locations, best score first (a point generator's items, or each actor's location); empty until the
    // query is done.
    TArray<FVector> GetResultsAsLocations();
    // Adds a named param to the running query instance (its NamedParams, which the generators' and tests' data bindings
    // read when the instance runs on the EQS manager's tick). False where the build has no SetNamedParam.
    bool SetNamedParam(FName ParamName, float Value);
};
