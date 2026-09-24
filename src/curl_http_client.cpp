#include "curl_http_client.h"

#include <curl/curl.h>

#include <climits>
#include <memory>
#include <mutex>
#include <optional>
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

struct MultiDeleter
{
    void operator()(CURLM *multi) const { curl_multi_cleanup(multi); }
};

struct SlistDeleter
{
    void operator()(curl_slist *list) const { curl_slist_free_all(list); }
};

// Takes the easy handle off the multi handle when send() returns, however it
// returns. Declared after both handles, so it runs before either cleanup:
// the order libcurl documents.
class Attachment
{
public:
    Attachment(CURLM *multi_, CURL *easy_)
        : multi(multi_), easy(easy_)
    {
    }
    Attachment(const Attachment &) = delete;
    Attachment &operator=(const Attachment &) = delete;
    ~Attachment() { curl_multi_remove_handle(multi, easy); }

private:
    CURLM *multi;
    CURL *easy;
};

struct Body
{
    std::string bytes;
    bool tooLarge{false};
    bool outOfMemory{false};
};

// libcurl calls back through C frames, which no exception may unwind: a
// failed append aborts the transfer instead, as does a body past the cap.
size_t onBody(char *data, size_t size, size_t count, void *userdata)
{
    auto *body = static_cast<Body *>(userdata);
    const size_t length = size * count;
    if (length > CurlHttpClient::MAX_BODY_BYTES - body->bytes.size()) {
        body->tooLarge = true;
        return 0;
    }
    try {
        body->bytes.append(data, length);
    } catch (...) {
        body->outOfMemory = true;
        return 0;
    }
    return length;
}

// A cancel check that throws is taken as a cancel: send() throws nothing,
// and neither may a libcurl callback.
bool isCancelled(const std::function<bool()> &cancelled) noexcept
{
    try {
        return cancelled && cancelled();
    } catch (...) {
        return true;
    }
}

// Non-zero aborts the transfer with CURLE_ABORTED_BY_CALLBACK.
int onProgress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return isCancelled(*static_cast<const std::function<bool()> *>(clientp)) ? 1 : 0;
}

// The result of the one transfer on multi once it has finished; nullopt if
// libcurl reports none, which it should never do.
std::optional<CURLcode> transferResult(CURLM *multi)
{
    int queued = 0;
    while (const CURLMsg *message = curl_multi_info_read(multi, &queued)) {
        if (message->msg == CURLMSG_DONE) {
            return message->data.result;
        }
    }
    return std::nullopt;
}

}

Result<HttpResponse> CurlHttpClient::send(const HttpRequest &request)
{
    const CURLcode initCode = globalInit();
    if (initCode != CURLE_OK) {
        return {{Error::Internal, std::string("libcurl failed to initialise: ") + curl_easy_strerror(initCode)}, {}};
    }
    if (request.method != "GET" && request.method != "POST") {
        return {{Error::BadInput, "Unsupported HTTP method: " + request.method}, {}};
    }
    if (request.timeout.count() <= 0) {
        return {{Error::Timeout, "No time left to send the request"}, {}};
    }
    if (isCancelled(request.cancelled)) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }

    std::unique_ptr<CURLM, MultiDeleter> multi(curl_multi_init());
    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!multi || !easy) {
        return {{Error::Internal, "libcurl could not create a handle"}, {}};
    }
    std::unique_ptr<curl_slist, SlistDeleter> headers;
    for (const auto &[name, value] : request.headers) {
        const std::string line = name + ": " + value;
        curl_slist *head = headers.release();
        curl_slist *appended = curl_slist_append(head, line.c_str());
        headers.reset(appended ? appended : head);
        if (!appended) {
            return {{Error::Internal, "libcurl could not build the request headers"}, {}};
        }
    }

    // long is 32 bits on Windows; a timeout that does not fit is no limit worth keeping.
    const long timeoutMs = request.timeout.count() > LONG_MAX ? LONG_MAX : static_cast<long>(request.timeout.count());
    Body body;

    CURL *handle = easy.get();
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    if (request.method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
    }
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, ""); // every encoding this libcurl can decode
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_QUICK_EXIT, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    if (request.cancelled) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &request.cancelled);
    }

    if (curl_multi_add_handle(multi.get(), handle) != CURLM_OK) {
        return {{Error::Internal, "libcurl could not start the transfer"}, {}};
    }
    const Attachment attachment(multi.get(), handle);

    // The one transfer runs until it finishes. Between polls the cancel is
    // checked, so it lands within POLL_MS even while libcurl waits for DNS, a
    // connection or the first byte; a poll returns early whenever there is
    // data, or a timer of libcurl's own - the timeout's among them - is due.
    CURLMcode multiCode = CURLM_OK;
    for (;;) {
        int running = 0;
        multiCode = curl_multi_perform(multi.get(), &running);
        if (multiCode != CURLM_OK || running == 0) {
            break;
        }
        if (isCancelled(request.cancelled)) {
            return {{Error::Cancelled, "Cancelled"}, {}};
        }
        multiCode = curl_multi_poll(multi.get(), nullptr, 0, POLL_MS, nullptr);
        if (multiCode != CURLM_OK) {
            break;
        }
    }
    // A multi handle that fails is libcurl's trouble, not the network's.
    if (multiCode != CURLM_OK) {
        return {{Error::Internal, curl_multi_strerror(multiCode)}, {}};
    }
    const std::optional<CURLcode> finished = transferResult(multi.get());
    if (!finished) {
        return {{Error::Internal, "libcurl lost track of the transfer"}, {}};
    }

    const CURLcode code = *finished;
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }
    if (code == CURLE_OPERATION_TIMEDOUT) {
        return {{Error::Timeout, curl_easy_strerror(code)}, {}};
    }
    if (body.tooLarge) {
        return {{Error::Network, "Response too large"}, {}};
    }
    if (body.outOfMemory || code == CURLE_OUT_OF_MEMORY) {
        return {{Error::Internal, "Out of memory"}, {}};
    }
    if (code != CURLE_OK) {
        return {{Error::Network, curl_easy_strerror(code)}, {}};
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    Result<HttpResponse> result{{}, {status, std::move(body.bytes)}};
    if (status >= 400) {
        result.status = {Error::Http, "HTTP " + std::to_string(status)};
    }
    return result;
}

std::shared_ptr<HttpClient> makeCurlHttpClient()
{
    return std::make_shared<CurlHttpClient>();
}

}
