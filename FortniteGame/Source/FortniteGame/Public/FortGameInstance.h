#pragma once

#include "Containers/Array.h"
#include "Engine/GameInstance.h"

class UFortPlaylistManager;
class UFortRegisteredPlayerInfo;
class USidecarSys;

class UFortGameInstance : public UGameInstance
{
    DefineUnrealClass(UFortGameInstance, FortniteGame)

public:
    DefineUProperty(UObject*, TooltipManager)
    DefineUProperty(TArray<UFortRegisteredPlayerInfo*>, RegisteredPlayers)
    // From 7.00: the DSS client Creative's islands are saved through (Transient, +0x330 on 7.00).
    DefineUProperty(USidecarSys*, SidecarSys)
    static bool HasSidecarSys() { return StaticClass() && Member_SidecarSys.IsAvailable(); }
    // Config (DefaultGame.ini): whether a player's registration gives them their creative profile, which the server's
    // controller copies (AFortPlayerController::CreativeModeProfile, from UFortRegisteredPlayerInfo +0x108 on 7.00).
    DefineUProperty(bool, bCreativeModeProfileEnabled)
    static bool HasCreativeModeProfileEnabled() { return StaticClass() && Member_bCreativeModeProfileEnabled.IsAvailable(); }

    // From 9.00 (+0x800), moved from AFortGameMode::PlaylistManager.
    DefineUProperty(UFortPlaylistManager*, PlaylistManager)
    static bool HasPlaylistManager() { return StaticClass() && Member_PlaylistManager.IsAvailable(); }

    void Init();

    static void Hook();

private:
    static inline void (*InitOG)(UFortGameInstance* This) = nullptr;
    static void InitHK(UFortGameInstance* This);
};
