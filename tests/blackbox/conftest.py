"""Options, fixtures, the live gate and the end-of-run report of the black-box tests.

    pytest tests/blackbox                 the offline part: no request leaves the machine
    pytest tests/blackbox --live          everything; talks to YouTube, paced
    --ytres-cli PATH                      default $YTRES_CLI, else build/Debug/ytres_cli(.exe)
    --yt-dlp PATH                         default $YT_DLP, else the one on PATH

Live tests carry the `live` marker (pytestmark in their module). Without
--live they are skipped; with it, the first bot check skips every live test
after it. A ytres_cli that hangs or dies skips every test after it, live or
not (harness.Halt).
"""

from __future__ import annotations

import os
import shutil
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional, Tuple

import pytest

import harness

# A proxy nobody listens on. ytres_cli's libcurl honours HTTPS_PROXY (the
# library sets no proxy of its own), so in an offline run any request it
# tried would fail there, on this machine, instead of reaching YouTube - and
# the test that tried would fail with Network. test_offline.py proves the
# trap holds.
DEAD_PROXY = 'http://127.0.0.1:9'
_PROXY_VARIABLES = ('HTTPS_PROXY', 'HTTP_PROXY', 'ALL_PROXY')


@dataclass
class Settings:
    live: bool
    ytres_cli: Path
    ytdlp: Optional[Path]
    ytdlp_missing: str  # why there is no yt-dlp, when there is none


@dataclass
class Row:
    """What the tests learnt about one video, search or playlist, for the end of the run."""
    kind: str
    name: str
    ours: str = '-'
    ours_seconds: Optional[float] = None
    theirs: str = '-'
    theirs_seconds: Optional[float] = None

    def ytres(self, said: str, seconds: Optional[float]) -> None:
        self.ours, self.ours_seconds = harness.redact(said), seconds

    def ytdlp(self, said: str, seconds: Optional[float]) -> None:
        self.theirs, self.theirs_seconds = harness.redact(said), seconds


@dataclass
class RunState:
    """What one pytest run keeps: the launchers, the rows tests recorded and how each test ended."""
    settings: Settings
    brake: harness.Brake
    live_launcher: harness.Launcher
    offline_launcher: harness.Launcher
    rows: Dict[Tuple[str, str], Row] = field(default_factory=dict)
    row_of_test: Dict[str, List[Tuple[str, str]]] = field(default_factory=dict)
    outcomes: Dict[str, str] = field(default_factory=dict)
    started: float = field(default_factory=time.monotonic)


STATE = pytest.StashKey[RunState]()


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup('ytres black-box tests')
    group.addoption('--live', action='store_true', default=False,
                    help='also run the tests that talk to YouTube (paced; stops at the first bot check)')
    group.addoption('--ytres-cli', default=None, metavar='PATH',
                    help=f'ytres_cli to test (default: $YTRES_CLI, else {harness.DEFAULT_YTRES_CLI})')
    group.addoption('--yt-dlp', dest='yt_dlp', default=None, metavar='PATH',
                    help='yt-dlp to compare with (default: $YT_DLP, else the one on PATH)')


def _settings(config: pytest.Config) -> Settings:
    cli = Path(config.getoption('ytres_cli') or os.environ.get('YTRES_CLI') or harness.DEFAULT_YTRES_CLI)
    if not cli.is_file():
        # The whole run stops here: every test needs it, the offline ones too.
        raise pytest.UsageError(f'ytres_cli not found at {cli}: build it (cmake --build build --config Debug) '
                                'or pass --ytres-cli PATH')
    given = config.getoption('yt_dlp') or os.environ.get('YT_DLP')
    if given:
        ytdlp = Path(given) if Path(given).is_file() else None
        missing = '' if ytdlp else f'yt-dlp not found at {given}'
    else:
        found = shutil.which('yt-dlp')
        ytdlp = Path(found) if found else None
        missing = '' if ytdlp else 'yt-dlp is not on PATH; pass --yt-dlp PATH or set YT_DLP'
    return Settings(config.getoption('live'), cli.resolve(), ytdlp, missing)


