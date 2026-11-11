#pragma once

#include "UObject/Object.h"

// UE's FSlateBrush (SlateCore/Public/Styling/SlateBrush.h): an image for the UI (its resource, size, tint, tiling and
// draw settings), such as a map or compass icon. 0x88 bytes in UE 4.21, and native: it has a vtable. Core never reads
// its members; it copies whole brushes with the struct's own copy (UScriptStruct::CopyScriptStruct, the native
// operator=), so the copy keeps its vtable and holds its resource as the original does.
struct FSlateBrush
{
    DefineUnrealStruct(FSlateBrush, SlateCore)
};
