#pragma once

#include "Misc/OutputDevice.h"

class FOutputDeviceRedirector : public FOutputDevice
{
public:
    void AddOutputDevice(FOutputDevice* OutputDevice);
};
