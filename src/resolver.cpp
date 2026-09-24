#include "ytres/http.h"
#include "ytres/ytres.h"

#include "ascii.h"
#include "innertube.h"
#include "url_parse.h"
#include "visitor_cache.h"
#include "watch_page.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace ytres {

namespace {

using Clock = std::chrono::steady_clock;

const size_t ERROR_EXCERPT_BYTES = 200;

std::int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The start of an error page for the log: one line, cut on a character
// boundary.
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

// "Sign in to confirm you're not a bot" is about who asks, not about the
// video, so other visitor data may get past it. It arrives as LoginRequired
// and only its reason tells it from a real sign-in wall.
bool isBotCheck(const Status &status)
{
    return status.code == Error::LoginRequired && asciiLower(status.message).find("not a bot") != std::string::npos;
}

}

// A Resolver must stay safe to call from several threads at once: the bot
// resolves the next song while one plays. options and http are never
// written after the constructor, HttpClient::send() is safe to call
// concurrently, and the one thing resolves share, the visitor data, sits in
// a VisitorCache that locks for itself and never across a request.
struct Resolver::Impl
{
    // What one resolve carries into each of its requests.
    struct Call
    {
        std::string videoId;
        const Request &request;
        Clock::time_point deadline;
    };

    Options options;
    std::shared_ptr<HttpClient> http;
    VisitorCache visitorCache;

    Result<VideoInfo> resolve(std::string_view urlOrId, const Request &request);
    Result<std::string> visitorDataToSend(const innertube::ClientDef &client, const Call &call);
    Result<std::string> fetchVisitorData(const innertube::ClientDef &client, const Call &call);
    Result<VideoInfo> askPlayer(const innertube::ClientDef &client, const std::string &visitorData, const Call &call) const;
    Result<HttpResponse> send(HttpRequest httpRequest, const Call &call) const;

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
    impl->http = options.http ? options.http : makeCurlHttpClient();
    impl->options = std::move(options);
}

Resolver::Resolver(Resolver &&other) noexcept = default;
Resolver &Resolver::operator=(Resolver &&other) noexcept = default;
Resolver::~Resolver() = default;

Result<VideoInfo> Resolver::resolve(std::string_view urlOrId, const Request &request)
{
    // No exception crosses the public API. The enum has no code for a bug or
    // an exhausted heap; Parse is the nearest.
    try {
        if (!impl) {
            return {{Error::BadInput, "This Resolver has been moved from"}, {}};
        }
        return impl->resolve(urlOrId, request);
    } catch (const std::exception &e) {
        return {{Error::Parse, std::string("Unexpected failure: ") + e.what()}, {}};
    } catch (...) {
        return {{Error::Parse, "Unexpected failure"}, {}};
    }
}

Result<VideoInfo> Resolver::Impl::resolve(std::string_view urlOrId, const Request &request)
{
    // steady_clock counts nanoseconds in 64 bits, so an extreme deadline
    // wraps around: milliseconds::max() as "no deadline" into the past, a
    // hugely negative one into no deadline at all.
    const Clock::time_point deadline =
        Clock::now() + std::clamp<std::chrono::milliseconds>(request.deadline, std::chrono::milliseconds::zero(),
                                                              std::chrono::hours(24));

    const Result<std::string> videoId = parseVideoId(urlOrId);
    if (!videoId) {
        return {videoId.status, {}};
    }
    // One client until M2 brings the ladder.
    const innertube::ClientDef *client = options.clients.empty() ? nullptr : innertube::findClient(options.clients.front());
    if (!client) {
        return {{Error::BadInput, "No InnerTube client to ask"}, {}};
    }
    const Call call{videoId.value, request, deadline};

    const Result<std::string> visitorData = visitorDataToSend(*client, call);
    if (!visitorData) {
        return {visitorData.status, {}};
    }
    Result<VideoInfo> info = askPlayer(*client, visitorData.value, call);
    if (!isBotCheck(info.status)) {
        return info;
    }

    // The cached visitor data may have worn out, or YouTube may have taken
    // against this one: fetch the page once more and ask once more, within
    // the same deadline. A page that brings nothing new leaves the bot check
    // as the answer.
    log(LogLevel::Warning, "Bot check for " + call.videoId + "; fetching fresh visitor data to ask once more");
    const Result<std::string> fresh = fetchVisitorData(*client, call);
    if (!fresh) {
        return {fresh.status, {}};
    }
    if (fresh.value.empty()) {
        return info;
    }
    return askPlayer(*client, fresh.value, call);
}

