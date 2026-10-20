#pragma once

class UAbilitySystemComponent;
class UObject;

class IAbilitySystemInterface
{
public:
    virtual ~IAbilitySystemInterface() = default;
    virtual UObject* _getUObject() const = 0;
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const = 0;
};
