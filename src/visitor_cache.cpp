#include "visitor_cache.h"

#include <utility>

namespace ytres {

VisitorCache::Entry VisitorCache::get(Clock::time_point now) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return {value, !value.empty() && now - storedAt < LIFETIME};
}

void VisitorCache::put(std::string newValue, Clock::time_point now)
{
    if (newValue.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    value = std::move(newValue);
    storedAt = now;
}

}
