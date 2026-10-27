#pragma once

#include "Containers/Map.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

struct FDataTableCategoryHandle
{
    DefineUnrealStruct(FDataTableCategoryHandle, Engine)
};

class UDataTable : public UObject
{
    DefineUnrealClass(UDataTable, Engine)

public:
    DefineUProperty(UScriptStruct*, RowStruct)

    TMap<FName, uint8_t*>& GetRowMap() const;
    uint8_t* FindRowUnchecked(FName RowName) const;
    uint8_t* AddRow(FName RowName);

    template <class T>
    T* FindRow(FName RowName, const wchar_t* ContextString, bool bWarnIfRowMissing = true) const
    {
        return reinterpret_cast<T*>(FindRowChecked(T::StaticStruct(), RowName, ContextString, bWarnIfRowMissing));
    }

private:
    uint8_t* FindRowChecked(UScriptStruct* Type, FName RowName, const wchar_t* ContextString, bool bWarnIfRowMissing) const;
};

struct FDataTableRowHandle
{
    DefineUnrealStruct(FDataTableRowHandle, Engine)
    DefineUProperty(UDataTable*, DataTable)
    DefineUProperty(FName, RowName)
};
