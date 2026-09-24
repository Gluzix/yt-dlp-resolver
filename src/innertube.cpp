#include "innertube.h"

#include "ascii.h"
#include "url_parse.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <initializer_list>
#include <utility>

namespace ytres::innertube {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

// Copied from yt-dlp's INNERTUBE_CLIENTS; docs/innertube-notes.md has the
// source row and the date it was read.
const ClientDef CLIENTS[] = {
    {
        ClientId::VisionOS,
        "visionos",
        101,
        "VISIONOS",
        "1.02",
        "Apple",
        "RealityDevice17,1",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
        "visionOS",
        "26.5.23O471",
        false,
    },
};

const char *const PLAYER_URL = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
const char *const ORIGIN = "https://www.youtube.com";

// yt-dlp reads on past these; every other status is a failure.
const std::string_view ACCEPTED_STATUSES[] = {"OK", "LIVE_STREAM_OFFLINE", "AGE_CHECK_REQUIRED", "AGE_VERIFICATION_REQUIRED"};

const json *child(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

const json *childArray(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? &*it : nullptr;
}

std::string readString(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool readBool(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

std::int64_t toInt64(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size() ? value : 0;
}

// YouTube sends some numbers as JSON numbers and others as strings
// ("lengthSeconds": "212"). Either reads; missing or malformed is 0.
std::int64_t readInt(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<std::int64_t>();
    }
    if (it->is_string()) {
        return toInt64(it->get_ref<const std::string &>());
    }
    return 0;
}

// audio/webm; codecs="opus" -> opus. A muxed format lists both codecs.
std::string codecsOf(const std::string &mimeType)
{
    const std::string key = "codecs=\"";
    const size_t start = mimeType.find(key);
    if (start == std::string::npos) {
        return {};
    }
    const size_t first = start + key.size();
    const size_t last = mimeType.find('"', first);
    return mimeType.substr(first, last == std::string::npos ? std::string::npos : last - first);
}

Track kindOf(const std::string &mimeType, const std::string &codecs)
{
    if (mimeType.rfind("audio/", 0) == 0) {
        return Track::Audio;
    }
    return codecs.find(',') != std::string::npos ? Track::Muxed : Track::Video;
}

Format readFormat(const json &entry, const std::string &url)
{
    Format format;
    format.itag = static_cast<int>(readInt(entry, "itag"));
    format.url = url;
    format.mimeType = readString(entry, "mimeType");
    format.codec = codecsOf(format.mimeType);
    format.kind = kindOf(format.mimeType, format.codec);
    // yt-dlp reads the average where YouTube gives one, else the peak.
    const std::int64_t averageBitrate = readInt(entry, "averageBitrate");
    format.bitrate = static_cast<int>(averageBitrate > 0 ? averageBitrate : readInt(entry, "bitrate"));
    format.contentLength = readInt(entry, "contentLength");
    format.audioSampleRate = static_cast<int>(readInt(entry, "audioSampleRate"));
    format.audioChannels = static_cast<int>(readInt(entry, "audioChannels"));
    format.width = static_cast<int>(readInt(entry, "width"));
    format.height = static_cast<int>(readInt(entry, "height"));
    format.fps = static_cast<int>(readInt(entry, "fps"));
    return format;
}

bool isAccepted(const std::string &status)
{
    for (std::string_view accepted : ACCEPTED_STATUSES) {
        if (status == accepted) {
            return true;
        }
    }
    return false;
}

bool mentionsAny(const std::string &text, std::initializer_list<std::string_view> phrases)
{
    const std::string lower = asciiLower(text);
    for (std::string_view phrase : phrases) {
        if (lower.find(phrase) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The detail YouTube files under errorScreen, a region block's among them,
// while reason says only "Video unavailable". simpleText, or runs to join.
std::string subreasonOf(const json &playability)
{
    const json *errorScreen = child(playability, "errorScreen");
    const json *renderer = errorScreen ? child(*errorScreen, "playerErrorMessageRenderer") : nullptr;
    const json *subreason = renderer ? child(*renderer, "subreason") : nullptr;
    if (!subreason) {
        return {};
    }
    std::string text = readString(*subreason, "simpleText");
    const json *runs = childArray(*subreason, "runs");
    if (text.empty() && runs) {
        for (const json &run : *runs) {
            if (run.is_object()) {
                text += readString(run, "text");
            }
        }
    }
    return text;
}

Status playabilityFailure(const std::string &status, const std::string &reason, const std::string &subreason)
{
    std::string message = reason.empty() ? "YouTube says " + status : reason;
    if (!subreason.empty()) {
        message += (message.back() == '.' ? " " : ". ") + subreason; // joined as yt-dlp joins them
    }
    const std::string text = reason + " " + subreason;
    // Age gates and private videos arrive as LOGIN_REQUIRED too, so the reason
    // decides first; LOGIN_REQUIRED with none of these phrases is the bot check.
    if (mentionsAny(text, {"confirm your age", "age-restricted", "age restricted", "inappropriate"})) {
        return {Error::AgeRestricted, message};
    }
    if (mentionsAny(text, {"your country", "your region"})) {
        return {Error::GeoBlocked, message};
    }
    if (mentionsAny(text, {"private"})) {
        return {Error::Unavailable, message};
    }
    if (status == "LOGIN_REQUIRED") {
        return {Error::LoginRequired, message};
    }
    return {Error::Unavailable, message};
}

// An accepted status with nothing usable behind it: pass on YouTube's
// reason where it gave one, else say what was missing.
Status noFormatsFailure(const std::string &status, const std::string &reason, const json *streaming,
                        int ciphered, int withoutUrl)
{
    if (status == "AGE_CHECK_REQUIRED" || status == "AGE_VERIFICATION_REQUIRED") {
        return {Error::AgeRestricted, reason.empty() ? "YouTube wants an age check" : reason};
    }
    if (status == "LIVE_STREAM_OFFLINE") {
        return {Error::Unavailable, reason.empty() ? "The live stream is offline" : reason};
    }
    if (ciphered > 0) {
        return {Error::NoFormats, "Every format needs the player JavaScript (" + std::to_string(ciphered) + " ciphered)"};
    }
    if (streaming && streaming->contains("serverAbrStreamingUrl")) {
        return {Error::NoFormats, "YouTube offers this client SABR streaming only"};
    }
    if (withoutUrl > 0) {
        return {Error::NoFormats, "No format has a url (" + std::to_string(withoutUrl) + " formats)"};
    }
    return {Error::NoFormats, "YouTube listed no formats"};
}

Result<VideoInfo> readPlayerResponse(const json &root, const std::string &videoId, std::int64_t nowUnix,
                                     const LogFn &log)
{
    const json *playability = child(root, "playabilityStatus");
    const std::string status = playability ? readString(*playability, "status") : std::string{};
    if (status.empty()) {
        return {{Error::Parse, "The player response has no playability status"}, {}};
    }
    const std::string reason = readString(*playability, "reason");
    if (!isAccepted(status)) {
        return {playabilityFailure(status, reason, subreasonOf(*playability)), {}};
    }

    // yt-dlp checks this too: now and then YouTube answers about another video.
    const json *details = child(root, "videoDetails");
    const std::string answeredId = details ? readString(*details, "videoId") : std::string{};
    if (answeredId != videoId) {
        return {{Error::Parse, answeredId.empty() ? "The player response has no video details"
                                                  : "The player response is about " + answeredId + ", not " + videoId}, {}};
    }

    VideoInfo info;
    info.videoId = videoId;
    info.title = readString(*details, "title");
    info.author = readString(*details, "author");
    info.webpageUrl = canonicalWatchUrl(videoId);
    info.durationSeconds = readInt(*details, "lengthSeconds");
    info.isLive = readBool(*details, "isLive");

    // Every url names its own death as expire=<unix seconds>, which is what
    // yt-dlp reads. expiresInSeconds stands in for a url without one.
    const json *streaming = child(root, "streamingData");
    const std::int64_t expiresIn = streaming ? readInt(*streaming, "expiresInSeconds") : 0;
    const std::int64_t fallbackExpiry = expiresIn > 0 ? nowUnix + expiresIn : 0;
    int ciphered = 0;
    int withoutUrl = 0;
    for (const char *listName : {"formats", "adaptiveFormats"}) {
        const json *entries = streaming ? childArray(*streaming, listName) : nullptr;
        if (!entries) {
            continue;
        }
        for (const json &entry : *entries) {
            if (!entry.is_object()) {
                continue;
            }
            const std::string url = readString(entry, "url");
            if (url.empty()) {
                if (entry.contains("signatureCipher")) {
                    ++ciphered;
                } else {
                    ++withoutUrl;
                }
                continue;
            }
            info.formats.push_back(readFormat(entry, url));

            const std::int64_t urlExpiry = toInt64(queryValue(url, "expire").value_or(std::string_view{}));
            const std::int64_t expiry = urlExpiry > 0 ? urlExpiry : fallbackExpiry;
            if (expiry > 0 && (info.expiresAtUnix == 0 || expiry < info.expiresAtUnix)) {
                info.expiresAtUnix = expiry;
            }
        }
    }

    if (info.formats.empty()) {
        return {noFormatsFailure(status, reason, streaming, ciphered, withoutUrl), {}};
    }
    if ((ciphered > 0 || withoutUrl > 0) && log) {
        log(LogLevel::Warning, "Skipped formats without a plain url: " + std::to_string(ciphered) + " ciphered, "
                                   + std::to_string(withoutUrl) + " with neither url nor cipher");
    }
    return {{}, std::move(info)};
}

}

const ClientDef *findClient(ClientId id)
{
    for (const ClientDef &client : CLIENTS) {
        if (client.id == id) {
            return &client;
        }
    }
    return nullptr;
}

HttpRequest playerRequest(const ClientDef &client, const std::string &videoId, const std::string &language,
                          const std::string &visitorData)
{
    // ordered_json keeps yt-dlp's key order, so the body reads like the one
    // in the notes.
    ordered_json clientContext = ordered_json::object();
    const std::pair<const char *, const char *> fields[] = {
        {"clientName", client.clientName},
        {"clientVersion", client.clientVersion},
        {"deviceMake", client.deviceMake},
        {"deviceModel", client.deviceModel},
        {"userAgent", client.userAgent},
        {"osName", client.osName},
        {"osVersion", client.osVersion},
    };
    for (const auto &[key, value] : fields) {
        if (value) {
            clientContext[key] = value;
        }
    }
    // yt-dlp's _extract_context forces these three onto every client.
    clientContext["hl"] = language;
    clientContext["timeZone"] = "UTC";
    clientContext["utcOffsetMinutes"] = 0;
    if (!visitorData.empty()) {
        clientContext["visitorData"] = visitorData;
    }

    ordered_json body = ordered_json::object();
    body["context"]["client"] = std::move(clientContext);
    body["videoId"] = videoId;
    body["playbackContext"]["contentPlaybackContext"]["html5Preference"] = "HTML5_PREF_WANTS";
    body["contentCheckOk"] = true;
    body["racyCheckOk"] = true;

    HttpRequest request;
    request.method = "POST";
    request.url = PLAYER_URL;
    // A caller's language in broken UTF-8 gets replaced rather than thrown over.
    request.body = body.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
    request.headers = {
        {"Content-Type", "application/json"},
        {"X-YouTube-Client-Name", std::to_string(client.contextClientName)},
        {"X-YouTube-Client-Version", client.clientVersion},
        {"Origin", ORIGIN},
        {"User-Agent", client.userAgent},
    };
    if (!visitorData.empty()) {
        request.headers.emplace_back("X-Goog-Visitor-Id", visitorData);
    }
    return request;
}

Result<VideoInfo> parsePlayerResponse(const std::string &body, const std::string &videoId, std::int64_t nowUnix,
                                      const LogFn &log)
{
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {{Error::Parse, "The player response is not a JSON object"}, {}};
    }
    // The reads check types as they go; this is the net under them.
    try {
        return readPlayerResponse(root, videoId, nowUnix, log);
    } catch (const json::exception &e) {
        return {{Error::Parse, std::string("The player response has an unexpected shape: ") + e.what()}, {}};
    }
}

}
