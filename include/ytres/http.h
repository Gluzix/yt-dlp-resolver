#pragma once

#include "ytres/ytres.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The resolver's only way onto the network. It builds the requests, an
// HttpClient carries them: libcurl by default, a fake that replays recorded
// responses in the tests, whatever another platform prefers.
// =======================================================
// Rules:
// - send() never throws. A failure comes back as a Status - Cancelled,
//   Timeout, Network, or Http for a status of 400 and up, with the response
//   still in value.
// - send() gives up once timeout has passed, connecting included, and polls
//   cancelled while it waits.
// - One client serves every thread its Resolver is called from, so send()
//   must be safe to call concurrently.
// =======================================================
namespace ytres {

struct HttpRequest
{
    std::string method, url, body;    // method is GET or POST
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds timeout{std::chrono::seconds{10}};
    std::function<bool()> cancelled;  // may be empty
};

struct HttpResponse
{
    long status{0};
    std::string body;
};

class HttpClient
{
public:
    virtual ~HttpClient() = default;
    virtual Result<HttpResponse> send(const HttpRequest &request) = 0;
};

// The built-in client on libcurl, the one a Resolver makes when
// Options::http is null. For a caller that wants to wrap it - to record,
// throttle or log - rather than replace it.
std::shared_ptr<HttpClient> makeCurlHttpClient();

}