def _offline_environment() -> Dict[str, str]:
    """This environment, with every proxy variable pointing at the dead proxy and no exceptions to it."""
    env = {name: value for name, value in os.environ.items()
           if name.upper() not in _PROXY_VARIABLES + ('NO_PROXY',)}
    for name in _PROXY_VARIABLES:
        env[name] = DEAD_PROXY
        if os.name != 'nt':  # libcurl reads the lower-case names first; Windows has no case
            env[name.lower()] = DEAD_PROXY
    return env


def _forbid_sockets() -> None:
    """The suite's own Python never opens a connection in an offline run; this makes sure."""
    def refuse(*_args: Any, **_kwargs: Any) -> Any:
        raise RuntimeError('an offline run tried to open a network connection')
    socket.socket.connect = refuse  # type: ignore[method-assign]
    socket.socket.connect_ex = refuse  # type: ignore[method-assign]
    socket.create_connection = refuse  # type: ignore[assignment]


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line('markers', 'live: talks to YouTube; runs only with --live')
    settings = _settings(config)
    if not settings.live:
        _forbid_sockets()
    brake = harness.Brake()
    config.stash[STATE] = RunState(
        settings=settings,
        brake=brake,
        live_launcher=harness.Launcher(harness.Pacer(), brake=brake),
        offline_launcher=harness.Launcher(env=_offline_environment(), brake=brake),
    )


def pytest_collection_modifyitems(config: pytest.Config, items: List[pytest.Item]) -> None:
    if config.stash[STATE].settings.live:
        return
    skip = pytest.mark.skip(reason='talks to YouTube; run with --live')
    for item in items:
        if item.get_closest_marker('live'):
            item.add_marker(skip)


def pytest_runtest_setup(item: pytest.Item) -> None:
    brake = item.config.stash[STATE].brake
    if brake.halt:
        pytest.skip(f'run halted: {brake.halt}')
    if brake.bot_check and item.get_closest_marker('live'):
        pytest.skip(f'live requests stopped: {brake.bot_check}')


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo[None]) -> Iterator[None]:
    outcome = yield
    report: pytest.TestReport = outcome.get_result()
    _redact_report(report)
    # The test's verdict for the table: failed if any phase failed, else how
    # its call (or a setup that skipped it) ended.
    outcomes = item.config.stash[STATE].outcomes
    if report.failed:
        outcomes[item.nodeid] = 'failed'
    elif report.when == 'call' or report.skipped:
        outcomes.setdefault(item.nodeid, report.outcome)


def _redact_report(report: pytest.TestReport) -> None:
    """No stream url or address in what pytest prints or writes to --junitxml, whatever a test let slip."""
    if report.longrepr is not None:
        if isinstance(report.longrepr, tuple):  # a skip: (file, line, reason)
            path, line, reason = report.longrepr
            report.longrepr = (path, line, harness.redact(reason))
        else:
            report.longrepr = harness.redact(str(report.longrepr))
    report.sections = [(title, harness.redact(content)) for title, content in report.sections]


# --- The report ------------------------------------------------------------

_KIND_ORDER = {'video': 0, 'search': 1, 'playlist': 2, 'dump': 3}


def _verdict(outcomes: List[str]) -> str:
    if 'failed' in outcomes:
        return 'FAILED'
    if outcomes and all(outcome == 'passed' for outcome in outcomes):
        return 'ok'
    return 'skipped' if 'passed' not in outcomes else 'partly skipped'


def _seconds(value: Optional[float]) -> str:
    return '-' if value is None else f'{value:.1f} s'


def _cut(text: str, width: int) -> str:
    return text if len(text) <= width else text[:width - 1] + '~'


