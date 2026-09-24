#include "ytres/http.h"
#include "ytres/ytres.h"

#include "innertube.h"
#include "url_parse.h"
#include "visitor_cache.h"
#include "watch_page.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>
#include <vector>

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

// Whether a client's failure passes the video on to the next client. Only a
// failure about the client does - how YouTube treats it, or what it can
// read. The video, the network, the call and the library would fail the same
// way whoever asked.
bool triesNextClient(Error error)
{
    switch (error) {
    case Error::BotCheck:     // its one retry with fresh visitor data is spent
    case Error::NoFormats:    // ciphered, SABR only, or none at all
    case Error::Http:
    case Error::Parse:
    case Error::PlayerScript: // a JS tier's failure is its client's
        return true;
    case Error::Ok:
    case Error::Cancelled:
    case Error::Timeout:
    case Error::Network:
    case Error::Internal:
    case Error::Unavailable:
    case Error::AgeRestricted:
    case Error::GeoBlocked:
    case Error::LoginRequired:
    case Error::BadInput:
        return false;
    }
    return false;
}

// How much a failure that passes the video on tells the caller, for the code
// a ladder that runs out reports. The bot check comes first: the caller must
// fall back and should stop asking for a while, and a client that can never
// play here (web, without a PO Token) must not bury it under NoFormats.
int weight(Error error)
{
    switch (error) {
    case Error::BotCheck:
        return 3;
    case Error::Http: // a 429 or a 5xx is YouTube's view of the caller too
        return 2;
    case Error::Parse:
        return 1;
    default: // NoFormats, PlayerScript: what the client can read, known in advance
        return 0;
    }
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
    // No exception crosses the public API. One that gets this far is a bug
    // or an exhausted heap, never YouTube's doing: Internal, so that nobody
    // reads it as a client that stopped working.
    try {
        if (!impl) {
            return {{Error::BadInput, "This Resolver has been moved from"}, {}};
        }
        return impl->resolve(urlOrId, request);
    } catch (const std::exception &e) {
        return {{Error::Internal, std::string("Unexpected failure: ") + e.what()}, {}};
    } catch (...) {
        return {{Error::Internal, "Unexpected failure"}, {}};
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
    // The whole ladder up front, so that a bad list fails before anything is sent.
    std::vector<const innertube::ClientDef *> ladder;
    for (ClientId id : options.clients) {
        const innertube::ClientDef *client = innertube::findClient(id);
        if (!client) {
            return {{Error::BadInput, "Unknown InnerTube client " + std::to_string(static_cast<int>(id))}, {}};
        }
        ladder.push_back(client);
    }
    if (ladder.empty()) {
        return {{Error::BadInput, "No InnerTube client to ask"}, {}};
    }
    const Call call{videoId.value, request, deadline};

    Result<std::string> visitorData = visitorDataToSend(*ladder.front(), call);
    if (!visitorData) {
        return {visitorData.status, {}};
    }
    bool refreshed = false;
    std::string answers;        // "<client>: <message>" for each client that passed the video on
    Error reported = Error::Ok; // the most telling of their codes
    for (size_t i = 0; i < ladder.size(); ++i) {
        const innertube::ClientDef &client = *ladder[i];
        Result<VideoInfo> info = askPlayer(client, visitorData.value, call);
        // The cached visitor data may have worn out, or YouTube may have
        // taken against this one: try once to fetch fresh visitor data and,
        // when the page brings some, ask once more, within the same deadline.
        // Once per resolve: the fresh value serves the clients after this one
        // too.
        if (info.status.code == Error::BotCheck && !refreshed) {
            refreshed = true;
            log(LogLevel::Warning, "Bot check for " + call.videoId + " on the " + client.key
                                       + " client; fetching fresh visitor data to ask once more");
            Result<std::string> fresh = fetchVisitorData(client, call);
            if (!fresh) {
                return {fresh.status, {}};
            }
            if (!fresh.value.empty()) {
                visitorData.value = std::move(fresh.value);
                info = askPlayer(client, visitorData.value, call);
            }
        }
        if (info) {
            return info;
        }
        const Error code = info.status.code;
        const std::string answer = std::string(client.key) + ": " + info.status.message;
        if (!triesNextClient(code)) {
            log(LogLevel::Debug, std::string("The ") + client.key + " client's answer ends the resolve: " + info.status.message);
            // The call's cancel and the video's own failures keep their code
            // and YouTube's words. A network, deadline or library failure
            // after earlier clients passed the video on keeps what they
            // answered, and an earlier bot check outranks the network and the
            // deadline: the caller falls back either way, but only the bot
            // check tells it to leave the library alone for a while.
            if (answers.empty() || (code != Error::Network && code != Error::Timeout && code != Error::Internal)) {
                return info;
            }
            const bool botCheckFirst = reported == Error::BotCheck && code != Error::Internal;
            return {{botCheckFirst ? Error::BotCheck : code, answers + "; " + answer}, {}};
        }
        if (reported == Error::Ok || weight(code) > weight(reported)) {
            reported = code;
        }
        answers += (answers.empty() ? "" : "; ") + answer;
        if (i + 1 < ladder.size()) {
            log(LogLevel::Warning, std::string("The ") + client.key + " client failed for " + call.videoId + " ("
                                       + info.status.message + "); asking the " + ladder[i + 1]->key + " client next");
        }
    }
    return {{reported, std::move(answers)}, {}};
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
