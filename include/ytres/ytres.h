#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Resolves a YouTube video to direct stream URLs, searches YouTube for
// videos and lists public playlists, by asking YouTube's InnerTube API what
// yt-dlp asks it, without YouTube's player JavaScript.
// =======================================================
// Rules:
// - No exception crosses this header: every call returns a Result whose
//   Status says what went wrong, so a C or JNI wrapper need not care.
// - Every string going in or out is UTF-8, and nothing converts code pages.
// - One Resolver may be called from several threads at once: the bot
//   resolves the next song while one plays. The one thing its calls share
//   is the cached visitor data, behind a mutex that is never held across a
//   request.
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
    // YouTube wants proof that the caller is no bot ("Sign in to confirm
    // you're not a bot"). It is about who asks, not about the video, and the
    // library has tried once to fetch fresh visitor data and, when it got
    // some, asked once more: fall back to another resolver, and stop asking
    // this one for a while.
    BotCheck,
    // A bug or a resource failure inside the library - an exception, an
    // exhausted heap, libcurl unable to make a handle - never YouTube's
    // doing. Log it as a bug; another resolver may still get the video.
    Internal,
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

// One video a search found: enough to list it and to choose, not to play
// it. Resolve its videoId, or watchUrl(videoId), for the streams.
struct SearchResult
{
    std::string videoId;
    std::string title;                // UTF-8
    std::string author;               // the channel's name
    std::int64_t durationSeconds{0};  // 0 when YouTube gives none: live, upcoming
    bool isLive{false};               // streaming now; resolve() answers it with NoFormats
    bool isUpcoming{false};           // a scheduled premiere or stream, not playable yet
};

// One video of a playlist: enough to list it, not to play it. Resolve its
// videoId, or watchUrl(videoId), for the streams.
struct PlaylistEntry
{
    std::string videoId;
    std::string title;                // UTF-8
    std::int64_t durationSeconds{0};  // 0 when YouTube gives none
};

// The first videos of a public playlist, and what YouTube says about it.
struct Playlist
{
    std::string playlistId;
    std::string title;                  // UTF-8; empty when YouTube gave none
    std::size_t totalCount{0};          // the videos YouTube says it has; 0 = unknown
    std::vector<PlaylistEntry> entries; // in playlist order, at most max
};

// https://www.youtube.com/watch?v=<videoId>: the page of a search result or
// a playlist entry, and a valid resolve() target.
std::string watchUrl(std::string_view videoId);

// Per-call cancellation and deadline. cancelled may be empty. deadline bounds
// the whole call - the watch page, every client of the ladder and the bot
// check's second try alike, or every page of a search or a playlist: each
// request gets the smaller of Options::requestTimeout and what is left of
// it, and none is sent once it has run out.
struct Request
{
    std::function<bool()> cancelled;
    std::chrono::milliseconds deadline{std::chrono::seconds{30}};
};

enum class LogLevel { Debug, Info, Warning, Error };

// The InnerTube clients the resolver can pose as, rows of yt-dlp's table.
// VisionOS needs neither YouTube's player JavaScript nor a PO Token; it is
// the one that plays. Web needs both, which the library does not have, so
// YouTube turns it away and it ends in NoFormats: it is there to exercise
// the client ladder, and as the pattern for the next client that works.
enum class ClientId { VisionOS, Web };

class Resolver
{
public:
    struct Options
    {
        // The client ladder, tried in order until one gives formats. A
        // failure about the client - BotCheck, NoFormats, Http, Parse,
        // PlayerScript - passes the video on to the next; one about the
        // video, the network, the call or the library ends the resolve there.
        // Empty, or an id the library does not know, is BadInput. Only
        // resolve() climbs it; search() and playlist() always ask as the
        // web client.
        std::vector<ClientId> clients{ClientId::VisionOS};
        // language goes to YouTube as hl, and YouTube's reasons come back in
        // it. The checks that tell failures apart read English, so with
        // another language an age gate, a private video or the bot check
        // reports as LoginRequired - and the bot check gets no second try -
        // and a region block as Unavailable. country is not sent yet: the
        // player request yt-dlp makes carries no gl.
        std::string language{"en"}, country{"US"};
        std::shared_ptr<HttpClient> http;                   // null -> built-in libcurl
        // Called on the resolving thread, so it must cope with several at once.
        // May be empty.
        std::function<void(LogLevel, std::string_view)> log;
        // Bounds each HTTP request; Request::deadline bounds the whole call.
        // Zero or less is BadInput.
        std::chrono::milliseconds requestTimeout{std::chrono::seconds{10}};
    };

