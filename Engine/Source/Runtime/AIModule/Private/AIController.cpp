#include "pch.h"
#include "AIController.h"
#include "ReflectionParams.h"

EPathFollowingRequestResult::Type AAIController::MoveToLocation(const FVector& Dest, float AcceptanceRadius, bool bStopOnOverlap, bool bUsePathfinding, bool bProjectDestinationToNavigation, bool bCanStrafe, bool bAllowPartialPath)
{
    Reflection::FParams Params(StaticFunction_MoveToLocation(), true);
    Params.Set(L"Dest", Dest);
    Params.Set(L"AcceptanceRadius", AcceptanceRadius);
    Params.Set(L"bStopOnOverlap", bStopOnOverlap);
    Params.Set(L"bUsePathfinding", bUsePathfinding);
    Params.Set(L"bProjectDestinationToNavigation", bProjectDestinationToNavigation);
    Params.Set(L"bCanStrafe", bCanStrafe);
    Params.Set(L"bAllowPartialPath", bAllowPartialPath);
    if (!Params.Call(this))
    {
        return EPathFollowingRequestResult::Failed;
    }
    return static_cast<EPathFollowingRequestResult::Type>(Params.Take<uint8_t>(L"ReturnValue"));
}

void AAIController::SetFocalPoint(const FVector& FocalPoint)
{
    Reflection::FParams Params(StaticFunction_K2_SetFocalPoint(), true);
    Params.Set(L"FP", FocalPoint);
    Params.Call(this);
}

void AAIController::SetFocus(AActor* NewFocus)
{
    Reflection::FParams Params(StaticFunction_K2_SetFocus(), true);
    Params.Set(L"NewFocus", NewFocus);
    Params.Call(this);
}

void AAIController::ClearFocus()
{
    Reflection::FParams Params(StaticFunction_K2_ClearFocus(), true);
    Params.Call(this);
}
