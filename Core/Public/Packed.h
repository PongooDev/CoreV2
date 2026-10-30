#pragma once

namespace Packed
{
    /// <summary>
    /// Builds 1.9.1 and up ship the exe packed: a stub in a second .text section decrypts the image at startup and
    /// only then jumps to the real entry point. Core is injected before any of that runs, so anything it looked for
    /// first would be read out of ciphertext, and anything it patched would be overwritten by the stub.
    /// <para>
    /// On such a build this starts a watcher that calls <paramref name="Start"/> once the stub hands over, and
    /// returns true. On an ordinary build it returns false and the caller runs <paramref name="Start"/> itself.
    /// </para>
    /// </summary>
    bool RunAfterStub(void (*Start)());
}
