#include "pch.h"
#include "Athena/AthenaBarrierFlag.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

AAthenaBarrierObjective* AAthenaBarrierFlag::GetObjectiveActor() const
{
    UFunction* Function = FindStaticFunction_GetObjectiveActor();
    if (!Function)
    {
        return nullptr;
    }
    // The class's function, which TeamObjective_Food1_C overrides: ProcessEvent finds the blueprint's.
    UFunction* Override = GetClass()->FindFunctionByName(Function->GetFName());
    Reflection::FParams Params(Override ? Override : Function);
    Params.Call(const_cast<AAthenaBarrierFlag*>(this));
    UObject** Result = Params.Get<UObject*>(L"ReturnValue");
    return Result ? Cast<AAthenaBarrierObjective>(*Result) : nullptr;
}
