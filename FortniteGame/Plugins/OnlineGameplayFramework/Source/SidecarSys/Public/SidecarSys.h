#pragma once

#include "Containers/UnrealString.h"
#include "Reflection.h"
#include "UObject/Object.h"

#include <cstdint>
#include <functional>
#include <string>

// What the sidecar knows of one file (7.00, CL-4629139): its meta (sent as X-Epic-Meta-* headers with each upload),
// bIsCheckedOut (the server holds its lock: uploads without autolock and the checkin need it) and bOperationPending.
struct FSidecarFileInfo
{
    DefineUnrealStruct(FSidecarFileInfo, SidecarSys)

public:
    DefineUProperty(bool, bIsCheckedOut)
    DefineUProperty(bool, bOperationPending)
};

// The client of the Data Storage Service (DSS) the game's Creative islands are kept in (from 7.00). The game instance
// owns one (UFortGameInstance::SidecarSys). Its requests go out through the MCP request queue with the DedicatedServer
// client's token (SendRequest, 7.00 0x2DF5910), to the OnlineSidecarServiceMcp service:
//   /api/dss/v1/`app_name/`file_path                         download
//   /api/dss/v1/`app_name/`file_path/checkout?pid=`pid&dl=`dl  checkout (dl: download the file with it)
//   /api/dss/v1/`app_name/`file_path?autolock=`autolock        upload (PUT, the file's bytes and X-Epic-Meta-* headers)
//   /api/dss/v1/`app_name/`file_path/restore?from=`from        restore
//   /api/dss/v1/`app_name/`file_path/checkin                   checkin (POST, the last bytes, then the lock is released)
// The client builds keep the upload (0x2DF4530) and the checkin (0x2DF3B40), which the island's save uses; the checkout,
// the download and the restore were only ever called by the server's island load, and went with it (their log strings
// "DssCheckout failed: file %s/%s is already checked out" and the URLs are all that is left).
class USidecarSys : public UObject
{
    DefineUnrealClass(USidecarSys, SidecarSys)

public:
    DefineUProperty(FString, DssCheckoutUrl)

    // The checkout's answer, on the game thread: whether the lock is the server's (200 with the file, 204 for a file
    // that doesn't exist yet), the HTTP status, and with dl the file's bytes (empty for a new file).
    using FOnDssCheckoutComplete = std::function<void(bool bCheckedOut, uint32_t Status, const std::string& Contents)>;

    // Restored (7.00 strips it with its only caller, the island load): checks AppName/FilePath out for Pid and, with
    // bDownload, brings its bytes back. Once the lock is the server's the file is in FileInfoMap, checked out, so the
    // game's own upload and checkin of it go through. Core sends the request to its backend itself (the game's request
    // and its completion went with the function), at the same path, with the same DedicatedServer client.
    void Checkout(const FString& AppName, const FString& FilePath, const FString& Pid, bool bDownload, FOnDssCheckoutComplete OnComplete);

    // The file's entry in FileInfoMap (the TMap's own key, "AppName/FilePath"), or null.
    FSidecarFileInfo* FindFileInfo(const FString& AppName, const FString& FilePath) const;

    // The server's id for its checkouts (DSS's pid): the game session's id once the session has one, else an id of this
    // server process's own (Core-Backend #471's contract).
    static FString GetCheckoutPid();

private:
    // FileInfoMap's storage (TMap<FString, FSidecarFileInfo>, 0x50 bytes), read as the engine lays a TSet out.
    struct FFileInfoMapStorage
    {
        uint8_t Bytes[0x50];
    };
    DefineUProperty(FFileInfoMapStorage, FileInfoMap)
};
