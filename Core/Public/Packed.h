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

    /// <summary>Whether the exe is packed: a second .text section, the stub's own. Ordinary builds have one.</summary>
    bool IsPacked();

    /// <summary>
    /// On a packed build, blocks until the packer has run the one-time check it makes over the code its stub
    /// decrypted, which it does as the engine finishes its initial load. Patching .text before that point leaves the
    /// game spinning in the packer's VM and it never reaches the login screen, so hooks must be enabled after it.
    /// <para>Returns straight away on an ordinary build, where nothing checks the code.</para>
    /// </summary>
    void WaitForCodeCheck();

    /// <summary>
    /// On a packed build, has the engine's own thread run <paramref name="Callback"/> as it enters UEngine::Init,
    /// right after FEngineLoop::Init creates GEngine: after the packer's one-time code check, and before the engine
    /// initialises, browses to its default map or does anything else Core changes. It swaps the Init slot of the
    /// engine class's vtable (data, which the check doesn't look at) as soon as the class default object named
    /// <paramref name="EngineDefaultObject"/> exists. Blocks the caller until the engine exists.
    /// <para>Returns false, without running <paramref name="Callback"/>, when the engine got there first.</para>
    /// </summary>
    bool RunBeforeEngineInit(const wchar_t* EngineDefaultObject, void (*Callback)());
}
