#include "watch_page.h"

#include "url_parse.h"

#include <nlohmann/json.hpp>

namespace ytres::watchpage {

namespace {

using nlohmann::json;

const size_t MAX_VISITOR_DATA = 1024;

// One past the brace that closes the JSON object opening at text[open];
// npos if it never closes. Braces inside strings do not count.
size_t objectEnd(std::string_view text, size_t open)
{
    int depth = 0;
    bool inString = false;
    for (size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') {
                ++i; // an escaped character cannot end the string
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i + 1;
        }
    }
    return std::string_view::npos;
}

// Null when object has no such key, or is no object at all.
const json *member(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() ? &*it : nullptr;
}

// The value becomes an HTTP header, so only what base64url and percent
// encoding use gets through: a CR or LF in a doctored page must never split
// the request. YouTube's is 558 characters today.
bool isHeaderSafe(const std::string &value)
{
    if (value.empty() || value.size() > MAX_VISITOR_DATA) {
        return false;
    }
    for (char c : value) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                             || c == '%' || c == '_' || c == '=' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string visitorDataIn(const json &config)
{
    const json *context = member(config, "INNERTUBE_CONTEXT");
    const json *client = context ? member(*context, "client") : nullptr;
    const json *visitorData = client ? member(*client, "visitorData") : nullptr;
    if (!visitorData || !visitorData->is_string()) {
        return {};
    }
    std::string value = visitorData->get<std::string>();
    return isHeaderSafe(value) ? value : std::string{};
}

}

HttpRequest request(const innertube::ClientDef &client, const std::string &videoId)
{
    HttpRequest request;
    request.method = "GET";
    request.url = canonicalWatchUrl(videoId);
    request.headers = {{"User-Agent", client.userAgent}};
    return request;
}

std::string visitorData(std::string_view html)
{
    const std::string_view call = "ytcfg.set(";
    for (size_t at = html.find(call); at != std::string_view::npos; at = html.find(call, at + call.size())) {
        // The page also makes ytcfg.set('...') calls; only the object form
        // carries the config, which is what yt-dlp's pattern matches too.
        const size_t open = html.find_first_not_of(" \t\r\n", at + call.size());
        if (open == std::string_view::npos || html[open] != '{') {
            continue;
        }
        const size_t end = objectEnd(html, open);
        if (end == std::string_view::npos) {
            return {};
        }
        const json config = json::parse(html.data() + open, html.data() + end, nullptr, false);
        if (config.is_object()) {
            std::string found = visitorDataIn(config);
            if (!found.empty()) {
                return found;
            }
        }
    }
    return {};
}

}
