#include "ytres/http.h"
#include "ytres/ytres.h"

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Development harness for the ytres library - a test rig, not the product.
// =======================================================
// Rules:
// - By default stdout gets exactly three lines - title, canonical page url,
//   best-audio url - the ones the bot reads from yt-dlp today. With
//   --search it gets one line per video instead: page url, seconds (or
//   "live" or "upcoming"), channel and title, separated by tabs. Every
//   failure goes to stderr as "<Error>: <message>" with exit code 1; a
//   search that fails on a later page prints the videos it read first.
// - Titles go out as the library's UTF-8, byte for byte: no code page
//   conversion here either.
// - A --dump is a fixture to commit, so scrubbed() takes out what identifies
//   whoever recorded it before writeFile() sees it.
// =======================================================

namespace {

const char *const USAGE = "usage: ytres_cli <url-or-id> [--formats] [--dump <file>] [--client visionos|web]\n"
                          "       ytres_cli --search <query> [--max N] [--dump <file>]\n";

// How many videos --search lists when --max does not say.
const std::size_t DEFAULT_SEARCH_MAX = 5;

// What --client takes: yt-dlp's names for the clients, as the library's
// table keeps them.
const std::pair<std::string_view, ytres::ClientId> CLIENT_NAMES[] = {
    {"visionos", ytres::ClientId::VisionOS},
    {"web", ytres::ClientId::Web},
};

const char *errorName(ytres::Error error)
{
    switch (error) {
    case ytres::Error::Ok: return "Ok";
    case ytres::Error::Cancelled: return "Cancelled";
    case ytres::Error::Timeout: return "Timeout";
    case ytres::Error::Network: return "Network";
    case ytres::Error::Http: return "Http";
    case ytres::Error::Parse: return "Parse";
    case ytres::Error::Unavailable: return "Unavailable";
    case ytres::Error::AgeRestricted: return "AgeRestricted";
    case ytres::Error::GeoBlocked: return "GeoBlocked";
    case ytres::Error::LoginRequired: return "LoginRequired";
    case ytres::Error::NoFormats: return "NoFormats";
    case ytres::Error::PlayerScript: return "PlayerScript";
    case ytres::Error::BadInput: return "BadInput";
    case ytres::Error::BotCheck: return "BotCheck";
    case ytres::Error::Internal: return "Internal";
    }
    return "Unknown";
}

const char *trackName(ytres::Track kind)
{
    switch (kind) {
    case ytres::Track::Audio: return "audio";
    case ytres::Track::Video: return "video";
    case ytres::Track::Muxed: return "muxed";
    }
    return "?";
}

// Wraps the built-in client and keeps the last player response and the
// first search response, for --dump: the Resolver hands out parsed results
// only. The first search page, because that is what a fixture records.
class RecordingHttpClient : public ytres::HttpClient
{
public:
    explicit RecordingHttpClient(std::shared_ptr<ytres::HttpClient> inner_)
        : inner(std::move(inner_))
    {
    }

    ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &request) override
    {
        ytres::Result<ytres::HttpResponse> response = inner->send(request);
        if (request.url.find("/youtubei/v1/player") != std::string::npos) {
            playerResponse = response.value.body;
        } else if (request.url.find("/youtubei/v1/search") != std::string::npos && !searchRecorded) {
            searchResponse = response.value.body;
            searchRecorded = true;
        }
        return response;
    }

    // Unguarded: the harness makes one call, on one thread.
    std::string playerResponse;
    std::string searchResponse;

private:
    std::shared_ptr<ytres::HttpClient> inner;
    bool searchRecorded{false};
};

struct Arguments
{
    std::string target;
    bool formats{false};
    std::string dumpPath;
    std::optional<ytres::ClientId> client; // the library's own ladder when empty
    std::optional<std::string> query;      // --search: a search instead of a resolve
    std::optional<std::size_t> max;        // --search's --max; DEFAULT_SEARCH_MAX when empty
};

std::optional<ytres::ClientId> clientNamed(std::string_view name)
{
    for (const auto &[clientName, id] : CLIENT_NAMES) {
        if (clientName == name) {
            return id;
        }
    }
    return std::nullopt;
}

// --max's count: decimal digits and nothing else. 0 goes through, for the
// library to answer.
std::optional<std::size_t> countIn(std::string_view text)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return count;
}

bool parseArguments(int argc, char **argv, Arguments &args)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--formats") {
            args.formats = true;
        } else if (arg == "--dump") {
            if (++i >= argc) {
                return false;
            }
            args.dumpPath = argv[i];
        } else if (arg == "--client") {
            if (++i >= argc) {
                return false;
            }
            args.client = clientNamed(argv[i]);
            if (!args.client) {
                return false;
            }
        } else if (arg == "--search") {
            if (++i >= argc || args.query) {
                return false;
            }
            args.query = argv[i];
        } else if (arg == "--max") {
            if (++i >= argc) {
                return false;
            }
            args.max = countIn(argv[i]);
            if (!args.max) {
                return false;
            }
        } else if (arg.rfind("--", 0) == 0 || !args.target.empty()) {
            return false;
        } else {
            args.target = arg;
        }
    }
    // Exactly one of a target and a search. --formats and --client are about
    // a resolve, --max about a search.
    if (args.query) {
        return args.target.empty() && !args.formats && !args.client;
    }
    return !args.target.empty() && !args.max;
}

