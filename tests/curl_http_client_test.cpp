#include "ytres/http.h"

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <stdexcept>

using ytres::Error;
using namespace std::chrono_literals;

// The libcurl client's answers that come before anything is sent, so these
// stay offline. The address is local all the same: were the checks to let a
// request through, it would go nowhere beyond this machine.
namespace {

ytres::HttpRequest localRequest()
{
    ytres::HttpRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:9/";
    request.timeout = 100ms;
    return request;
}

}

TEST_CASE("the libcurl client cancels before sending when the check says so")
{
    ytres::HttpRequest request = localRequest();
    request.cancelled = [] { return true; };
    CHECK(ytres::makeCurlHttpClient()->send(request).status.code == Error::Cancelled);
}

TEST_CASE("a cancel check that throws is Internal in the libcurl client too, not Cancelled")
{
    ytres::HttpRequest request = localRequest();
    request.cancelled = []() -> bool { throw std::runtime_error("the check broke"); };
    const ytres::Result<ytres::HttpResponse> result = ytres::makeCurlHttpClient()->send(request);
    CHECK(result.status.code == Error::Internal);
    CHECK(result.status.message == "The cancel check threw");
}
