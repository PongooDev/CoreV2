#include "pch.h"
#include "Engine/DataTable.h"
#include "HAL/UnrealMemory.h"
#include "Log.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDataTable, Log, All);

TMap<FName, uint8_t*>& UDataTable::GetRowMap() const
{
    // RowMap is the native member right after RowStruct.
    return *reinterpret_cast<TMap<FName, uint8_t*>*>(reinterpret_cast<uintptr_t>(&GetMember_RowStruct()) + sizeof(UScriptStruct*));
}

uint8_t* UDataTable::FindRowUnchecked(FName RowName) const
{
    if (!RowStruct || RowName.IsNone())
    {
        return nullptr;
    }
    uint8_t* const* RowData = GetRowMap().Find(RowName);
    return RowData ? *RowData : nullptr;
}

uint8_t* UDataTable::FindRowChecked(UScriptStruct* Type, FName RowName, const wchar_t* ContextString, bool bWarnIfRowMissing) const
{
    if (!RowStruct)
    {
        UE_LOG(LogDataTable, Error, TEXT("UDataTable::FindRow : '%s' specified no row for DataTable '%s'."), ContextString, *GetPathName());
        return nullptr;
    }
    if (!Type || !RowStruct->IsChildOf(Type))
    {
        UE_LOG(LogDataTable, Error, TEXT("UDataTable::FindRow : '%s' specified incorrect type for DataTable '%s'."), ContextString, *GetPathName());
        return nullptr;
    }
    if (RowName.IsNone())
    {
        UE_LOG(LogDataTable, Warning, TEXT("UDataTable::FindRow : '%s' requested invalid row 'None' from DataTable '%s'."), ContextString, *GetPathName());
        return nullptr;
    }

    uint8_t* const* RowData = GetRowMap().Find(RowName);
    if (!RowData)
    {
        if (bWarnIfRowMissing)
        {
            UE_LOG(LogDataTable, Warning, TEXT("UDataTable::FindRow : '%s' requested row '%s' not in DataTable '%s'."), ContextString, *RowName.ToString(), *GetPathName());
        }
        return nullptr;
    }
    return *RowData;
}

uint8_t* UDataTable::AddRow(FName RowName)
{
    UScriptStruct* Struct = RowStruct;
    if (!Struct || RowName.IsNone())
    {
        return nullptr;
    }

    uint8_t* RowData = static_cast<uint8_t*>(FMemory::Malloc(Struct->GetStructureSize()));
    Struct->InitializeStruct(RowData);
    GetRowMap().Add(RowName, RowData);
    return RowData;
}
