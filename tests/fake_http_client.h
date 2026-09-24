#pragma once

#include "ytres/http.h"

#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The value of the header called name, or empty.
inline std::string headerValue(const ytres::HttpRequest &request, const std::string &name)
{
    for (const auto &[key, value] : request.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

// Stands in for the network: the InnerTube POST gets the next of
// playerBodyQueue while it lasts, else the asking client's entry in
// playerBodyByClient, else playerBody; any other request (the watch page)
// gets watchPage; and every request is kept for the test to look at, with
// the time it arrived.
class FakeHttpClient : public ytres::HttpClient
{
public:
    using Clock = std::chrono::steady_clock;

    explicit FakeHttpClient(std::string playerBody_, std::string watchPage_ = {})
        : playerBody(std::move(playerBody_)), watchPage(std::move(watchPage_))
    {
    }

    ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &request) override
    {
        requests.push_back(request);
        arrivals.push_back(Clock::now());
        const bool player = request.url.find("/youtubei/v1/player") != std::string::npos;
        // A request slower than its timeout waits the timeout out and fails,
        // as the HttpClient contract has a real one do.
        const std::chrono::milliseconds delay = player ? playerDelay : pageDelay;
        if (delay > std::chrono::milliseconds::zero()) {
            const bool timesOut = delay >= request.timeout;
            waitUntil(arrivals.back() + (timesOut ? request.timeout : delay));
            if (timesOut) {
                return {{ytres::Error::Timeout, "Timeout was reached"}, {}};
            }
        }
        const ytres::Status &failure = player ? playerFailure : pageFailure;
        if (failure.code != ytres::Error::Ok) {
            return {failure, {}};
        }
        std::string body = player ? playerBody : watchPage;
        const auto byClient = playerBodyByClient.find(headerValue(request, "X-YouTube-Client-Name"));
        if (player && !playerBodyQueue.empty()) {
            body = std::move(playerBodyQueue.front());
            playerBodyQueue.pop_front();
        } else if (player && byClient != playerBodyByClient.end()) {
            body = byClient->second;
        }
        ytres::Result<ytres::HttpResponse> result{{}, {player ? playerStatus : pageStatus, std::move(body)}};
        if (result.value.status >= 400) {
            // what the HttpClient contract asks of every client
            result.status = {ytres::Error::Http, "HTTP " + std::to_string(result.value.status)};
        }
        return result;
    }

    std::string playerBody;
    std::deque<std::string> playerBodyQueue; // answered in turn, ahead of the rest
    std::map<std::string, std::string> playerBodyByClient; // by X-YouTube-Client-Name, ahead of playerBody
    std::string watchPage;
    long playerStatus{200};
    long pageStatus{200};
    ytres::Status playerFailure; // not Ok: returned instead of a response
    ytres::Status pageFailure;
    std::chrono::milliseconds playerDelay{0}; // how long each request takes to answer
    std::chrono::milliseconds pageDelay{0};
    std::vector<ytres::HttpRequest> requests;
    std::vector<Clock::time_point> arrivals; // one per request

private:
    // Never early, whatever the platform's sleep does: a test that measures
    // what is left of a deadline needs the time really spent.
    static void waitUntil(Clock::time_point until)
    {
        while (Clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// A recorded response from tests/fixtures, byte for byte.
inline std::string readFixture(const std::string &name)
{
    std::ifstream file(std::string(YTRES_FIXTURES) + "/" + name, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
