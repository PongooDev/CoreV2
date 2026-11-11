#pragma once

enum EInputEvent
{
    IE_Pressed = 0,
    IE_Released = 1,
    IE_Repeat = 2,
    IE_DoubleClick = 3,
    IE_Axis = 4,
    IE_MAX = 5,
};

#include "Reflection.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"

enum ENetMode
{
    NM_Standalone,
    NM_DedicatedServer,
    NM_ListenServer,
    NM_Client,
    NM_MAX,
};

namespace ENetworkFailure
{
    enum Type
    {
        NetDriverAlreadyExists,
        NetDriverCreateFailure,
        NetDriverListenFailure,
        ConnectionLost,
        ConnectionTimeout,
        FailureReceived,
        OutdatedClient,
        OutdatedServer,
        PendingConnectionFailure,
        NetGuidMismatch,
        NetChecksumMismatch,
    };
}

class ULevel;

// When a tick function runs in the world's frame, relative to physics (the world ticks the groups in this order).
enum ETickingGroup : uint8_t
{
    TG_PrePhysics,
    TG_StartPhysics,
    TG_DuringPhysics,
    TG_EndPhysics,
    TG_PostPhysics,
    TG_PostUpdateWork,
    TG_LastDemotable,
    TG_NewlySpawned,
    TG_MAX,
};

// Engine.TickFunction: a function the world's tick task manager runs once a frame, in its tick group, once it is
// registered with a level (an actor's PrimaryActorTick, a component's PrimaryComponentTick, the world's physics ticks).
// Core's view of the build's struct: reflection has the members a tick is set up with, at the build's offsets (0x50
// bytes from 1.8.1 to 5.40). The rest (bRegistered, the tick state, the level it is registered with) is the engine's,
// kept by its constructor and by RegisterTickFunction, UnRegisterTickFunction and SetTickFunctionEnable, which Core
// calls (FTickFunction::ResolveNatives).
struct FTickFunction
{
    DefineUnrealStruct(FTickFunction, Engine)
    // TEnumAsByte<ETickingGroup>.
    DefineUProperty(uint8_t, TickGroup)
    DefineUProperty(uint8_t, EndTickGroup)
    DefineBitfieldUProperty(bTickEvenWhenPaused)
    DefineBitfieldUProperty(bCanEverTick)
    DefineBitfieldUProperty(bStartWithTickEnabled)
    DefineBitfieldUProperty(bAllowTickOnDedicatedServer)
    // 0 or less: every frame.
    DefineUProperty(float, TickInterval)

public:
    // Adds it to Level's tick functions (FTickTaskManager::AddTickFunction): it runs each frame of the level's world
    // while enabled. Nothing when it is registered already.
    void RegisterTickFunction(ULevel* Level);
    // Takes it out of its level's tick functions. Nothing when it isn't registered (a level that goes clears its
    // functions' bRegistered, FTickTaskLevel's destructor, 5.40 0x2894C50).
    void UnRegisterTickFunction();
    // Enables or disables it, registered or not (it moves between its level's enabled and disabled sets).
    void SetTickFunctionEnable(bool bInEnabled);
    bool IsTickFunctionRegistered() const;

    // Finds RegisterTickFunction, UnRegisterTickFunction, SetTickFunctionEnable and bRegistered (TickTaskManager.cpp).
    // Game thread, once the engine's reflection is up; false where they aren't all found, logged once.
    static bool ResolveNatives();
};

// A tick function of Core's, as Epic's subclasses of FTickFunction are (FActorTickFunction, the physics ticks): the
// build's FTickFunction, made by the engine's own constructor (the struct's UScriptStruct::InitializeStruct) so its
// layout is the build's, with ExecuteTick and DiagnosticMessage in a copy of the build's FTickFunction vtable (slots 1
// and 2 after the destructor: 1.8.1, 3.2 and 5.40's physics ticks have these three). Core's object is held after the
// struct, as a subclass's members follow its base.
class FCoreTickFunction
{
public:
    FCoreTickFunction() = default;
    FCoreTickFunction(const FCoreTickFunction&) = delete;
    FCoreTickFunction& operator=(const FCoreTickFunction&) = delete;
    // Unregisters and destroys the build's struct (its destructor), unless the engine is exiting.
    virtual ~FCoreTickFunction();

    // Makes the build's FTickFunction: false (logged) where the natives or the struct aren't there.
    bool Initialize();
    // The build's struct, once Initialize made it.
    FTickFunction* GetTickFunction() const { return reinterpret_cast<FTickFunction*>(Storage); }

    // What the tick does each frame (Epic's ExecuteTick(DeltaTime, TickType, CurrentThread, MyCompletionGraphEvent)).
    virtual void ExecuteTick(float DeltaTime) = 0;
    // What the engine's tick reports name it by (dumpticks).
    virtual std::wstring DiagnosticMessage() const = 0;

private:
    static void ExecuteTickThunk(uint8_t* This, float DeltaTime, int32_t TickType, int32_t CurrentThread, const void* MyCompletionGraphEvent);
    static FString* DiagnosticMessageThunk(uint8_t* This, FString* Result);
    static FCoreTickFunction* OwnerOf(uint8_t* Struct);

    uint8_t* Storage = nullptr;
};

struct FURL
{
    DefineUnrealStruct(FURL, Engine)
    DefineUProperty(FString, Protocol)
    DefineUProperty(FString, Host)
    DefineUProperty(int32_t, Port)
    DefineUProperty(FString, Map)
    DefineUProperty(FString, RedirectURL)
    DefineUProperty(TArray<FString>, Op)
    DefineUProperty(FString, Portal)
    DefineUProperty(int32_t, Valid)
};
