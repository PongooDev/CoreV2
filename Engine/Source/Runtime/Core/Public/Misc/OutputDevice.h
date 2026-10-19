#pragma once

class FOutputDevice
{
public:
    virtual ~FOutputDevice() = default;

    bool bSuppressEventTag = false;
    bool bAutoEmitLineTerminator = true;
};

static_assert(sizeof(FOutputDevice) == 0x10);