// Every stream url names the requesting address: ?ip=/&ip= in queries,
// /ip/<addr>/ in hlsManifestUrl's path, IPv6 percent-encoded. Visitor data
// identifies the visitor to YouTube, and carries the country, so it goes
// too, under both names it travels by: "visitorData", and the web client's
// tracking parameter "visitor_data".
std::string scrubbed(std::string text)
{
    text = std::regex_replace(text, std::regex(R"(([?&])ip=[0-9A-Fa-f.:%]+)"), "$1ip=203.0.113.7");
    text = std::regex_replace(text, std::regex(R"(/ip/[0-9A-Fa-f.:%]+/)"), "/ip/203.0.113.7/");
    text = std::regex_replace(text, std::regex(R"("key":"visitor_data","value":"[^"]*")"),
                              R"("key":"visitor_data","value":"FIXTURE")");
    return std::regex_replace(text, std::regex(R"("visitorData":"[^"]*")"), R"("visitorData":"FIXTURE")");
}

bool writeFile(const std::string &path, const std::string &contents)
{
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    return !file.fail();
}

// One line per format: itag, kind, codec, bitrate, size, dimensions.
void printFormats(const ytres::VideoInfo &info)
{
    for (const ytres::Format &format : info.formats) {
        std::string dimensions = "-";
        if (format.kind != ytres::Track::Audio) {
            dimensions = std::to_string(format.width) + "x" + std::to_string(format.height) + "@" + std::to_string(format.fps);
        }
        std::printf("%4d  %-5s  %-24s %8d bps  %11lld bytes  %s\n", format.itag, trackName(format.kind),
                    format.codec.c_str(), format.bitrate, static_cast<long long>(format.contentLength),
                    dimensions.c_str());
    }
}

// --search's length column: the seconds, or what stands in for them.
std::string lengthOf(const ytres::SearchResult &result)
{
    if (result.isLive) {
        return "live";
    }
    if (result.isUpcoming) {
        return "upcoming";
    }
    return std::to_string(result.durationSeconds);
}

// --search: one line per video, then the failure, if any.
int runSearch(ytres::Resolver &resolver, const RecordingHttpClient &recorder, const Arguments &args)
{
    const ytres::Result<std::vector<ytres::SearchResult>> found =
        resolver.search(*args.query, args.max.value_or(DEFAULT_SEARCH_MAX));

    // Written before the verdict, as for a resolve.
    if (!args.dumpPath.empty() && !recorder.searchResponse.empty()
        && !writeFile(args.dumpPath, scrubbed(recorder.searchResponse))) {
        std::cerr << "Cannot write " << args.dumpPath << '\n';
        return 1;
    }
    // A later page's failure leaves the videos read before it: they go out too.
    for (const ytres::SearchResult &result : found.value) {
        std::cout << ytres::watchUrl(result.videoId) << '\t' << lengthOf(result) << '\t' << result.author << '\t'
                  << result.title << '\n';
    }
    if (!found) {
        std::cerr << errorName(found.status.code) << ": " << found.status.message << '\n';
        return 1;
    }
    return 0;
}

}

int main(int argc, char **argv)
{
    Arguments args;
    if (!parseArguments(argc, argv, args)) {
        std::cerr << USAGE;
        return 1;
    }

    auto recorder = std::make_shared<RecordingHttpClient>(ytres::makeCurlHttpClient());
    ytres::Resolver::Options options;
    options.http = recorder;
    if (args.client) {
        options.clients = {*args.client};
    }
    options.log = [](ytres::LogLevel level, std::string_view text) {
        if (level >= ytres::LogLevel::Warning) {
            std::cerr << text << '\n';
        }
    };
    ytres::Resolver resolver(std::move(options));
    if (args.query) {
        return runSearch(resolver, *recorder, args);
    }
    const ytres::Result<ytres::VideoInfo> result = resolver.resolve(args.target);

    // Written before the verdict: an unavailable video's answer is a fixture too.
    if (!args.dumpPath.empty() && !recorder->playerResponse.empty()
        && !writeFile(args.dumpPath, scrubbed(recorder->playerResponse))) {
        std::cerr << "Cannot write " << args.dumpPath << '\n';
        return 1;
    }
    if (!result) {
        std::cerr << errorName(result.status.code) << ": " << result.status.message << '\n';
        return 1;
    }

    const ytres::VideoInfo &info = result.value;
    if (args.formats) {
        printFormats(info);
        return 0;
    }
    const std::optional<ytres::Format> best = info.bestAudio();
    if (!best) {
        std::cerr << errorName(ytres::Error::NoFormats) << ": No audio-only format\n";
        return 1;
    }
    std::cout << info.title << '\n' << info.webpageUrl << '\n' << best->url << '\n';
    return 0;
}
