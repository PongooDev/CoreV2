#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"

class IHttpBase
{
public:
    virtual FString GetURL() = 0;
    virtual FString GetURLParameter(const FString& ParameterName) = 0;
    virtual FString GetHeader(const FString& HeaderName) = 0;
    virtual TArray<FString> GetAllHeaders() = 0;
    virtual FString GetContentType() = 0;
    virtual int32_t GetContentLength() = 0;
    virtual const TArray<uint8_t>& GetContent() = 0;
    virtual ~IHttpBase() {};
};
