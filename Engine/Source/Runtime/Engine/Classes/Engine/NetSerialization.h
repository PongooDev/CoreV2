#pragma once

#include "Misc/CoreMiscDefines.h"
#include "ObjectLayout.h"
#include "Reflection.h"

struct FFastArraySerializerItem
{
    DefineUnrealStruct(FFastArraySerializerItem, Engine)
    DefineUProperty(int32_t, ReplicationID)
    DefineUProperty(int32_t, ReplicationKey)
};

struct FFastArraySerializer
{
    DefineNativeMember(int32_t, IDCounter, FFastArraySerializer_IDCounter)

public:
    void MarkItemDirty(FFastArraySerializerItem& Item)
    {
        if (Item.ReplicationID == INDEX_NONE)
        {
            IDCounter = IDCounter + 1;
            Item.ReplicationID = IDCounter;
            if (IDCounter == INDEX_NONE)
            {
                IDCounter = IDCounter + 1;
            }
        }

        Item.ReplicationKey = Item.ReplicationKey + 1;
        MarkArrayDirty();
    }

    void MarkArrayDirty();
};
