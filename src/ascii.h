#pragma once

#include <string>
#include <string_view>

namespace ytres {

// Case folding for ASCII only - hosts, schemes and YouTube's English reason
// texts - so that no locale can change the answer.
inline std::string asciiLower(std::string_view text)
{
    std::string lower(text);
    for (char &c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

}
