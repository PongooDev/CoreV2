#pragma once

template <bool HAS_EDITOR_DATA, bool IS_DEDICATED_SERVER, bool IS_CLIENT_ONLY>
struct FWindowsPlatformProperties
{
    static constexpr bool HasEditorOnlyData() { return HAS_EDITOR_DATA; }
    static constexpr bool IsServerOnly() { return IS_DEDICATED_SERVER; }
    static constexpr bool IsClientOnly() { return IS_CLIENT_ONLY; }
    static constexpr bool IsGameOnly() { return !HAS_EDITOR_DATA && !IS_DEDICATED_SERVER && !IS_CLIENT_ONLY; }
};
