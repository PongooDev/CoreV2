#pragma once

enum class ESPMode
{
    NotThreadSafe = 0,
    Fast = 0,
    ThreadSafe = 1,
};

template <class ObjectType, ESPMode Mode = ESPMode::Fast> class TSharedRef;
template <class ObjectType, ESPMode Mode = ESPMode::Fast> class TSharedPtr;
template <class ObjectType, ESPMode Mode = ESPMode::Fast> class TWeakPtr;
template <class ObjectType, ESPMode Mode = ESPMode::Fast> class TSharedFromThis;
