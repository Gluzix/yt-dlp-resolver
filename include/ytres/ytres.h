#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Resolves a YouTube video to direct stream URLs by asking YouTube's
// InnerTube API what yt-dlp asks it, without YouTube's player JavaScript.
// =======================================================
// Rules:
// - No exception crosses this header: every call returns a Result whose
//   Status says what went wrong, so a C or JNI wrapper need not care.
// - Every string going in or out is UTF-8, and nothing converts code pages.
// - One Resolver may be called from several threads at once: the bot
//   resolves the next song while one plays. resolve() keeps no state between
//   calls, and any cache added later needs a mutex.
// - Stream URLs die at VideoInfo::expiresAtUnix and work only from the IP
//   address that resolved them.
// =======================================================
namespace ytres {

class HttpClient;

enum class Error {
    Ok, Cancelled, Timeout, Network, Http, Parse,
    Unavailable,     // deleted, private, removed
    AgeRestricted,
    GeoBlocked,
    LoginRequired,
    NoFormats,       // playable, but nothing usable came back
    PlayerScript,    // base.js fetch or extraction failed (JS tier only)
    BadInput,
};

struct Status
{
    Error code{Error::Ok};
    std::string message;              // UTF-8, safe to show a user
    explicit operator bool() const { return code == Error::Ok; }
};

template <class T>
struct Result
{
    Status status;
    T value{};
    explicit operator bool() const { return bool(status); }
};

enum class Track { Audio, Video, Muxed };

struct Format
{
    int itag{0};
    Track kind{Track::Audio};
    std::string url;                  // playable as-is
    std::string mimeType;             // audio/webm; codecs="opus"
    std::string codec;                // opus, mp4a.40.2, vp9, avc1
    int bitrate{0};                   // bits per second
    std::int64_t contentLength{0};    // 0 when unknown
    int audioSampleRate{0};
    int audioChannels{0};
    int width{0}, height{0}, fps{0};  // video only
    bool isDrc{false};                // YouTube's dynamic-range-compressed copy of another format
    bool isDefaultAudio{true};        // false for a dubbed or described track
};

struct VideoInfo
{
    std::string videoId;
    std::string title;                // UTF-8
    std::string author;
    std::string webpageUrl;
    std::int64_t durationSeconds{0};
    bool isLive{false};
    std::vector<Format> formats;
    std::int64_t expiresAtUnix{0};    // every url above dies at this time

    // The stream to play when only the sound matters: itag 251 (Opus), else
    // 140 (AAC, 128k), else the highest bitrate - what yt-dlp's -f bestaudio
    // picks on ordinary videos. It chooses among the audio-only formats in
    // the original language without DRC, and among all audio-only formats
    // only when there are none such. Empty when no format is audio-only.
    std::optional<Format> bestAudio() const;
};

// Per-call cancellation and deadline. cancelled may be empty.
struct Request
{
    std::function<bool()> cancelled;
    std::chrono::milliseconds deadline{std::chrono::seconds{30}};
};

enum class LogLevel { Debug, Info, Warning, Error };

// The InnerTube clients the resolver can pose as. VisionOS needs neither
// YouTube's player JavaScript nor a PO Token.
enum class ClientId { VisionOS };

class Resolver
{
public:
    struct Options
    {
        // Tried in order once there is a client ladder; for now only the
        // first is used.
        std::vector<ClientId> clients{ClientId::VisionOS};
        // language goes to YouTube as hl, and YouTube's reasons come back in
        // it. The checks that tell failures apart read English, so with
        // another language an age gate or a private video reports as
        // LoginRequired and a region block as Unavailable. country is not
        // sent yet: the player request yt-dlp makes carries no gl.
        std::string language{"en"}, country{"US"};
        std::shared_ptr<HttpClient> http;                   // null -> built-in libcurl
        // Called on the resolving thread, so it must cope with several at once.
        // May be empty.
        std::function<void(LogLevel, std::string_view)> log;
        // Bounds each HTTP request; Request::deadline bounds the whole resolve.
        std::chrono::milliseconds requestTimeout{std::chrono::seconds{10}};
    };

    // Two constructors rather than "Options options = {}": Clang and GCC
    // refuse that default while Resolver is incomplete, because it needs
    // Options' member initialisers.
    Resolver();
    explicit Resolver(Options options);
    // Movable, so a factory can hand one out; not copyable. A moved-from
    // Resolver answers every resolve() with BadInput.
    Resolver(Resolver &&other) noexcept;
    Resolver &operator=(Resolver &&other) noexcept;
    ~Resolver();

    // Title, page url and every usable stream of one video. Accepts any
    // youtube.com or youtu.be video link, or a bare 11-character id. Takes
    // two requests: the watch page (about 1.3 MB), for the visitor data
    // without which YouTube bot-checks most videos, then the player API.
    Result<VideoInfo> resolve(std::string_view urlOrId, const Request &request = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
