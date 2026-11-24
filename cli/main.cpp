#include "arguments.h"
#include "scrub.h"

#include "ytres/http.h"
#include "ytres/ytres.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

// Development harness for the ytres library - a test rig, not the product.
// =======================================================
// Rules:
// - By default stdout gets exactly three lines - title, canonical page url,
//   best-audio url - the ones the bot reads from yt-dlp today. With
//   --search it gets one line per video instead: page url, seconds (or
//   "live" or "upcoming"), channel and title, separated by tabs. With
//   --playlist it gets the playlist's title and count, then one line per
//   video: page url, seconds and title, all separated by tabs. Every
//   failure goes to stderr as "<Error>: <message>" with exit code 1; a
//   search or a playlist that fails on a later page prints what it read
//   first.
// - A --dump is a side output: it is written after the results are out, so
//   a dump that cannot be written ("Internal: Cannot write <file>") costs
//   the run its exit code, not what it fetched. A failure or a cancel that
//   left no response to dump says so on stderr.
// - Titles go out as the library's UTF-8, byte for byte: no code page
//   conversion here either.
// - A --dump is a fixture to commit, so scrubbed() (scrub.h) takes out what
//   identifies whoever recorded it before writeFile() sees it.
// - Nothing escapes main(): an exception is "Internal: <what>" on stderr
//   with exit code 1, and a Debug build's C runtime reports to stderr, never
//   in a window, so an unattended run cannot hang on a dialog.
// =======================================================

namespace {

const char *const USAGE = "usage: ytres_cli <url-or-id> [--formats] [--dump <file>] [--client visionos|web]\n"
                          "       ytres_cli --search <query> [--max N] [--dump <file>]\n"
                          "       ytres_cli --playlist <url-or-id> [--max N] [--dump <file>]\n";

// How many videos --search lists when --max does not say.
const std::size_t DEFAULT_SEARCH_MAX = 5;

// How many videos --playlist lists when --max does not say.
const std::size_t DEFAULT_PLAYLIST_MAX = 50;

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
// first search and browse responses, for --dump: the Resolver hands out
// parsed results only. The first page, because that is what a fixture
// records.
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
        } else if (request.url.find("/youtubei/v1/browse") != std::string::npos && !browseRecorded) {
            browseResponse = response.value.body;
            browseRecorded = true;
        }
        return response;
    }

    // Unguarded: the harness makes one call, on one thread.
    std::string playerResponse;
    std::string searchResponse;
    std::string browseResponse;

private:
    std::shared_ptr<ytres::HttpClient> inner;
    bool searchRecorded{false};
    bool browseRecorded{false};
};

bool writeFile(const std::string &path, const std::string &contents)
{
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    return !file.fail();
}

// The --dump, when one was asked for: response scrubbed, after the run's
// output. code is the run's exit code so far, and comes back 1 when the
// file cannot be written.
int writeDump(const ytres_cli::Arguments &args, const std::string &response, int code)
{
    if (!args.dumpPath) {
        return code;
    }
    if (response.empty()) {
        std::cerr << "No response to dump\n";
        return code;
    }
    if (!writeFile(*args.dumpPath, ytres_cli::scrubbed(response))) {
        std::cerr << "Internal: Cannot write " << *args.dumpPath << '\n';
        return 1;
    }
    return code;
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

// A resolve's output: the three lines, or every format with --formats, or
// the failure. The exit code.
int printVideo(const ytres::Result<ytres::VideoInfo> &result, const ytres_cli::Arguments &args)
{
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
int runSearch(ytres::Resolver &resolver, const RecordingHttpClient &recorder, const ytres_cli::Arguments &args)
{
    const ytres::Result<std::vector<ytres::SearchResult>> found =
        resolver.search(*args.query, args.max.value_or(DEFAULT_SEARCH_MAX));

    // A later page's failure leaves the videos read before it: they go out too.
    for (const ytres::SearchResult &result : found.value) {
        std::cout << ytres::watchUrl(result.videoId) << '\t' << lengthOf(result) << '\t' << result.author << '\t'
                  << result.title << '\n';
    }
    int code = 0;
    if (!found) {
        std::cerr << errorName(found.status.code) << ": " << found.status.message << '\n';
        code = 1;
    }
    // Written whatever the verdict: an empty search's answer is a fixture too.
    return writeDump(args, recorder.searchResponse, code);
}

// --playlist: the title and the count, one line per video, then the
// failure, if any.
int runPlaylist(ytres::Resolver &resolver, const RecordingHttpClient &recorder, const ytres_cli::Arguments &args)
{
    const ytres::Result<ytres::Playlist> list =
        resolver.playlist(*args.playlist, args.max.value_or(DEFAULT_PLAYLIST_MAX));

    // A later page's failure leaves the playlist as read before it: it goes
    // out too. A failure on the first page leaves nothing to print.
    const ytres::Playlist &listed = list.value;
    if (list || !listed.title.empty() || listed.totalCount > 0 || !listed.entries.empty()) {
        std::cout << listed.title << '\t' << listed.totalCount << '\n';
    }
    for (const ytres::PlaylistEntry &entry : listed.entries) {
        std::cout << ytres::watchUrl(entry.videoId) << '\t' << entry.durationSeconds << '\t' << entry.title << '\n';
    }
    int code = 0;
    if (!list) {
        std::cerr << errorName(list.status.code) << ": " << list.status.message << '\n';
        code = 1;
    }
    // Written whatever the verdict: a missing playlist's answer is a fixture too.
    return writeDump(args, recorder.browseResponse, code);
}

// The whole run, for main() to guard. The exit code.
int run(int argc, char **argv)
{
    ytres_cli::Arguments args;
    if (!ytres_cli::parseArguments(std::vector<std::string>(argv + 1, argv + argc), args)) {
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
    if (args.playlist) {
        return runPlaylist(resolver, *recorder, args);
    }
    const ytres::Result<ytres::VideoInfo> result = resolver.resolve(*args.target);
    // Written whatever the verdict: an unavailable video's answer is a fixture too.
    return writeDump(args, recorder->playerResponse, printVideo(result, args));
}

}

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
    // The Debug C runtime opens a modal window on a failed assertion or an
    // abort(); tests and scheduled runs drive this harness with nobody to
    // close it, so it reports to stderr instead. Macros that expand to
    // nothing in a Release build, hence no loop.
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // Only the fault report goes: an abort() still writes its message, and
    // to stderr, because _CRT_ERROR does; a silent exit code 3 would tell
    // whoever reads the run's output nothing.
    (void)_set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    // Nothing escapes: an exception out of main() is an abort(), which is
    // a crash to whoever drives the harness, not an answer.
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "Internal: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "Internal: unexpected failure\n";
    }
    return 1;
}
