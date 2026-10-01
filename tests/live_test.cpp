#include "ytres/http.h"
#include "ytres/ytres.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Touches the network: resolves tests/corpus.txt against live YouTube, counts
// the requests of a warm resolve, times a cancel through libcurl, walks the
// client ladder for real and searches once. Built with the rest but never run
// by ctest; README.md, "Live tests", says how to run it. YouTube changes under
// these checks, so a failure here says "look", not always "the library broke".

using ytres::Error;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

const char *errorName(Error error)
{
    switch (error) {
    case Error::Ok: return "Ok";
    case Error::Cancelled: return "Cancelled";
    case Error::Timeout: return "Timeout";
    case Error::Network: return "Network";
    case Error::Http: return "Http";
    case Error::Parse: return "Parse";
    case Error::Unavailable: return "Unavailable";
    case Error::AgeRestricted: return "AgeRestricted";
    case Error::GeoBlocked: return "GeoBlocked";
    case Error::LoginRequired: return "LoginRequired";
    case Error::NoFormats: return "NoFormats";
    case Error::PlayerScript: return "PlayerScript";
    case Error::BadInput: return "BadInput";
    case Error::BotCheck: return "BotCheck";
    case Error::Internal: return "Internal";
    }
    return "Unknown";
}

struct CorpusEntry
{
    std::string id;
    std::string expected; // the name of an Error
    std::string line;     // as written, for a failure to quote
};

// tests/corpus.txt: an id and the name of an Error on each line; from a # to
// the end of the line is a comment.
std::vector<CorpusEntry> readCorpus()
{
    std::ifstream file(YTRES_CORPUS, std::ios::binary);
    std::vector<CorpusEntry> entries;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line.substr(0, line.find('#')));
        CorpusEntry entry;
        if (fields >> entry.id >> entry.expected) {
            entry.line = line;
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

// The built-in libcurl client, counting what goes through it.
class CountingHttpClient : public ytres::HttpClient
{
public:
    ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &request) override
    {
        ++count;
        return inner->send(request);
    }

    std::atomic<int> count{0};

private:
    const std::shared_ptr<ytres::HttpClient> inner = ytres::makeCurlHttpClient();
};

long long msSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Exactly 11 characters from [A-Za-z0-9_-], as a video id is.
bool looksLikeVideoId(const std::string &id)
{
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
}

}

TEST_CASE("every video in the corpus resolves as the corpus says")
{
    const std::vector<CorpusEntry> corpus = readCorpus();
    REQUIRE(corpus.size() >= 7);
    ytres::Resolver resolver; // one for them all, as the bot keeps one
    for (const CorpusEntry &entry : corpus) {
        CAPTURE(entry.line);
        const Clock::time_point start = Clock::now();
        const ytres::Result<ytres::VideoInfo> result = resolver.resolve(entry.id);
        const std::string outcome = errorName(result.status.code); // a std::string, which doctest prints as text
        MESSAGE(entry.id << ": " << outcome << " in " << msSince(start) << " ms"
                         << (result ? "" : " - " + result.status.message));
        CHECK(outcome == entry.expected);
        if (!result) {
            continue;
        }
        CHECK_FALSE(result.value.title.empty());
        CHECK(result.value.webpageUrl == "https://www.youtube.com/watch?v=" + entry.id);
        CHECK(result.value.expiresAtUnix > nowUnix());
        const std::optional<ytres::Format> best = result.value.bestAudio();
        CHECK(best.has_value()); // not REQUIRE: one video's miss must not hide the rest
        if (best) {
            CHECK(best->url.rfind("https://", 0) == 0);
        }
    }
}

TEST_CASE("a warm Resolver resolves with the player request alone")
{
    const auto http = std::make_shared<CountingHttpClient>();
    ytres::Resolver::Options options;
    options.http = http;
    ytres::Resolver resolver(options);

    Clock::time_point start = Clock::now();
    REQUIRE(resolver.resolve("dQw4w9WgXcQ"));
    const long long cold = msSince(start);
    const int coldRequests = http->count.load();

    // Another video: the visitor data belongs to no one video.
    start = Clock::now();
    REQUIRE(resolver.resolve("jNQXAC9IVRw"));
    const long long warm = msSince(start);
    const int warmRequests = http->count.load() - coldRequests;

    MESSAGE("cold resolve " << cold << " ms in " << coldRequests << " requests, warm " << warm << " ms in "
                            << warmRequests);
    CHECK(coldRequests == 2);
    CHECK(warmRequests == 1);
}

