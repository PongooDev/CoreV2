#pragma once

#include <cstddef>
#include <cstdint>

class UClass;
class UFunction;
class UScriptStruct;
class UStruct;

namespace Reflection
{
    inline constexpr int32_t UnresolvedOffset = -2;
    inline constexpr int32_t MissingOffset = -1;

    enum class ETypeKind : uint8_t
    {
        Class,
        ScriptStruct,
    };

    enum class EMemberKind : uint8_t
    {
        Value,
        Bool,
        Struct,
        StructArray,
    };

    using FOwnerGetter = UStruct* (*)();

    class FTypeInfo
    {
    public:
        FTypeInfo(const wchar_t* InPackage, const wchar_t* InNativeName, ETypeKind InKind);

        UStruct* Get();
        UClass* GetClass();
        UScriptStruct* GetScriptStruct();
        const wchar_t* GetNativeName() const { return NativeName; }
        const wchar_t* GetPackage() const { return Package; }

    private:
        const wchar_t* Package;
        const wchar_t* NativeName;
        ETypeKind Kind;
        UStruct* Struct = nullptr;
    };

    class FMemberInfo
    {
    public:
        FMemberInfo(FOwnerGetter InOwner, const wchar_t* InName, size_t InSize, EMemberKind InKind, FOwnerGetter InStruct = nullptr);

        int32_t GetOffsetChecked();
        uint8_t GetFieldMask() const { return FieldMask; }
        uint8_t GetByteMask() const { return ByteMask; }
        bool Resolve();
        const wchar_t* GetName() const { return Name; }

    private:
        friend bool ResolveAll();

        FOwnerGetter Owner;
        const wchar_t* Name;
        size_t Size;
        EMemberKind Kind;
        FOwnerGetter StructType;
        int32_t Offset = UnresolvedOffset;
        uint8_t FieldMask = 0;
        uint8_t ByteMask = 0;
    };

    class FFunctionInfo
    {
    public:
        FFunctionInfo(FOwnerGetter InOwner, const wchar_t* InName);

        UFunction* Get();

    private:
        friend bool ResolveAll();

        FOwnerGetter Owner;
        const wchar_t* Name;
        UFunction* Function = nullptr;
    };

    bool ResolveAll();

    template <typename T>
    T& AccessMember(const void* Object, FMemberInfo& Member)
    {
        return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(Object) + Member.GetOffsetChecked());
    }

    inline bool GetBoolMember(const void* Object, FMemberInfo& Member)
    {
        return (AccessMember<uint8_t>(Object, Member) & Member.GetFieldMask()) != 0;
    }

    inline void SetBoolMember(void* Object, FMemberInfo& Member, bool bValue)
    {
        uint8_t& Byte = AccessMember<uint8_t>(Object, Member);
        Byte = static_cast<uint8_t>((Byte & ~Member.GetFieldMask()) | (bValue ? Member.GetByteMask() : 0));
    }
}

#define DefineUnrealClass(ClassName, Package) \
private: \
    static inline Reflection::FTypeInfo TypeInfo_##ClassName{ TEXT(#Package), TEXT(#ClassName), Reflection::ETypeKind::Class }; \
public: \
    static UClass* StaticClass() { return TypeInfo_##ClassName.GetClass(); } \
    static UStruct* StaticReflectionOwner() { return TypeInfo_##ClassName.Get(); }

#define DefineUnrealStruct(StructName, Package) \
private: \
    static inline Reflection::FTypeInfo TypeInfo_##StructName{ TEXT(#Package), TEXT(#StructName), Reflection::ETypeKind::ScriptStruct }; \
public: \
    static UScriptStruct* StaticStruct() { return TypeInfo_##StructName.GetScriptStruct(); } \
    static UStruct* StaticReflectionOwner() { return TypeInfo_##StructName.Get(); }

#define DefineUProperty(Type, Name) \
private: \
    static inline Reflection::FMemberInfo Member_##Name{ &StaticReflectionOwner, TEXT(#Name), sizeof(Type), Reflection::EMemberKind::Value }; \
public: \
    Type& GetMember_##Name() const { return Reflection::AccessMember<Type>(this, Member_##Name); } \
    void SetMember_##Name(Type const& Value) { GetMember_##Name() = Value; } \
    __declspec(property(get = GetMember_##Name, put = SetMember_##Name)) Type Name;

#define DefineUStructProperty(Type, Name) \
private: \
    static inline Reflection::FMemberInfo Member_##Name{ &StaticReflectionOwner, TEXT(#Name), 0, Reflection::EMemberKind::Struct, &Type::StaticReflectionOwner }; \
public: \
    Type& GetMember_##Name() const { return Reflection::AccessMember<Type>(this, Member_##Name); } \
    __declspec(property(get = GetMember_##Name)) Type Name;

#define DefineUStructArrayProperty(Type, Name) \
private: \
    static inline Reflection::FMemberInfo Member_##Name{ &StaticReflectionOwner, TEXT(#Name), 0, Reflection::EMemberKind::StructArray, &Type::StaticReflectionOwner }; \
public: \
    TStructArray<Type>& GetMember_##Name() const { return Reflection::AccessMember<TStructArray<Type>>(this, Member_##Name); } \
    __declspec(property(get = GetMember_##Name)) TStructArray<Type> Name;

#define DefineBitfieldUProperty(Name) \
private: \
    static inline Reflection::FMemberInfo Member_##Name{ &StaticReflectionOwner, TEXT(#Name), sizeof(uint8_t), Reflection::EMemberKind::Bool }; \
public: \
    bool GetMember_##Name() const { return Reflection::GetBoolMember(this, Member_##Name); } \
    void SetMember_##Name(bool bValue) { Reflection::SetBoolMember(this, Member_##Name, bValue); } \
    __declspec(property(get = GetMember_##Name, put = SetMember_##Name)) bool Name;

#define DefineUFunction(Name) \
private: \
    static inline Reflection::FFunctionInfo Function_##Name{ &StaticReflectionOwner, TEXT(#Name) }; \
public: \
    static UFunction* StaticFunction_##Name() { return Function_##Name.Get(); }
