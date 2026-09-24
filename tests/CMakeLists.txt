find_package(doctest CONFIG REQUIRED)

# Offline by construction: every Resolver in here gets a FakeHttpClient.
add_executable(ytres_tests
    main.cpp
    fake_http_client.h
    test_resolver.h
    url_parse_test.cpp
    format_pick_test.cpp
    player_request_test.cpp
    player_response_test.cpp
    watch_page_test.cpp
    resolver_test.cpp
    visitor_cache_test.cpp
    client_ladder_test.cpp
    deadline_test.cpp
)
target_link_libraries(ytres_tests PRIVATE ytres doctest::doctest nlohmann_json::nlohmann_json)
# The internals are tested directly, not only through the public API.
target_include_directories(ytres_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(ytres_tests PRIVATE YTRES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
ytres_compile_options(ytres_tests)

add_test(NAME ytres_tests COMMAND ytres_tests)