TEST_CASE("a cancel lands within 200 ms, even while libcurl waits for a connection")
{
    // 10.255.255.1 is routed nowhere, so the connection never comes and only
    // the cancel at 100 ms can end the request.
    const std::shared_ptr<ytres::HttpClient> http = ytres::makeCurlHttpClient();
    for (int i = 0; i < 3; ++i) {
        ytres::HttpRequest request;
        request.method = "GET";
        request.url = "http://10.255.255.1/";
        request.timeout = 10s;
        const Clock::time_point start = Clock::now();
        request.cancelled = [start] { return Clock::now() - start >= 100ms; };
        const ytres::Result<ytres::HttpResponse> result = http->send(request);
        const long long took = msSince(start);
        MESSAGE("cancelled at 100 ms, returned at " << took << " ms");
        CHECK(result.status.code == Error::Cancelled);
        CHECK(took < 200);
    }
}

TEST_CASE("a resolve cancelled at 100 ms returns within 200 ms")
{
    ytres::Resolver resolver; // cold, so the cancel lands in the watch page
    ytres::Request request;
    const Clock::time_point start = Clock::now();
    request.cancelled = [start] { return Clock::now() - start >= 100ms; };
    const ytres::Result<ytres::VideoInfo> result = resolver.resolve("dQw4w9WgXcQ", request);
    const long long took = msSince(start);
    MESSAGE("resolve cancelled at 100 ms, returned at " << took << " ms");
    CHECK(result.status.code == Error::Cancelled);
    CHECK(took < 200);
}

TEST_CASE("a search finds videos in one request, each with an id, a title and a channel")
{
    // What notices YouTube changing the search answer's shape: the offline
    // tests replay what it was on 2026-10-01.
    const auto http = std::make_shared<CountingHttpClient>();
    ytres::Resolver::Options options;
    options.http = http;
    ytres::Resolver resolver(options);
    const Clock::time_point start = Clock::now();
    const ytres::Result<std::vector<ytres::SearchResult>> found = resolver.search("Rick Astley", 5);
    const std::string outcome = errorName(found.status.code); // a std::string, which doctest prints as text
    MESSAGE("search: " << outcome << " in " << msSince(start) << " ms, " << found.value.size() << " videos"
                       << (found ? "" : " - " + found.status.message));
    CHECK(outcome == "Ok");
    CHECK(http->count.load() == 1); // no watch page, one page of results
    CHECK_FALSE(found.value.empty());
    CHECK(found.value.size() <= 5);
    for (const ytres::SearchResult &video : found.value) {
        CAPTURE(video.videoId);
        CHECK(looksLikeVideoId(video.videoId));
        CHECK_FALSE(video.title.empty());
        CHECK_FALSE(video.author.empty());
    }
}

TEST_CASE("the ladder falls through web to visionos")
{
    std::vector<std::string> warnings;
    ytres::Resolver::Options options;
    options.clients = {ytres::ClientId::Web, ytres::ClientId::VisionOS};
    options.log = [&warnings](ytres::LogLevel level, std::string_view text) {
        if (level == ytres::LogLevel::Warning) {
            warnings.emplace_back(text);
        }
    };
    ytres::Resolver resolver(options);
    const ytres::Result<ytres::VideoInfo> result = resolver.resolve("dQw4w9WgXcQ");
    for (const std::string &warning : warnings) {
        MESSAGE(warning);
    }
    REQUIRE(result);
    const std::optional<ytres::Format> best = result.value.bestAudio();
    REQUIRE(best.has_value());
    CHECK(best->itag == 251);
    CHECK(std::any_of(warnings.begin(), warnings.end(),
                      [](const std::string &warning) { return warning.find("The web client failed") == 0; }));
}
