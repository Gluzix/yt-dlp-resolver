#include "ytres/http.h"
#include "ytres/ytres.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

// Development harness for the ytres library - a test rig, not the product.
// =======================================================
// Rules:
// - By default stdout gets exactly three lines - title, canonical page url,
//   best-audio url - the ones the bot reads from yt-dlp today. Every failure
//   goes to stderr as "<Error>: <message>" with exit code 1.
// - Titles go out as the library's UTF-8, byte for byte: no code page
//   conversion here either.
// - A --dump is a fixture to commit, so scrubbed() takes out what identifies
//   whoever recorded it before writeFile() sees it.
// =======================================================

namespace {

const char *const USAGE = "usage: ytres_cli <url-or-id> [--formats] [--dump <file>]\n";

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

// Wraps the built-in client and keeps the last player response, for
// --dump: the Resolver hands out parsed results only.
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
        }
        return response;
    }

    std::string playerResponse; // unguarded: the harness resolves once, on one thread

private:
    std::shared_ptr<ytres::HttpClient> inner;
};

struct Arguments
{
    std::string target;
    bool formats{false};
    std::string dumpPath;
};

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
        } else if (arg.rfind("--", 0) == 0 || !args.target.empty()) {
            return false;
        } else {
            args.target = arg;
        }
    }
    return !args.target.empty();
}

// Every stream url names the requesting address: ?ip=/&ip= in queries,
// /ip/<addr>/ in hlsManifestUrl's path, IPv6 percent-encoded. Visitor data
// identifies the visitor to YouTube, so it goes too.
std::string scrubbed(std::string text)
{
    text = std::regex_replace(text, std::regex(R"(([?&])ip=[0-9A-Fa-f.:%]+)"), "$1ip=203.0.113.7");
    text = std::regex_replace(text, std::regex(R"(/ip/[0-9A-Fa-f.:%]+/)"), "/ip/203.0.113.7/");
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
    options.log = [](ytres::LogLevel level, std::string_view text) {
        if (level >= ytres::LogLevel::Warning) {
            std::cerr << text << '\n';
        }
    };
    ytres::Resolver resolver(std::move(options));
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
