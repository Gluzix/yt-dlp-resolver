#pragma once

#include <cstddef>
#include <regex>
#include <string>

// What ytres_cli takes out of a --dump before it is written, so that a dump
// can be committed as a fixture. A header of its own so that ytres_tests can
// pin it; it is harness code, not the library's.
namespace ytres_cli {

// text with every match of pattern replaced by its group 1, then
// replacement, then its group 2 (empty when the pattern has no second
// group). By hand, because "$2" + "203..." would read as group 22.
inline std::string replaceAround(const std::string &text, const std::regex &pattern, const std::string &replacement)
{
    std::string out;
    std::size_t copied = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) {
        const std::smatch &match = *it;
        out.append(text, copied, static_cast<std::size_t>(match.position(0)) - copied);
        out += match.str(1) + replacement + (match.size() > 2 ? match.str(2) : std::string{});
        copied = static_cast<std::size_t>(match.position(0) + match.length(0));
    }
    out.append(text, copied, std::string::npos);
    return out;
}

// Every stream url names the requesting address: ?ip=/&ip= in queries,
// /ip/<addr>/ in hlsManifestUrl's path, IPv6 percent-encoded. A url inside
// signatureCipher is percent-encoded whole (%26ip%3D, %2Fip%2F), and a page
// may escape its ampersands as \u0026, so the separators come in those
// forms too. The address itself is hex digits, dots and colons, a colon
// possibly written %3A or, encoded twice, %253A - and nothing else, so that
// the %26 after an encoded address is left for the next parameter.
// Visitor data identifies the visitor to YouTube, and carries the country,
// so it goes too, under both names it travels by: "visitorData", and the web
// client's tracking parameter "visitor_data", whose key and value may come
// in either order. A JSON string is read whole, escaped quotes and all, and
// whitespace may sit around the colons.
inline std::string scrubbed(std::string text)
{
    const std::string address = R"((?:[0-9A-Fa-f.:]|%3[Aa]|%253[Aa])+)";
    const std::string jsonString = R"("(?:[^"\\]|\\.)*")";
    text = replaceAround(text, std::regex(R"(((?:[?&]|%26|%3[Ff]|\\u0026)ip(?:=|%3[Dd])))" + address), "203.0.113.7");
    text = replaceAround(text, std::regex(R"(((?:/|%2[Ff])ip(?:/|%2[Ff])))" + address + R"((/|%2[Ff]))"), "203.0.113.7");
    text = std::regex_replace(text, std::regex(R"("key"\s*:\s*"visitor_data"\s*,\s*"value"\s*:\s*)" + jsonString),
                              R"("key":"visitor_data","value":"FIXTURE")");
    text = std::regex_replace(text, std::regex(R"("value"\s*:\s*)" + jsonString + R"(\s*,\s*"key"\s*:\s*"visitor_data")"),
                              R"("value":"FIXTURE","key":"visitor_data")");
    return std::regex_replace(text, std::regex(R"("visitorData"\s*:\s*)" + jsonString), R"("visitorData":"FIXTURE")");
}

}
