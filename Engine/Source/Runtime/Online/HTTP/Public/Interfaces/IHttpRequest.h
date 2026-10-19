#pragma once

#include "Interfaces/IHttpBase.h"
#include "Templates/SharedPointerInternals.h"

namespace EHttpRequestStatus
{
    enum Type
    {
        NotStarted,
        Processing,
        Failed,
        Failed_ConnectionError,
        Succeeded,
    };
}

class IHttpRequest;
class IHttpResponse;
class FHttpRequestCompleteDelegate;
class FHttpRequestProgressDelegate;

typedef TSharedPtr<IHttpRequest> FHttpRequestPtr;
typedef TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> FHttpResponsePtr;

class IHttpRequest : public IHttpBase
{
public:
    virtual FString GetVerb() = 0;
    virtual void SetVerb(const FString& Verb) = 0;
    virtual void SetURL(const FString& URL) = 0;
    virtual void SetContent(const TArray<uint8_t>& ContentPayload) = 0;
    virtual void SetContentAsString(const FString& ContentString) = 0;
    virtual void SetHeader(const FString& HeaderName, const FString& HeaderValue) = 0;
    virtual void AppendToHeader(const FString& HeaderName, const FString& AdditionalHeaderValue) = 0;
    virtual bool ProcessRequest() = 0;
    virtual FHttpRequestCompleteDelegate& OnProcessRequestComplete() = 0;
    virtual FHttpRequestProgressDelegate& OnRequestProgress() = 0;
    virtual void CancelRequest() = 0;
    virtual EHttpRequestStatus::Type GetStatus() = 0;
    virtual const FHttpResponsePtr GetResponse() const = 0;
    virtual void Tick(float DeltaSeconds) = 0;
    virtual float GetElapsedTime() = 0;
    virtual ~IHttpRequest() {};
};
