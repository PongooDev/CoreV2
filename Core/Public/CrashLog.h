#pragma once

namespace CrashLog
{
    // Writes a crash's faulting address and call stack to the Core log, as module+offset (#113).
    void Install();
}
