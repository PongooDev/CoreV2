#pragma once

struct FCommandLine
{
    static const wchar_t* Get();
    // Puts -Switch back in the engine's command line when the process was started with it but the shipping build's
    // command line whitelist dropped it.
    static void RestoreSwitch(const wchar_t* Switch);
    // Adds -Switch to the engine's command line unless it has it.
    static void AddSwitch(const wchar_t* Switch);
    // Before the engine starts: lets -nullrhi, -nosplash, -nosound and -log through the shipping build's whitelist.
    static void ApproveServerSwitches();
};
