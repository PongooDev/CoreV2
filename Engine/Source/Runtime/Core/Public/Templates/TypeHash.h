#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

inline uint32_t HashCombine(uint32_t A, uint32_t C)
{
    uint32_t B = 0x9e3779b9;
    A += B;

    A -= B; A -= C; A ^= (C >> 13);
    B -= C; B -= A; B ^= (A << 8);
    C -= A; C -= B; C ^= (B >> 13);
    A -= B; A -= C; A ^= (C >> 12);
    B -= C; B -= A; B ^= (A << 16);
    C -= A; C -= B; C ^= (B >> 5);
    A -= B; A -= C; A ^= (C >> 3);
    B -= C; B -= A; B ^= (A << 10);
    C -= A; C -= B; C ^= (B >> 15);

    return C;
}

inline uint32_t PointerHash(const void* Key, uint32_t C = 0)
{
    const uintptr_t PtrInt = reinterpret_cast<uintptr_t>(Key) >> 4;
    return HashCombine(static_cast<uint32_t>(PtrInt), C);
}

inline uint32_t GetTypeHash(const uint8_t A) { return A; }
inline uint32_t GetTypeHash(const int8_t A) { return static_cast<uint32_t>(A); }
inline uint32_t GetTypeHash(const uint16_t A) { return A; }
inline uint32_t GetTypeHash(const int16_t A) { return static_cast<uint32_t>(A); }
inline uint32_t GetTypeHash(const int32_t A) { return static_cast<uint32_t>(A); }
inline uint32_t GetTypeHash(const uint32_t A) { return A; }

inline uint32_t GetTypeHash(const uint64_t A)
{
    return static_cast<uint32_t>(A) + (static_cast<uint32_t>(A >> 32) * 23);
}

inline uint32_t GetTypeHash(const int64_t A)
{
    return static_cast<uint32_t>(A) + (static_cast<uint32_t>(A >> 32) * 23);
}

inline uint32_t GetTypeHash(const float Value)
{
    uint32_t Bits;
    memcpy(&Bits, &Value, sizeof(Bits));
    return Bits;
}

template <typename T>
inline uint32_t GetTypeHash(T* A)
{
    return PointerHash(A);
}

template <typename EnumType, std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
inline uint32_t GetTypeHash(EnumType E)
{
    return GetTypeHash(static_cast<std::underlying_type_t<EnumType>>(E));
}
