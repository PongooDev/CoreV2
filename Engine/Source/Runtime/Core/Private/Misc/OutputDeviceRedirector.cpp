#include "pch.h"
#include "Misc/OutputDeviceRedirector.h"
#include "CoreGlobals.h"
#include "Natives.h"

FOutputDeviceRedirector* GetGlobalLogSingleton()
{
    return Natives::GetGlobalLogSingleton();
}

void FOutputDeviceRedirector::AddOutputDevice(FOutputDevice* OutputDevice)
{
    Natives::FOutputDeviceRedirector_AddOutputDevice(this, OutputDevice);
}
