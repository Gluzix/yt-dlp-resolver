#include "ytres/http.h"
#include "ytres/ytres.h"

#include "continuation.h"
#include "innertube.h"
#include "playlist.h"
#include "search.h"
#include "url_parse.h"
#include "visitor_cache.h"
#include "watch_page.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytres {

namespace {

using Clock = std::chrono::steady_clock;

const size_t ERROR_EXCERPT_BYTES = 200;

// A search reads no more pages than this, whatever max asks for: about two
// hundred videos, and a stop for a feed that would hand out pages forever.
const int MAX_SEARCH_PAGES = 10;

// A playlist reads no more pages than this, whatever max asks for: twenty
// thousand videos. An ordinary playlist holds fewer, but a channel's uploads
// (UU...) can hold more, and those stop here with a warning. The library's
// own stop for a feed that hands out fresh tokens for ever; yt-dlp's
// _entries has only the other, a token that repeats.
const int MAX_PLAYLIST_PAGES = 200;

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
// concurrently, and the one thing calls share, the visitor data, sits in a
// VisitorCache that locks for itself and never across a request.
struct Resolver::Impl
{
    // What one call carries into each of its requests.
    struct Call
    {
        std::string videoId; // empty for a search or a playlist, which are about no one video
        const Request &request;
        Clock::time_point deadline;
    };

    Options options;
    std::shared_ptr<HttpClient> http;
    VisitorCache visitorCache;

