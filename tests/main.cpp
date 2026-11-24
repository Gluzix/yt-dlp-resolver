#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

// doctest's own main, but with the Debug C runtime told to report to stderr:
// a failed assertion or an abort() in a Debug build would otherwise open a
// modal window, and an unattended run would wait on it for ever.
int main(int argc, char **argv)
{
#if defined(_MSC_VER)
    // Macros that expand to nothing in a Release build, hence no loop.
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    return doctest::Context(argc, argv).run();
}

#if defined(_MSC_VER) && defined(_DEBUG)
// Asks the modes back rather than failing an assertion to see where it goes:
// if the setting were lost, that would be the window this is here to prevent.
// doctest adds the debugger output to _CRT_ASSERT's while a test runs, so
// the check is for the file and against the window, not for one exact mode.
TEST_CASE("a Debug test run reports to stderr, not to a window")
{
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        CAPTURE(type);
        const int mode = _CrtSetReportMode(type, _CRTDBG_REPORT_MODE);
        CHECK((mode & _CRTDBG_MODE_FILE) != 0);
        CHECK((mode & _CRTDBG_MODE_WNDW) == 0);
    }
    CHECK((_set_abort_behavior(0, 0) & (_WRITE_ABORT_MSG | _CALL_REPORTFAULT)) == 0);
}
#endif
