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
};