def report_lines(state: RunState) -> List[str]:
    """The end-of-run table: one row per video, search and playlist, then the program starts."""
    tests_of: Dict[Tuple[str, str], List[str]] = {}
    for nodeid, keys in state.row_of_test.items():
        for key in keys:
            tests_of.setdefault(key, []).append(state.outcomes.get(nodeid, 'not run'))
    lines = []
    if state.rows:
        lines.append(f'{"kind":<9}{"name":<38}{"ytres":<30}{"took":>7}  {"yt-dlp":<28}{"took":>7}  verdict')
        # Grouped by kind in the plan's order; within a kind, as the tests ran.
        ordered = sorted(state.rows.items(), key=lambda item: _KIND_ORDER.get(item[1].kind, len(_KIND_ORDER)))
        for key, row in ordered:
            lines.append(f'{row.kind:<9}{_cut(row.name, 37):<38}{_cut(row.ours, 29):<30}{_seconds(row.ours_seconds):>7}'
                         f'  {_cut(row.theirs, 27):<28}{_seconds(row.theirs_seconds):>7}  {_verdict(tests_of.get(key, []))}')
        lines.append('')
    live = dict(state.live_launcher.starts)
    offline = dict(state.offline_launcher.starts)
    lines.append(f'programs started: live {live or "none"}; offline, behind the dead proxy, {offline or "none"}; '
                 f'{time.monotonic() - state.started:.0f} s')
    if state.brake.halt:
        lines.append(f'RUN HALTED: {state.brake.halt}')
    if state.brake.bot_check:
        lines.append(f'LIVE REQUESTS STOPPED: {state.brake.bot_check}')
    return [harness.redact(line) for line in lines]


def pytest_terminal_summary(terminalreporter: Any, exitstatus: int, config: pytest.Config) -> None:
    state = config.stash.get(STATE, None)
    if state is None:
        return
    terminalreporter.section('ytres black-box report')
    # A title may hold characters the console's code page lacks; they become ?
    # rather than end the run in an encoding error.
    encoding = getattr(sys.stdout, 'encoding', None) or 'utf-8'
    for line in report_lines(state):
        terminalreporter.write_line(line.encode(encoding, 'replace').decode(encoding, 'replace'))


# --- Fixtures --------------------------------------------------------------

class Remembering:
    """Calls through to an object and remembers each answer.

    Two tests that ask the same question (ytres's answer, and its comparison
    with yt-dlp's) so start the program once.
    """

    def __init__(self, inner: Any) -> None:
        self._inner = inner
        self._answers: Dict[Tuple[Any, ...], Any] = {}

    def __getattr__(self, name: str) -> Callable[..., Any]:
        method = getattr(self._inner, name)

        def remembered(*args: Any, **kwargs: Any) -> Any:
            key = (name, args, tuple(sorted(kwargs.items())))
            if key not in self._answers:
                self._answers[key] = method(*args, **kwargs)
            return self._answers[key]
        return remembered


@pytest.fixture(scope='session')
def settings(pytestconfig: pytest.Config) -> Settings:
    return pytestconfig.stash[STATE].settings


@pytest.fixture(scope='session')
def offline_ytres(pytestconfig: pytest.Config) -> harness.Ytres:
    """ytres_cli behind the dead proxy, unpaced: for the answers that come before any request."""
    state = pytestconfig.stash[STATE]
    return harness.Ytres(state.settings.ytres_cli, state.offline_launcher)


@pytest.fixture(scope='session')
def ytres(pytestconfig: pytest.Config) -> harness.Ytres:
    """ytres_cli against live YouTube: paced, and each answer remembered for the run."""
    state = pytestconfig.stash[STATE]
    return Remembering(harness.Ytres(state.settings.ytres_cli, state.live_launcher))  # type: ignore[return-value]


@pytest.fixture(scope='session')
def ytdlp(pytestconfig: pytest.Config) -> harness.YtDlp:
    """yt-dlp against live YouTube, or a skip with the reason when there is none."""
    state = pytestconfig.stash[STATE]
    if state.settings.ytdlp is None:
        pytest.skip(f'no comparison with yt-dlp: {state.settings.ytdlp_missing}')
    return Remembering(harness.YtDlp(state.settings.ytdlp, state.live_launcher))  # type: ignore[return-value]


@pytest.fixture
def record(request: pytest.FixtureRequest) -> Callable[[str, str], Row]:
    """record(kind, name): the table row for a video, search or playlist, for the test to fill in."""
    state = request.config.stash[STATE]

    def row(kind: str, name: str) -> Row:
        key = (kind, harness.redact(name))
        found = state.rows.setdefault(key, Row(kind, key[1]))
        keys = state.row_of_test.setdefault(request.node.nodeid, [])
        if key not in keys:
            keys.append(key)
        return found
    return row
