#include "pch.h"
#include "UnsafeEnvironment.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"

#include <algorithm>
#include <vector>

namespace
{
    // xor eax, eax; ret, written over the entry: nothing reads what either function returns.
    constexpr uint8_t ReturnZero[] = { 0x33, 0xC0, 0xC3 };
    // One function holds the notification and one queues it. More than that is not the pattern below, and nulling
    // whatever else the scan turned up would take working code with it.
    constexpr size_t MaxFunctions = 2;

    void Add(std::vector<uintptr_t>& Functions, uintptr_t Address)
    {
        const uintptr_t Function = Finder::FunctionStart(Address);
        if (Function && std::find(Functions.begin(), Functions.end(), Function) == Functions.end())
        {
            Functions.push_back(Function);
        }
    }
}

void UnsafeEnvironment::Null()
{
    // 2.2 and up: the UAC client's cheat-detected delegate runs a handler that reports the detection, queues
    // FPlatformMisc::RequestExit, and queues a second function which puts "Found active cheat or reverse engineering
    // tool. Please close them before playing Fortnite to avoid account ban." on screen under the
    // UnsafeEnvironment_Title heading and sends the player back to the main menu. Core is what it finds, so both the
    // notification and the handler that queues it lose their bodies. The registration is left alone: it binds a
    // handler that now returns at once.
    std::vector<uintptr_t> Notifications;
    for (const uintptr_t Ref : Finder::StringRefs(L"UnsafeEnvironment_Title"))
    {
        Add(Notifications, Ref);
    }
    if (Notifications.empty())
    {
        // 1.1 to 2.1 have the UAC client but no such notification.
        return;
    }

    std::vector<uintptr_t> Handlers;
    for (const uintptr_t Notification : Notifications)
    {
        // The notification is bound into a delegate rather than called, so its address is taken with a lea.
        for (const uintptr_t Ref : Finder::AddressRefs(Notification))
        {
            Add(Handlers, Ref);
        }
        for (const uintptr_t Call : Finder::CallsTo(Notification, true))
        {
            Add(Handlers, Call);
        }
    }
    std::erase_if(Handlers, [&Notifications](uintptr_t Function) { return std::find(Notifications.begin(), Notifications.end(), Function) != Notifications.end(); });

    if (Handlers.empty())
    {
        Log::Error(L"Redirect: nothing takes the address of the unsafe environment notification, so the exit it is queued with stays");
    }
    if (Notifications.size() > MaxFunctions || Handlers.size() > MaxFunctions)
    {
        Log::Error(L"Redirect: the unsafe environment notification has {} candidates and {} callers, nothing nulled", Notifications.size(), Handlers.size());
        return;
    }

    for (const uintptr_t Function : Notifications)
    {
        Hooks::WriteBytes(L"Unsafe environment notification", reinterpret_cast<void*>(Function), ReturnZero);
    }
    for (const uintptr_t Function : Handlers)
    {
        Hooks::WriteBytes(L"Unsafe environment handler", reinterpret_cast<void*>(Function), ReturnZero);
    }
}
