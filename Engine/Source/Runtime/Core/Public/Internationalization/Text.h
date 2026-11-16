#pragma once

#include "Templates/SharedPointer.h"

#include <cstdint>
#include <utility>

class ITextData;

// Epic's FText as the engine holds it: a thread-safe shared reference to its text data, then its flags (0x18 bytes, UE
// 4.16 to 4.22). Core never builds the data itself: the engine makes it (UKismetTextLibrary::Conv_StringToText), and
// Core only moves, copies and releases the reference, as the engine's own copy and destructor do. A default FText of
// Core's has no data and must not be handed to the engine; a text the engine initialized (InitializeStruct) has its
// empty text's.
class FText
{
public:
    FText() = default;
    FText(const FText&) = default;
    FText(FText&&) noexcept = default;
    FText& operator=(const FText&) = default;
    FText& operator=(FText&& Other) noexcept
    {
        TextData = std::move(Other.TextData);
        Flags = Other.Flags;
        return *this;
    }

    bool HasTextData() const { return TextData.IsValid(); }

private:
    TSharedPtr<ITextData, ESPMode::ThreadSafe> TextData;
    uint32_t Flags = 0;
};

static_assert(sizeof(FText) == 0x18);
