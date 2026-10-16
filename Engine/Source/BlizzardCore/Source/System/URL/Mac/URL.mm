#include <cstdint>
#include <cstdlib>
#include <cstring>
#import <Foundation/Foundation.h>
#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"

// HTTP downloads through NSURLSession. The callback sees the same sequence as with WinINet:
// ConnectedOK and GetHTTPStatusCode when the response arrives, then for a 2xx response
// GetLastModified and GetContentLength, WantData, ReceivedData for each chunk with Stop checked
// after it, and finally Completed with the status, after which the request is deleted. Callbacks
// run on a background queue, as the WinINet ones run on pool threads.
//
// HttpRequest::m_request holds the retained NSURLSessionDataTask.

using Blizzard::URL::DownloadCallback;
using Blizzard::URL::HttpRequest;

@interface BlizzardURLDelegate : NSObject <NSURLSessionDataDelegate>
@end

static NSURLSession* s_session;
static NSMutableDictionary<NSNumber*, NSValue*>* s_requests;

static HttpRequest* RequestForTask(NSURLSessionTask* task) {
    @synchronized(s_requests) {
        return static_cast<HttpRequest*>([s_requests[@(task.taskIdentifier)] pointerValue]);
    }
}

@implementation BlizzardURLDelegate

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    HttpRequest* request = RequestForTask(dataTask);

    if (!request) {
        completionHandler(NSURLSessionResponseCancel);
        return;
    }

    DownloadCallback* callback = request->m_callback;
    NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<NSHTTPURLResponse*>(response) : nil;
    int32_t statusCode = http ? static_cast<int32_t>(http.statusCode) : 0;

    if (!request->m_connected) {
        callback->ConnectedOK();
        request->m_connected = true;
    }

    callback->GetHTTPStatusCode(statusCode);

    if (statusCode >= 200 && statusCode < 300) {
        NSString* lastModified = [http valueForHTTPHeaderField:@"Last-Modified"];

        if (lastModified) {
            callback->GetLastModified(lastModified.UTF8String);
        }

        NSString* contentLength = [http valueForHTTPHeaderField:@"Content-Length"];

        if (contentLength) {
            callback->GetContentLength(static_cast<uint32_t>(atol(contentLength.UTF8String)));
        }

        if (!callback->WantData()) {
            request->m_done = true;
            completionHandler(NSURLSessionResponseCancel);
            return;
        }

        completionHandler(NSURLSessionResponseAllow);
        return;
    }

    switch (statusCode) {
        case 304:
            request->m_status = DownloadCallback::DOWNLOAD_NOT_MODIFIED;
            break;
        case 400:
            request->m_status = DownloadCallback::DOWNLOAD_BAD_REQUEST;
            break;
        case 403:
            request->m_status = DownloadCallback::DOWNLOAD_FORBIDDEN;
            break;
        case 404:
            request->m_status = DownloadCallback::DOWNLOAD_NOT_FOUND;
            break;
        default:
            request->m_status = DownloadCallback::DOWNLOAD_FAILED;
            break;
    }

    request->m_done = true;
    completionHandler(NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    HttpRequest* request = RequestForTask(dataTask);

    if (!request || request->m_done) {
        return;
    }

    DownloadCallback* callback = request->m_callback;

    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        if (request->m_done) {
            *stop = YES;
            return;
        }

        if (!callback->ReceivedData(bytes, static_cast<int32_t>(range.length))) {
            request->m_status = DownloadCallback::DOWNLOAD_ABORTED;
            request->m_done = true;
        }

        if (callback->Stop()) {
            request->m_status = DownloadCallback::DOWNLOAD_STOPPED;
            request->m_done = true;
        }
    }];

    if (request->m_done) {
        [dataTask cancel];
    }
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    HttpRequest* request = RequestForTask(task);

    if (!request) {
        return;
    }

    @synchronized(s_requests) {
        [s_requests removeObjectForKey:@(task.taskIdentifier)];
    }

    // A cancel issued by this delegate already set the status.
    if (error && !request->m_done) {
        switch (error.code) {
            case NSURLErrorTimedOut:
                request->m_status = DownloadCallback::DOWNLOAD_TIMEOUT;
                break;
            case NSURLErrorCannotFindHost:
            case NSURLErrorDNSLookupFailed:
                request->m_status = DownloadCallback::DOWNLOAD_NAME_NOT_RESOLVED;
                break;
            case NSURLErrorCannotConnectToHost:
            case NSURLErrorNotConnectedToInternet:
                request->m_status = DownloadCallback::DOWNLOAD_CANNOT_CONNECT;
                break;
            default:
                request->m_status = DownloadCallback::DOWNLOAD_FAILED;
                break;
        }
    }

    request->m_done = true;
    request->m_callback->Completed(request->m_status);
    delete request;
}

