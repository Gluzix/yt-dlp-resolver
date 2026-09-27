#pragma once

#include <chrono>
#include <mutex>
#include <string>

// The watch page's visitor data, kept from one resolve to the next. Fetching
// the page is most of a cold resolve (docs/innertube-notes.md, Verified
// live), and one value serves every video, every client and every thread.
// =======================================================
// Rules:
// - Safe to call from several threads at once. The mutex guards the value
//   alone and is never held across a request, so two cold resolves that
//   race both fetch the page, and the later value wins.
// - A value is fresh for LIFETIME after it was stored. That is belt and
//   braces against one that quietly stopped working, not a limit YouTube
//   sets.
// - A stale value stays until a new one replaces it: an old visitor id still
//   beats none when the page cannot be fetched.
// - The clock comes in as now, so expiry is tested without waiting for it.
// =======================================================
namespace ytres {

class VisitorCache
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::hours LIFETIME{6};

    struct Entry
    {
        std::string value;  // empty when nothing was ever stored
        bool fresh{false};  // stored less than LIFETIME before now
    };

    Entry get(Clock::time_point now) const;

    // An empty newValue is ignored: it would only push out one that may still work.
    void put(std::string newValue, Clock::time_point now);

private:
    mutable std::mutex mutex;
    std::string value;
    Clock::time_point storedAt;
};

}
