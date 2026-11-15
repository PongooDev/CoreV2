#pragma once

#include "Building/BuildingActor.h"
#include "Delegates/Delegate.h"

class EBarrierFoodTeam
{
    DefineUnrealEnum(EBarrierFoodTeam, FortniteGame)
    DefineUEnumValue(Burger)
    DefineUEnumValue(Tomato)
    DefineUEnumValue(MAX)
};

// Food Fight's mascot (from 6.30; TeamGenerator_C): the giant food head on a team's restaurant, an
// ABuildingGameplayActor (Core declares none, so this derives from its ABuildingActor). It is the child actor of its
// team's AAthenaBarrierFlag. It takes damage only while bAllowDamage and only from the other team (its OnDamageServer
// override, 6.30 0xC4EA40). Its blueprint's OnDeathServer calls OnGeneratorDestroyed (0xC50470), which broadcasts the
// objective-destroyed delegate on the authority: the Barrier mutator's OnObjectiveDestroyed.
class AAthenaBarrierObjective : public ABuildingActor
{
    DefineUnrealClass(AAthenaBarrierObjective, FortniteGame)
    DefineUFunction(OnGeneratorDestroyed)

public:
    DefineUProperty(EBarrierFoodTeam, FoodTeam)
    DefineUProperty(float, HeadRotationYaw)
    DefineUProperty(bool, bAllowDamage)

    // DECLARE_MULTICAST_DELEGATE_OneParam(..., AAthenaBarrierObjective*): what OnGeneratorDestroyed broadcasts, the first
    // of the class's unreflected members (6.30: "lea rsi, [rcx+740h]" in 0xC50470, found there). Null where it wasn't.
    TMulticastDelegate<AAthenaBarrierObjective*>* GetOnObjectiveDestroyed() const;

    // 21.40's AAthenaBarrierObjective::SetAllowDamage: whether the other team's damage reaches the mascot (bAllowDamage,
    // replicated).
    void SetAllowDamage(bool bInAllowDamage);
};
