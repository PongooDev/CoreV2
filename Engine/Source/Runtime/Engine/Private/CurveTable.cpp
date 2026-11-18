#include "pch.h"
#include "Engine/CurveTable.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

bool FCurveTableRowHandle::Eval(float XValue, float* YValue, const wchar_t* ContextString) const
{
    (void)ContextString;
    // Epic's finds the row (UCurveTable::FindCurve) and evaluates its FRealCurve. Core reaches the same lookup through
    // the engine's own UDataTableFunctionLibrary::EvaluateCurveTableRow, which every build has.
    UCurveTable* Table = CurveTable;
    if (!Table || !YValue)
    {
        return false;
    }
    static UFunction* Evaluate = FindObject<UFunction>(nullptr, L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow");
    UClass* Library = FindObject<UClass>(nullptr, L"/Script/Engine.DataTableFunctionLibrary");
    UObject* LibraryDefault = Library ? Library->GetDefaultObject() : nullptr;
    if (!Evaluate || !LibraryDefault)
    {
        return false;
    }
    Reflection::FParams Params(Evaluate, true);
    // EEvaluateCurveTableResult::RowFound is 0.
    if (!Params.Set(L"CurveTable", reinterpret_cast<UObject*>(Table)) || !Params.Set(L"RowName", static_cast<FName>(RowName)) || !Params.Set(L"InXY", XValue)
        || !Params.Call(LibraryDefault) || Params.Take<uint8_t>(L"OutResult") != 0)
    {
        return false;
    }
    *YValue = Params.Take<float>(L"OutXY");
    return true;
}
