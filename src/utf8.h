#pragma once

#include <cstddef>
#include <string_view>

// Whether text is well-formed UTF-8, as RFC 3629 and Unicode's table 3-7
// define it. The library sends a caller's strings as they are and lets the
// JSON writer put U+FFFD in place of broken bytes; this only lets it say so
// first, since a query that went out mangled can still come back Ok.
// Portable, pure and locale-free.
namespace ytres::utf8 {

inline bool isValid(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        // How many continuation bytes follow, and the range the first of them
        // must fall in: narrower than 80-BF after E0, ED, F0 and F4, which
        // rules out overlong forms, the surrogates and code points past
        // U+10FFFF. C0, C1 and F5-FF never start a character.
        std::size_t continuations = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuations = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            continuations = 2;
            low = lead == 0xE0 ? 0xA0 : 0x80;
            high = lead == 0xED ? 0x9F : 0xBF;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            continuations = 3;
            low = lead == 0xF0 ? 0x90 : 0x80;
            high = lead == 0xF4 ? 0x8F : 0xBF;
        } else {
            return false; // a stray continuation byte, or no lead byte at all
        }
        if (text.size() - i - 1 < continuations) {
            return false; // cut off at the end
        }
        for (std::size_t k = 1; k <= continuations; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if (next < low || next > high) {
                return false;
            }
            low = 0x80;
            high = 0xBF;
        }
        i += continuations + 1;
    }
    return true;
}

}