    // Two constructors rather than "Options options = {}": Clang and GCC
    // refuse that default while Resolver is incomplete, because it needs
    // Options' member initialisers.
    Resolver();
    explicit Resolver(Options options);
    // Movable, so a factory can hand one out; not copyable. A moved-from
    // Resolver answers every call with BadInput.
    Resolver(Resolver &&other) noexcept;
    Resolver &operator=(Resolver &&other) noexcept;
    ~Resolver();

    // Title, page url and every usable stream of one video. Accepts any
    // youtube.com or youtu.be video link, or a bare 11-character id.
    // A warm Resolver asks the player API alone: one request. A cold one
    // first fetches a watch page (about 1.3 MB) for the visitor data without
    // which YouTube bot-checks most videos, and keeps it for up to 6 hours
    // for every later resolve on any thread. A bot check makes it try once
    // to fetch fresh visitor data and, when it gets some, ask once more,
    // within the same deadline.
    // When every client in the ladder fails, the result carries the most
    // telling of their codes - BotCheck, then Http, then Parse, then
    // NoFormats or PlayerScript - and its message names each client with
    // what it answered. A Network, Timeout or Internal failure that ends the
    // ladder after earlier clients failed keeps their answers in its
    // message, and an earlier BotCheck outranks a later Network or Timeout.
    Result<VideoInfo> resolve(std::string_view urlOrId, const Request &request = {});

    // Up to max videos for query, best match first; an empty list when
    // YouTube finds none. One request for the first twenty or so, one more
    // per further page. Channels, playlists and shelves in the results are
    // skipped. max of 0, or a query that is empty or all spaces, is BadInput.
    // A first page where YouTube counts results but holds none the library
    // can read is Parse, not an empty list: YouTube has changed its answer,
    // and the caller should fall back rather than report nothing found.
    // Asked as the web client whatever Options::clients says: that list is
    // the player's ladder. Cached visitor data is sent when there is some,
    // but none is fetched for a search; with none cached, a later page
    // carries the visitor data YouTube named on the page before it, as
    // yt-dlp sends it, and that value is not cached.
    // When a later page fails, the result carries that failure and the
    // videos read so far.
    Result<std::vector<SearchResult>> search(std::string_view query, std::size_t max, const Request &request = {});

    // The first max videos of a public playlist, in its order, with its
    // title and how many videos YouTube says it has. Accepts a youtube.com
    // /playlist or /watch link, or a youtu.be link, that carries list=, or a
    // bare playlist id. One request per hundred videos, and no more requests
    // than max needs: a 6000-video playlist asked for its first 50 costs one.
    // Videos YouTube hides as unavailable are not listed, nor is one titled
    // [Private video] or [Deleted video] should it list one, so entries may
    // be fewer than totalCount says. A list longer than 20,000 videos - a
    // big channel's uploads (UU...) - is cut there, with a warning in the
    // log, and comes back Ok.
    // A playlist that does not exist is Unavailable, with YouTube's words,
    // as is any whose answer is an error alert instead of a playlist. A mix
    // (RD...), Watch Later, Liked videos and the like are BadInput: they
    // belong to a signed-in viewer. max of 0 is BadInput. A first page where
    // YouTube counts videos but holds none the library can read is Parse,
    // not an empty playlist: YouTube has changed its answer, and the caller
    // should fall back rather than report the playlist empty.
    // Asked as the web client whatever Options::clients says. Cached visitor
    // data is sent when there is some, but none is fetched for a playlist;
    // with none cached, a later page carries the visitor data YouTube named
    // on the page before it, as yt-dlp sends it, and that value is not cached.
    // When a later page fails, the result carries that failure and the
    // playlist as read so far: its title, its count and the entries before
    // the page that failed.
    Result<Playlist> playlist(std::string_view urlOrId, std::size_t max, const Request &request = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
