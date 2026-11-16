#include "pch.h"
#include "SidecarSys.h"
#include "BackendRequest.h"
#include "Containers/Map.h"
#include "GameThread.h"
#include "Log.h"
#include "Misc/Guid.h"
#include "Natives.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "UObject/WeakObjectPtr.h"

#include <cwchar>
#include <format>
#include <memory>

namespace
{
    // FSidecarFileInfo as FileInfoMap holds it (7.00: 0x58 bytes, Meta first, then bIsCheckedOut and bOperationPending;
    // the checkin's answer clears +0x50 and +0x51 of the pair's value, 0x2DF3320).
    struct FSidecarFileInfoValue
    {
        uint8_t Meta[0x50];
        bool bIsCheckedOut;
        bool bOperationPending;
        uint8_t Padding[6];
    };
    static_assert(sizeof(FSidecarFileInfoValue) == 0x58);
    using FFileInfoMap = TMap<FString, FSidecarFileInfoValue>;

    std::wstring ReplaceToken(std::wstring Text, std::wstring_view Token, const std::wstring& Value)
    {
        for (size_t At = Text.find(Token); At != std::wstring::npos; At = Text.find(Token, At + Value.size()))
        {
            Text.replace(At, Token.size(), Value);
        }
        return Text;
    }
}

FString USidecarSys::GetCheckoutPid()
{
    // The contract asks for the game session's id; Core's server has no reflected id of its session to read, so its
    // pid is this process's (the backend then keeps a lock 15 idle minutes, renewed by each upload and checkin).
    static const std::wstring Pid = []
    {
        const FGuid Guid = FGuid::NewGuid();
        return std::format(L"{:08x}{:08x}{:08x}{:08x}", Guid.A, Guid.B, Guid.C, Guid.D);
    }();
    return FString(Pid.c_str());
}

FSidecarFileInfo* USidecarSys::FindFileInfo(const FString& AppName, const FString& FilePath) const
{
    if (!Member_FileInfoMap.IsAvailable())
    {
        return nullptr;
    }
    // The map's key is "%s/%s" of the two (the upload's and the meta setter's own key, 0x2DF4530 and 0x2DF5C40), and
    // FString keys match without case.
    const std::wstring Key = AppName.ToStdString() + L"/" + FilePath.ToStdString();
    FFileInfoMap& Map = *reinterpret_cast<FFileInfoMap*>(&GetMember_FileInfoMap());
    for (auto& Pair : Map)
    {
        if (_wcsicmp(Pair.Key.ToStdString().c_str(), Key.c_str()) == 0)
        {
            return reinterpret_cast<FSidecarFileInfo*>(&Pair.Value);
        }
    }
    return nullptr;
}

void USidecarSys::Checkout(const FString& AppName, const FString& FilePath, const FString& Pid, bool bDownload, FOnDssCheckoutComplete OnComplete)
{
#if WITH_SERVER_CODE
    // DssCheckoutUrl as the sidecar's constructor set it (7.00 0x2DF2D30), with the game's own tokens.
    std::wstring Path = Member_DssCheckoutUrl.IsAvailable() ? DssCheckoutUrl.ToStdString() : std::wstring();
    if (Path.empty())
    {
        Path = L"/api/dss/v1/`app_name/`file_path/checkout?pid=`pid&dl=`dl";
    }
    Path = ReplaceToken(Path, L"`app_name", AppName.ToStdString());
    Path = ReplaceToken(Path, L"`file_path", FilePath.ToStdString());
    Path = ReplaceToken(Path, L"`pid", Pid.ToStdString());
    Path = ReplaceToken(Path, L"`dl", bDownload ? L"true" : L"false");

    const FWeakObjectPtr WeakSidecar(this);
    const std::wstring App = AppName.ToStdString();
    const std::wstring File = FilePath.ToStdString();
    auto Complete = std::make_shared<FOnDssCheckoutComplete>(std::move(OnComplete));
    BackendRequest::PostJson(std::format(L"DSS checkout of {}/{}", App, File), Path, std::string(), FFortMcpConfig::DedicatedServerClientId,
        FFortMcpConfig::DedicatedServerClientSecret, [WeakSidecar, App, File, Complete](uint32_t Status, const std::string& Body)
    {
        GameThread::Enqueue([WeakSidecar, App, File, Complete, Status, Body]()
        {
            // 200: the lock and the file; 204 (or 200 empty): the lock, no file yet (a new island). Anything else is no
            // lock: 404 is a file that isn't one of the owner's islands, 409 another server's lock (Core-Backend #471).
            const bool bCheckedOut = Status == 200 || Status == 204;
            USidecarSys* Sidecar = static_cast<USidecarSys*>(WeakSidecar.Get());
            if (bCheckedOut && Sidecar)
            {
                // The file goes into FileInfoMap as the sidecar's own meta setter adds it (its FindOrAdd), and is then
                // the server's: the upload refuses a file that isn't ("DssUpload failed: file %s/%s is not checked out
                // (and no autolock requested)"), the checkin likewise. The key is the one the save sets on every upload
                // (serialized-at, 0xF5B03A), so no meta of Core's goes out with the file.
                const FString AppName(App.c_str());
                const FString FilePath(File.c_str());
                if (Natives::USidecarSys_SetMeta)
                {
                    Natives::USidecarSys_SetMeta(Sidecar, AppName, FilePath, FString(L"serialized-at"), FString(L""));
                }
                if (FSidecarFileInfo* Info = Sidecar->FindFileInfo(AppName, FilePath))
                {
                    Info->bIsCheckedOut = true;
                    Info->bOperationPending = false;
                }
                else
                {
                    Log::Error(L"Creative: {}/{} is checked out, but the sidecar's FileInfoMap has no entry for it (meta setter {}), so the game won't upload it",
                        App, File, Natives::USidecarSys_SetMeta ? L"found" : L"not found");
                }
            }
            Log::Info(L"Creative: DSS checkout of {}/{}: HTTP {}, {}", App, File, Status,
                Status == 200 && !Body.empty() ? std::format(L"{} bytes", Body.size()) : bCheckedOut ? std::wstring(L"no file yet") : std::wstring(L"not checked out"));
            if (*Complete)
            {
                (*Complete)(bCheckedOut, Status, Status == 200 ? Body : std::string());
            }
        });
    });
#endif
}
