#include "ytres/http.h"
#include "ytres/ytres.h"

#include "curl_http_client.h"
#include "innertube.h"
#include "url_parse.h"
#include "watch_page.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace ytres {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

// A Resolver must stay safe to call from several threads at once: the bot
// resolves the next song while one plays. Impl is never written after the
// constructor and HttpClient::send() is safe to call concurrently, so
// nothing here needs a lock. A cache added later will.
struct Resolver::Impl
{
    Options options;
    std::shared_ptr<HttpClient> http;

    Result<VideoInfo> resolve(std::string_view urlOrId, const Request &request) const;
    Result<std::string> fetchVisitorData(const innertube::ClientDef &client, const std::string &videoId,
                                         const Request &request, Clock::time_point deadline) const;
    Result<HttpResponse> send(HttpRequest httpRequest, const Request &request, Clock::time_point deadline) const;

    void log(LogLevel level, std::string_view text) const
    {
        if (options.log) {
            options.log(level, text);
        }
    }
};

Resolver::Resolver()
    : Resolver(Options{})
{
}

Resolver::Resolver(Options options)
    : impl(std::make_unique<Impl>())
{
    impl->http = options.http ? options.http : std::make_shared<CurlHttpClient>();
    impl->options = std::move(options);
}

Resolver::~Resolver() = default;

Result<VideoInfo> Resolver::resolve(std::string_view urlOrId, const Request &request)
{
    // No exception crosses the public API. The enum has no code for a bug or
    // an exhausted heap; Parse is the nearest.
    try {
        return impl->resolve(urlOrId, request);
    } catch (const std::exception &e) {
        return {{Error::Parse, std::string("Unexpected failure: ") + e.what()}, {}};
    } catch (...) {
        return {{Error::Parse, "Unexpected failure"}, {}};
    }
}

Result<VideoInfo> Resolver::Impl::resolve(std::string_view urlOrId, const Request &request) const
{
    // steady_clock counts nanoseconds in 64 bits: milliseconds::max() as
    // "no deadline" would wrap into the past.
    const Clock::time_point deadline =
        Clock::now() + std::min<std::chrono::milliseconds>(request.deadline, std::chrono::hours(24));

    const Result<std::string> videoId = parseVideoId(urlOrId);
    if (!videoId) {
        return {videoId.status, {}};
    }
    // One client until M2 brings the ladder.
    const innertube::ClientDef *client = options.clients.empty() ? nullptr : innertube::findClient(options.clients.front());
    if (!client) {
        return {{Error::BadInput, "No InnerTube client to ask"}, {}};
    }

    const Result<std::string> visitorData = fetchVisitorData(*client, videoId.value, request, deadline);
    if (!visitorData) {
        return {visitorData.status, {}};
    }

    log(LogLevel::Debug, "Asking the " + std::string(client->key) + " client for " + videoId.value);
    const Result<HttpResponse> response =
        send(innertube::playerRequest(*client, videoId.value, options.language, visitorData.value), request, deadline);
    if (!response) {
        return {response.status, {}};
    }
    // CurlHttpClient fails a status of 400 and up itself; this also catches a
    // redirect it did not follow, and whatever another client lets through.
    if (response.value.status < 200 || response.value.status >= 300) {
        return {{Error::Http, "YouTube answered HTTP " + std::to_string(response.value.status)}, {}};
    }

    Result<VideoInfo> info = innertube::parsePlayerResponse(response.value.body, videoId.value, nowUnix(),
                                                            [this](LogLevel level, std::string_view text) { log(level, text); });
    if (info) {
        log(LogLevel::Debug, videoId.value + ": " + std::to_string(info.value.formats.size()) + " formats, expiring at "
                                 + std::to_string(info.value.expiresAtUnix));
    }
    return info;
}

// Without visitor data most player requests meet a bot check, but with none
// the request is still worth making (some videos answer anyway), so a page
// that fails or lacks it costs a warning, not the resolve. Only a cancel
// ends the resolve here; a spent deadline ends it at the next send().
Result<std::string> Resolver::Impl::fetchVisitorData(const innertube::ClientDef &client, const std::string &videoId,
                                                     const Request &request, Clock::time_point deadline) const
{
    const Result<HttpResponse> page = send(watchpage::request(client, videoId), request, deadline);
    if (page.status.code == Error::Cancelled) {
        return {page.status, {}};
    }
    if (!page) {
        log(LogLevel::Warning, "No watch page (" + page.status.message + "), so no visitor data");
        return {};
    }
    std::string visitorData = watchpage::visitorData(page.value.body);
    if (visitorData.empty()) {
        log(LogLevel::Warning, "The watch page has no visitor data (HTTP " + std::to_string(page.value.status) + ")");
    }
    return {{}, std::move(visitorData)};
}

// Sends one request within what is left of the resolve's deadline.
Result<HttpResponse> Resolver::Impl::send(HttpRequest httpRequest, const Request &request, Clock::time_point deadline) const
{
    if (request.cancelled && request.cancelled()) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }
    const auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (timeLeft.count() <= 0) {
        return {{Error::Timeout, "The resolve ran out of time"}, {}};
    }
    httpRequest.timeout = std::min(options.requestTimeout, timeLeft);
    httpRequest.cancelled = request.cancelled;
    return http->send(httpRequest);
}

}