@end

Blizzard::URL::HttpRequest::~HttpRequest() {
    if (this->m_request) {
        CFRelease(this->m_request);
        this->m_request = nullptr;
    }
}

Blizzard::URL::HttpRequest::HttpRequest() {
    this->m_connectEvent = nullptr;
    this->m_requestEvent = nullptr;
    this->m_completeEvent = nullptr;
    this->m_dataEvent = nullptr;
    this->m_session = nullptr;
    this->m_connect = nullptr;
    this->m_request = nullptr;
    this->m_state = 0;
    this->m_done = false;
    this->m_connected = false;
    this->m_callback = nullptr;
    this->m_error = 0;
    this->m_status = Blizzard::URL::DownloadCallback::DOWNLOAD_SUCCESS;
}

static NSURLSession* GetSession() {
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        configuration.HTTPAdditionalHeaders = @{ @"User-Agent": @"Blizzard Web Client" };

        NSOperationQueue* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        queue.name = @"Blizzard Web Client";

        s_requests = [[NSMutableDictionary alloc] init];
        s_session = [NSURLSession sessionWithConfiguration:configuration delegate:[[BlizzardURLDelegate alloc] init] delegateQueue:queue];
    });

    return s_session;
}

bool Blizzard::URL::DownloadURL(const char* url, DownloadCallback* callback) {
    @autoreleasepool {
        NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url]];

        if (!target || !target.host) {
            return false;
        }

        NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:target];

        int32_t timeout = callback->GetTimeout();

        if (timeout > 0) {
            urlRequest.timeoutInterval = timeout / 1000.0;
        }

        const char* postData = callback->GetPostData();

        if (postData) {
            urlRequest.HTTPMethod = @"POST";
            const char* contentType = callback->GetPostContentType();
            [urlRequest setValue:[NSString stringWithUTF8String:contentType ? contentType : "text/html"] forHTTPHeaderField:@"Content-Type"];
            urlRequest.HTTPBody = [NSData dataWithBytes:postData length:strlen(postData)];
        } else if (callback->WantData()) {
            urlRequest.HTTPMethod = @"GET";
        } else {
            urlRequest.HTTPMethod = @"HEAD";
        }

        const char* ifModifiedSince = callback->WantIfModifiedSince();

        if (ifModifiedSince) {
            [urlRequest setValue:[NSString stringWithUTF8String:ifModifiedSince] forHTTPHeaderField:@"If-Modified-Since"];
        }

        DownloadCallback::ByteRange range = callback->GetByteRange();

        if (range.m_start != -1 || range.m_end != -1) {
            [urlRequest setValue:[NSString stringWithFormat:@"bytes=%lld-%lld", range.m_start, range.m_end] forHTTPHeaderField:@"Range"];
        }

        DownloadCallback::Cookie cookie = callback->GetCookie();

        if (cookie.m_name && *cookie.m_name && cookie.m_value) {
            [urlRequest setValue:[NSString stringWithFormat:@"%s=%s", cookie.m_name, cookie.m_value] forHTTPHeaderField:@"Cookie"];
        }

        HttpRequest* request = new HttpRequest();
        request->m_callback = callback;
        request->m_state = 1;

        NSURLSessionDataTask* task = [GetSession() dataTaskWithRequest:urlRequest];
        request->m_request = const_cast<void*>(CFBridgingRetain(task));

        @synchronized(s_requests) {
            s_requests[@(task.taskIdentifier)] = [NSValue valueWithPointer:request];
        }

        [task resume];

        return true;
    }
}