// The visitor data for a player request: the cached value while it is fresh,
// else what the watch page has now. When the page brings none, a stale value
// still goes out, since an old visitor id beats none.
Result<std::string> Resolver::Impl::visitorDataToSend(const innertube::ClientDef &client, const Call &call)
{
    VisitorCache::Entry cached = visitorCache.get(Clock::now());
    if (cached.fresh) {
        return {{}, std::move(cached.value)};
    }
    log(LogLevel::Debug, cached.value.empty() ? "No visitor data yet; fetching the watch page"
                                              : "The visitor data has expired; fetching the watch page");
    Result<std::string> fetched = fetchVisitorData(client, call);
    if (fetched && fetched.value.empty()) {
        fetched.value = visitorCache.get(Clock::now()).value; // another thread may have stored one meanwhile
    }
    return fetched;
}

// Fetches the watch page and caches the visitor data in it; the value is
// empty when the page has none. Without visitor data most player requests
// meet a bot check, but the request is still worth making (some videos
// answer anyway), so a page that fails or lacks it costs a warning, not the
// resolve, and leaves the cache as it was. Only a cancel ends the resolve
// here; a spent deadline ends it at the next send().
Result<std::string> Resolver::Impl::fetchVisitorData(const innertube::ClientDef &client, const Call &call)
{
    const Result<HttpResponse> page = send(watchpage::request(client, call.videoId), call);
    if (page.status.code == Error::Cancelled) {
        return {page.status, {}};
    }
    if (!page) {
        log(LogLevel::Warning, "No watch page (" + page.status.message + "), so no new visitor data");
        return {};
    }
    watchpage::VisitorData found = watchpage::visitorData(page.value.body);
    if (found.value.empty() && !found.refused.empty()) {
        log(LogLevel::Warning, "Refused the watch page's visitor data (" + found.refused + "), so none is sent");
    } else if (found.value.empty()) {
        log(LogLevel::Warning, "The watch page has no visitor data (HTTP " + std::to_string(page.value.status) + ")");
    }
    visitorCache.put(found.value, Clock::now());
    return {{}, std::move(found.value)};
}

Result<VideoInfo> Resolver::Impl::askPlayer(const innertube::ClientDef &client, const std::string &visitorData,
                                            const Call &call) const
{
    log(LogLevel::Debug, "Asking the " + std::string(client.key) + " client for " + call.videoId);
    const Result<HttpResponse> response =
        send(innertube::playerRequest(client, call.videoId, options.language, visitorData), call);
    if (!response) {
        return {response.status, {}};
    }
    // CurlHttpClient fails a status of 400 and up itself; this also catches a
    // redirect it did not follow, and whatever another client lets through.
    if (response.value.status < 200 || response.value.status >= 300) {
        return {{Error::Http, "YouTube answered HTTP " + std::to_string(response.value.status)}, {}};
    }

    Result<VideoInfo> info = innertube::parsePlayerResponse(response.value.body, call.videoId, nowUnix(),
                                                            [this](LogLevel level, std::string_view text) { log(level, text); });
    if (info) {
        log(LogLevel::Debug, call.videoId + ": " + std::to_string(info.value.formats.size()) + " formats, expiring at "
                                 + std::to_string(info.value.expiresAtUnix));
    }
    return info;
}

// Sends one request within what is left of the resolve's deadline.
Result<HttpResponse> Resolver::Impl::send(HttpRequest httpRequest, const Call &call) const
{
    if (call.request.cancelled && call.request.cancelled()) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }
    const auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(call.deadline - Clock::now());
    if (timeLeft.count() <= 0) {
        return {{Error::Timeout, "The resolve ran out of time"}, {}};
    }
    httpRequest.timeout = std::min(options.requestTimeout, timeLeft);
    httpRequest.cancelled = call.request.cancelled;
    Result<HttpResponse> response = http->send(httpRequest);
    // An error page can say what went wrong, but it is YouTube's text, not
    // ours: the log may have it, the Status message may not.
    if (response.status.code == Error::Http && !response.value.body.empty()) {
        log(LogLevel::Debug, response.status.message + " from " + httpRequest.url + ": "
                                 + bodyExcerpt(response.value.body, ERROR_EXCERPT_BYTES));
    }
    return response;
}

}
