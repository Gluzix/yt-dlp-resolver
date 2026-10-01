#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// What visitor data must be before it may go out as X-Goog-Visitor-Id,
// wherever the library read it: a watch page's ytcfg, or a search answer's
// responseContext.
// =======================================================
// Rules:
// - One check for every source, so that no answer of YouTube's, doctored or
//   grown, can put into a header what another source could not.
// - Only what base64 and percent encoding use gets through: a CR or LF in a
//   doctored answer must never split the request.
// =======================================================
namespace ytres {

// YouTube's is 558 characters today.
inline constexpr std::size_t MAX_VISITOR_DATA = 4096;

// Why value may not go out as a header; empty when it may.
inline std::string visitorDataRefusal(std::string_view value)
{
    if (value.empty()) {
        return "empty";
    }
    if (value.size() > MAX_VISITOR_DATA) {
        return std::to_string(value.size()) + " characters, over " + std::to_string(MAX_VISITOR_DATA);
    }
    for (const char c : value) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                             || c == '%' || c == '_' || c == '=' || c == '+' || c == '/' || c == '-';
        if (!allowed) {
            return "a character outside [A-Za-z0-9%_=+/-]";
        }
    }
    return {};
}

}