    Result<VideoInfo> resolve(std::string_view urlOrId, const Request &request);
    Result<std::vector<SearchResult>> search(std::string_view query, std::size_t max, const Request &request);
    Result<Playlist> playlist(std::string_view urlOrId, std::size_t max, const Request &request);
    Result<Clock::time_point> callDeadline(const Request &request) const;
    Result<std::string> visitorDataToSend(const innertube::ClientDef &client, const Call &call);
    Result<std::string> fetchVisitorData(const innertube::ClientDef &client, const Call &call);
    Result<VideoInfo> askPlayer(const innertube::ClientDef &client, const std::string &visitorData, const Call &call) const;
    Result<innertube::SearchPage> askSearch(const innertube::ClientDef &client, const std::string &query,
                                            const std::string &visitorData, const innertube::Continuation &continuation,
                                            const Call &call) const;
    Result<innertube::PlaylistPage> askPlaylist(const innertube::ClientDef &client, const std::string &playlistId,
                                                const std::string &visitorData,
                                                const innertube::Continuation &continuation, const Call &call) const;
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

Result<std::vector<SearchResult>> Resolver::search(std::string_view query, std::size_t max, const Request &request)
{
    // The same net as resolve()'s, for the same reasons.
    try {
        if (!impl) {
            return {{Error::BadInput, "This Resolver has been moved from"}, {}};
        }
        return impl->search(query, max, request);
    } catch (const std::exception &e) {
        return {{Error::Internal, std::string("Unexpected failure: ") + e.what()}, {}};
    } catch (...) {
        return {{Error::Internal, "Unexpected failure"}, {}};
    }
}

Result<Playlist> Resolver::playlist(std::string_view urlOrId, std::size_t max, const Request &request)
{
    // The same net as resolve()'s, for the same reasons.
    try {
        if (!impl) {
            return {{Error::BadInput, "This Resolver has been moved from"}, {}};
        }
        return impl->playlist(urlOrId, max, request);
    } catch (const std::exception &e) {
        return {{Error::Internal, std::string("Unexpected failure: ") + e.what()}, {}};
    } catch (...) {
        return {{Error::Internal, "Unexpected failure"}, {}};
    }
}

// The opening every call shares: a request timeout that can be met, and the
// call's deadline as a point in time.
Result<Clock::time_point> Resolver::Impl::callDeadline(const Request &request) const
{
    // Every request would fail as a Timeout, which reads as the network's
    // fault; the fault is the caller's.
    if (options.requestTimeout <= std::chrono::milliseconds::zero()) {
        return {{Error::BadInput, "Options::requestTimeout must be positive"}, {}};
    }
    // steady_clock counts nanoseconds in 64 bits, so an extreme deadline
    // wraps around: milliseconds::max() as "no deadline" into the past, a
    // hugely negative one into no deadline at all.
    return {{}, Clock::now() + std::clamp<std::chrono::milliseconds>(request.deadline, std::chrono::milliseconds::zero(),
                                                                      std::chrono::hours(24))};
}

Result<VideoInfo> Resolver::Impl::resolve(std::string_view urlOrId, const Request &request)
{
    const Result<Clock::time_point> deadline = callDeadline(request);
    if (!deadline) {
        return {deadline.status, {}};
    }

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
    const Call call{videoId.value, request, deadline.value};

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

Result<std::vector<SearchResult>> Resolver::Impl::search(std::string_view query, std::size_t max, const Request &request)
{
    const Result<Clock::time_point> deadline = callDeadline(request);
    if (!deadline) {
        return {deadline.status, {}};
    }
    if (max == 0) {
        return {{Error::BadInput, "A search must ask for at least one video"}, {}};
    }
    if (query.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return {{Error::BadInput, "The search query is empty"}, {}};
    }
    // yt-dlp searches as the web client whatever it plays with, and so does
    // the library: Options::clients is the player's ladder.
    const innertube::ClientDef *web = innertube::findClient(ClientId::Web);
    if (!web) {
        return {{Error::Internal, "The client table has no web client"}, {}};
    }
    // No watch page for a search: YouTube answered it bare on 2026-10-01.
    // Whatever a resolve left in the cache goes along, fresh or stale, on
    // every page. With nothing cached, a later page carries the visitor data
    // the page before it named, as yt-dlp sends it; that stays out of the
    // cache, which holds what a watch page gave.
    const std::string cached = visitorCache.get(Clock::now()).value;
    std::string visitorData = cached;
    const std::string text(query);
    const Call call{std::string{}, request, deadline.value};

    std::vector<SearchResult> found;
    innertube::Continuation next;
    for (int pageNumber = 1;; ++pageNumber) {
        Result<innertube::SearchPage> page = askSearch(*web, text, visitorData, next, call);
        // A later page's failure keeps what the pages before it brought:
        // a caller cancelled on page three can still use the first two.
        if (!page) {
            return {page.status, std::move(found)};
        }
        std::vector<SearchResult> &results = page.value.results;
        log(LogLevel::Debug, "search page " + std::to_string(pageNumber) + ": " + std::to_string(results.size()) + " videos");
        for (SearchResult &result : results) {
            if (found.size() >= max) {
                break;
            }
            found.push_back(std::move(result));
        }
        // A page that brought no videos promises none on the next.
        if (found.size() >= max || page.value.next.token.empty() || results.empty() || pageNumber >= MAX_SEARCH_PAGES) {
            break;
        }
        next = std::move(page.value.next);
        if (cached.empty() && !page.value.visitorData.empty()) {
            visitorData = std::move(page.value.visitorData);
        }
    }
    return {{}, std::move(found)};
}

Result<Playlist> Resolver::Impl::playlist(std::string_view urlOrId, std::size_t max, const Request &request)
{
    const Result<Clock::time_point> deadline = callDeadline(request);
    if (!deadline) {
        return {deadline.status, {}};
    }
    if (max == 0) {
        return {{Error::BadInput, "A playlist must be asked for at least one video"}, {}};
    }
    const Result<std::string> playlistId = parsePlaylistId(urlOrId);
    if (!playlistId) {
        return {playlistId.status, {}};
    }
    // yt-dlp lists a playlist as the web client, as it searches.
    const innertube::ClientDef *web = innertube::findClient(ClientId::Web);
    if (!web) {
        return {{Error::Internal, "The client table has no web client"}, {}};
    }
    // As for a search: no watch page, whatever a resolve left in the cache
    // on every page, and with nothing cached, the visitor data the page
    // before named, kept out of the cache.
    const std::string cached = visitorCache.get(Clock::now()).value;
    std::string visitorData = cached;
    const Call call{std::string{}, request, deadline.value};

    Playlist list;
    list.playlistId = playlistId.value;
    innertube::Continuation next;
    std::set<std::string> tokensSent;
    for (int pageNumber = 1;; ++pageNumber) {
        Result<innertube::PlaylistPage> page = askPlaylist(*web, list.playlistId, visitorData, next, call);
        // A later page's failure keeps what the pages before it brought: the
        // title, the count and the entries.
        if (!page) {
            return {page.status, std::move(list)};
        }
        if (pageNumber == 1) {
            list.title = std::move(page.value.title);
            list.totalCount = page.value.totalCount;
        }
        std::vector<PlaylistEntry> &entries = page.value.entries;
        log(LogLevel::Debug, "playlist page " + std::to_string(pageNumber) + ": " + std::to_string(entries.size()) + " videos");
        for (PlaylistEntry &entry : entries) {
            if (list.entries.size() >= max) {
                break;
            }
            list.entries.push_back(std::move(entry));
        }
        // Unlike a search, a page that brought no entry may still lead on:
        // it can hold nothing but videos nobody may watch. The guards below
        // stop a feed that loops instead.
        if (list.entries.size() >= max || page.value.next.token.empty()) {
            break;
        }
        // A list this long is cut, not finished: the caller must hear of it.
        if (pageNumber >= MAX_PLAYLIST_PAGES) {
            log(LogLevel::Warning, "The playlist " + list.playlistId + " runs past " + std::to_string(MAX_PLAYLIST_PAGES)
                                       + " pages; stopping with " + std::to_string(list.entries.size()) + " videos");
            break;
        }
        // A token sent before would fetch a page read before, and the one
        // after it, for ever.
        if (!tokensSent.insert(page.value.next.token).second) {
            log(LogLevel::Warning, "The playlist " + list.playlistId + " leads back to a page already read, after page "
                                       + std::to_string(pageNumber) + "; stopping there");
            break;
        }
        next = std::move(page.value.next);
        if (cached.empty() && !page.value.visitorData.empty()) {
            visitorData = std::move(page.value.visitorData);
        }
    }
    return {{}, std::move(list)};
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

// One page of a search: the first when continuation has no token, else the
// one it leads to.
Result<innertube::SearchPage> Resolver::Impl::askSearch(const innertube::ClientDef &client, const std::string &query,
                                                        const std::string &visitorData,
                                                        const innertube::Continuation &continuation, const Call &call) const
{
    const Result<HttpResponse> response =
        send(innertube::searchRequest(client, query, options.language, visitorData, continuation), call);
    if (!response) {
        return {response.status, {}};
    }
    // As for the player: a redirect CurlHttpClient did not follow, or
    // whatever another client lets through.
    if (response.value.status < 200 || response.value.status >= 300) {
        return {{Error::Http, "YouTube answered HTTP " + std::to_string(response.value.status)}, {}};
    }
    return innertube::parseSearchResponse(response.value.body);
}

// One page of a playlist: the first when continuation has no token, else the
// one it leads to.
Result<innertube::PlaylistPage> Resolver::Impl::askPlaylist(const innertube::ClientDef &client,
                                                            const std::string &playlistId,
                                                            const std::string &visitorData,
                                                            const innertube::Continuation &continuation,
                                                            const Call &call) const
{
    const Result<HttpResponse> response =
        send(innertube::playlistRequest(client, playlistId, options.language, visitorData, continuation), call);
    if (!response) {
        return {response.status, {}};
    }
    // As for the player: a redirect CurlHttpClient did not follow, or
    // whatever another client lets through.
    if (response.value.status < 200 || response.value.status >= 300) {
        return {{Error::Http, "YouTube answered HTTP " + std::to_string(response.value.status)}, {}};
    }
    return innertube::parsePlaylistResponse(response.value.body);
}

// Sends one request within what is left of the call's deadline.
Result<HttpResponse> Resolver::Impl::send(HttpRequest httpRequest, const Call &call) const
{
    if (call.request.cancelled && call.request.cancelled()) {
        return {{Error::Cancelled, "Cancelled"}, {}};
    }
    const auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(call.deadline - Clock::now());
    if (timeLeft.count() <= 0) {
        return {{Error::Timeout, "The call ran out of time"}, {}};
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
