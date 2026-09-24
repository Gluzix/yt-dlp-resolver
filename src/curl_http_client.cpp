#include "curl_http_client.h"

#include <curl/curl.h>

#include <climits>
#include <memory>
#include <mutex>
#include <string>

namespace ytres {

namespace {

CURLcode globalInit()
{
    static std::once_flag once;
    static CURLcode result = CURLE_OK;
    std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result;
}

struct EasyDeleter
{
    void operator()(CURL *easy) const { curl_easy_cleanup(easy); }
};

struct SlistDeleter
{
    void operator()(curl_slist *list) const { curl_slist_free_all(list); }
};

// libcurl calls back through C frames, which no exception may unwind: a
// failed append aborts the transfer instead.
size_t onBody(char *data, size_t size, size_t count, void *userdata)
{
    try {
        static_cast<std::string *>(userdata)->append(data, size * count);
    } catch (...) {
        return 0;
    }
    return size * count;
}

// Non-zero aborts the transfer with CURLE_ABORTED_BY_CALLBACK.
int onProgress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto *cancelled = static_cast<const std::function<bool()> *>(clientp);
    try {
        return (*cancelled)() ? 1 : 0;
    } catch (...) {
        return 1; // a cancel check that throws is taken as a cancel
    }
}

// The start of an error page for a message a user may see: one line, cut on
// a character boundary.
std::string bodyExcerpt(const std::string &body, size_t maxBytes)
{
    size_t end = body.size() < maxBytes ? body.size() : maxBytes;
    while (end > 0 && end < body.size() && (static_cast<unsigned char>(body[end]) & 0xC0) == 0x80) {
        --end; // never split a UTF-8 sequence
    }
    std::string excerpt = body.substr(0, end);
    for (char &c : excerpt) {
        if (static_cast<unsigned char>(c) < 0x20) {
            c = ' ';
        }
    }
    return excerpt;
}

}

Result<HttpResponse> CurlHttpClient::send(const HttpRequest &request)
{
    const CURLcode initCode = globalInit();
    if (initCode != CURLE_OK) {
        return {{Error::Network, std::string("libcurl failed to initialise: ") + curl_easy_strerror(initCode)}, {}};
    }
    if (request.method != "GET" && request.method != "POST") {
        return {{Error::BadInput, "Unsupported HTTP method: " + request.method}, {}};
    }
    if (request.timeout.count() <= 0) {
        return {{Error::Timeout, "No time left to send the request"}, {}};
    }
    if (request.cancelled && request.cancelled()) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }

    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!easy) {
        return {{Error::Network, "libcurl could not create a handle"}, {}};
    }
    std::unique_ptr<curl_slist, SlistDeleter> headers;
    for (const auto &[name, value] : request.headers) {
        const std::string line = name + ": " + value;
        curl_slist *head = headers.release();
        curl_slist *appended = curl_slist_append(head, line.c_str());
        headers.reset(appended ? appended : head);
        if (!appended) {
            return {{Error::Network, "libcurl could not build the request headers"}, {}};
        }
    }

    // long is 32 bits on Windows; a timeout that does not fit is no limit worth keeping.
    const long timeoutMs = request.timeout.count() > LONG_MAX ? LONG_MAX : static_cast<long>(request.timeout.count());
    std::string body;
    char errorBuffer[CURL_ERROR_SIZE] = {};

    CURL *handle = easy.get();
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    if (request.method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
    }
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    if (request.cancelled) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &request.cancelled);
    }

    const CURLcode code = curl_easy_perform(handle);
    const std::string detail = errorBuffer[0] ? std::string(errorBuffer) : std::string(curl_easy_strerror(code));
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        return {{Error::Timeout, detail}, {}};
    }
    if (code != CURLE_OK) {
        return {{Error::Network, std::string(curl_easy_strerror(code)) + (errorBuffer[0] ? ": " + detail : "")}, {}};
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    Result<HttpResponse> result{{}, {status, std::move(body)}};
    if (status >= 400) {
        result.status = {Error::Http, "HTTP " + std::to_string(status) + ": " + bodyExcerpt(result.value.body, ERROR_BODY_PREFIX)};
    }
    return result;
}

}
