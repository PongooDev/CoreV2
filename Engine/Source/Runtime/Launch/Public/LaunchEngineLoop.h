#pragma once

class FEngineLoop
{
public:
    static void Hook();

    // For a packed build, whose server only gets to patch once PreInit is long over (Packed::RunBeforeEngineInit):
    // what Hook does to PreInit's regular-client block, done to the globals themselves (GIsServer on, GIsClient off),
    // and the idle-mode patch, which FEngineLoop::Tick still reads. PreInit's other dedicated-server branches (splash,
    // Slate, movie player) have already run as a client's and are left alone.
    static void HookAfterPreInit();

    // For a 3.5+ packed build, whose server patches no code until the engine has ticked (Packed::RunBeforeEngineStart):
    // HookAfterPreInit in two steps. GIsClient goes off now, so UEngine::Init makes no viewport or local player, but
    // GIsServer stays off until the engine starts: FLinker's constructor filters the client-only exports of a package
    // for a server (GIsServer on, GIsClient off) unless the FLinker::Hook patch is in, and that patch is held back, so
    // UEngine::Init's loads (Fortnite's UI style among them) would come up empty. With both off nothing is filtered, as
    // with the patch. ServerAtStart then turns GIsServer on, once the patch is in and before the default map loads.
    static void HookAfterPreInitUntilStart();
    static void ServerAtStart();
};
