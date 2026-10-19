#pragma once

#include "Misc/CoreMiscDefines.h"

#include <cstdint>

class FInBunch;
class UChannel;
class UNetConnection;

namespace EAcceptConnection
{
    enum Type
    {
        Reject,
        Accept,
        Ignore,
    };
}

class FNetworkNotify
{
public:
    virtual EAcceptConnection::Type NotifyAcceptingConnection() PURE_VIRTUAL(FNetworkNotify::NotifyAcceptedConnection, return EAcceptConnection::Ignore;);
    virtual void NotifyAcceptedConnection(UNetConnection* Connection) PURE_VIRTUAL(FNetworkNotify::NotifyAcceptedConnection, );
    virtual bool NotifyAcceptingChannel(UChannel* Channel) PURE_VIRTUAL(FNetworkNotify::NotifyAcceptingChannel, return false;);
    virtual void NotifyControlMessage(UNetConnection* Connection, uint8_t MessageType, FInBunch& Bunch) PURE_VIRTUAL(FNetworkNotify::NotifyReceivedText, );
};
