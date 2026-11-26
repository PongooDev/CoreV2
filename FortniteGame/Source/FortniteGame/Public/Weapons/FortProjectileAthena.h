#pragma once

#include "GameFramework/Actor.h"
#include "Reflection.h"

class UProjectileMovementComponent;
class UScriptStruct;

class AFortProjectileBase : public AActor
{
    DefineUnrealClass(AFortProjectileBase, FortniteGame)

public:
    DefineUProperty(UProjectileMovementComponent*, ProjectileMovementComponent)
};

// Battle Royale's bullet projectiles (B_Prj_Bullet_Sniper and its children; a rocket is an AFortProjectileBase). Its pawn
// hits are the shooter's client's: on the server its BeginPlay (9.00 0x10CC4D0, 8.51 0x1056950) turns the capsule's
// collision off and starts KillAfterNoPawnHitTimerHandle (TimeToWaitForPawnHitBeforeKillOnServer, 4 s on 9.00), and
// InternalOnTouched (9.00 0x10DC830) does nothing on authority. The owning client's copy touches the pawn and sends
// ServerNotifyPawnHit; its _Implementation (vtable 0xD9, 9.00 0x10E6130) moves the projectile to the hit and runs
// AFortProjectileBase::InternalOnTouched on the server, which applies EffectContainerSpecToApplyOnTouch to the pawn.
class AFortProjectileAthena : public AFortProjectileBase
{
    DefineUnrealClass(AFortProjectileAthena, FortniteGame)
    DefineUFunction(ServerNotifyPawnHit)

public:
    // Hit is an FHitResult (HitResultStruct). A server RPC called on the server runs its _Implementation there.
    void ServerNotifyPawnHit(const void* Hit);

    // ServerNotifyPawnHit's Hit parameter's struct; null where the build has no ServerNotifyPawnHit.
    static UScriptStruct* HitResultStruct();
};
