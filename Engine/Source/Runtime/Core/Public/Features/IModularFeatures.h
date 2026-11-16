#pragma once

#include "Features/IModularFeature.h"
#include "UObject/NameTypes.h"

#include <cstdint>

// The engine's registry of modular features (Epic's Features/IModularFeatures.h), called through the engine's own
// object (the static FModularFeatures that IModularFeatures::Get returns). Its vtable as UE 4.16 to 4.22 declare it,
// checked on 7.00 (FModularFeatures' vtable 0x41873F8: the destructor, GetModularFeatureImplementationCount 0x16F4280,
// GetModularFeatureImplementation 0x16F40E0, RegisterModularFeature 0x16F90B0 (AddUnique into ModularFeaturesMap, then
// the registered event's Broadcast), UnregisterModularFeature 0x16FF250, then the two events). UE5 puts
// LockModularFeatureList and UnlockModularFeatureList after the destructor (21.40 PDB), which these builds don't have:
// Finder::ResolveModularFeaturesLive only accepts a build whose own lookups call slots 1 and 2.
class IModularFeatures
{
public:
    // The registry (Core: Natives::IModularFeatures_Get, which must be resolved first).
    static IModularFeatures& Get();

    virtual ~IModularFeatures() = default;

    // Whether any implementation of Type is registered, and the first one, as the engine's templated helpers do.
    template <typename TModularFeature>
    bool IsModularFeatureAvailable(const FName Type)
    {
        return GetModularFeatureImplementationCount(Type) > 0;
    }

    template <typename TModularFeature>
    TModularFeature& GetModularFeature(const FName Type)
    {
        return static_cast<TModularFeature&>(*GetModularFeatureImplementation(Type, 0));
    }

    virtual int32_t GetModularFeatureImplementationCount(const FName Type) = 0;
    virtual IModularFeature* GetModularFeatureImplementation(const FName Type, const int32_t Index) = 0;
    virtual void RegisterModularFeature(const FName Type, IModularFeature* ModularFeature) = 0;
    virtual void UnregisterModularFeature(const FName Type, IModularFeature* ModularFeature) = 0;
};
