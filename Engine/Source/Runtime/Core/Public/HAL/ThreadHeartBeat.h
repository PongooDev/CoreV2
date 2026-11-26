#pragma once

// The engine's hang detector (Epic's HAL/ThreadHeartBeat.h), called through the engine's own object. Its thread
// (FHeartBeatThread) ends the process when a thread it watches sends no heartbeat for [Core.System] HangDuration
// (25 s by default; 9.10 CL-6573057 FThreadHeartBeat::OnHang +0x1D84B60 -> FPlatformMisc::RequestExit(true)). The game
// thread's heartbeat is sent once per frame, so one long frame of Core's own work counts as a hang.
// Finder::ResolveThreadHeartBeat finds the three functions below; a build where it doesn't find them has none of this.
class FThreadHeartBeat
{
public:
    // The singleton, or null before FThreadHeartBeat::Get first made it.
    static FThreadHeartBeat* GetNoInit();

    // Whether the build's GetNoInit, SuspendHeartBeat and ResumeHeartBeat were found.
    static bool IsAvailable();

    // The calling thread's heartbeat stops being checked until a matching ResumeHeartBeat (counted). 4.20 (3.3 CL-3942182
    // 0x120B100) takes no argument, 4.23 (9.10 0x1D8B200) bAllThreads, so false is passed in edx either way.
    void SuspendHeartBeat(bool bAllThreads = false);
    // The last of the matching resumes restarts the thread's hang clock from now.
    void ResumeHeartBeat(bool bAllThreads = false);
};

// Epic's scope for slow work on one thread: the heartbeat is suspended for its lifetime.
class FSlowHeartBeatScope
{
public:
    explicit FSlowHeartBeatScope(bool bAllThreads = false)
        : bSuspendedAllThreads(bAllThreads)
    {
        if (FThreadHeartBeat* HB = FThreadHeartBeat::GetNoInit())
        {
            HB->SuspendHeartBeat(bSuspendedAllThreads);
            bSuspended = true;
        }
    }

    ~FSlowHeartBeatScope()
    {
        if (!bSuspended)
        {
            return;
        }
        if (FThreadHeartBeat* HB = FThreadHeartBeat::GetNoInit())
        {
            HB->ResumeHeartBeat(bSuspendedAllThreads);
        }
    }

    FSlowHeartBeatScope(const FSlowHeartBeatScope&) = delete;
    FSlowHeartBeatScope& operator=(const FSlowHeartBeatScope&) = delete;

    bool IsSuspended() const { return bSuspended; }

private:
    bool bSuspendedAllThreads;
    bool bSuspended = false;
};
