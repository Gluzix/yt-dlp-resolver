"""Running ytres_cli and yt-dlp, and reading what they print, as plain data.

Everything the black-box tests need from the two programs lives here, with
no pytest in it, so that the pure parts (the parsers, the corpus readers,
redact) can be tested offline on their own and reused by a script.

The contract read here is ytres_cli's (cli/main.cpp, README.md):
  ytres_cli <url-or-id>            title, page url, best-audio stream url
  ytres_cli --search <query>       url TAB seconds|live|upcoming TAB channel TAB title
  ytres_cli --playlist <url-or-id> title TAB count, then url TAB seconds TAB title
and every failure is "<Error>: <message>" on stderr with exit code 1.
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Generic, List, Mapping, Optional, Sequence, Tuple, TypeVar
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[2]
EXE_SUFFIX = '.exe' if sys.platform == 'win32' else ''
DEFAULT_YTRES_CLI = ROOT / 'build' / 'Debug' / ('ytres_cli' + EXE_SUFFIX)
VIDEO_CORPUS = ROOT / 'tests' / 'corpus.txt'
SEARCH_CORPUS = Path(__file__).resolve().parent / 'corpus_search.txt'
PLAYLIST_CORPUS = Path(__file__).resolve().parent / 'corpus_playlists.txt'

# How long a program may run before it is killed. yt-dlp lists 150 playlist
# entries in a few seconds on a good line; two minutes is stuck, not slow.
RUN_TIMEOUT_SECONDS = 120.0

# ytres_cli answers in a second or two; its own request timeouts end any
# call well inside a minute. One still running after that is hung, or
# waiting on a window nobody will close, and the whole run stops (Halt).
YTRES_TIMEOUT_SECONDS = 60.0

# The exit codes ytres_cli's contract allows: 0, or 1 with "<Error>: ...".
# Anything else (3 for an abort(), a negative NTSTATUS for a crash) is the
# program dying, and the whole run stops.
YTRES_EXIT_CODES = (0, 1)

# The least time between one program start and the next in a live run.
PACE_SECONDS = 1.5

# The only address a dump or a fixture may hold (RFC 5737 documentation range).
PLACEHOLDER_ADDRESS = '203.0.113.7'

# ytres::Error, in the names ytres_cli prints (errorName() in cli/main.cpp).
ERROR_NAMES = ('Ok', 'Cancelled', 'Timeout', 'Network', 'Http', 'Parse', 'Unavailable', 'AgeRestricted',
               'GeoBlocked', 'LoginRequired', 'NoFormats', 'PlayerScript', 'BadInput', 'BotCheck', 'Internal')

VIDEO_ID = re.compile(r'[0-9A-Za-z_-]{11}')
WATCH_URL = re.compile(r'https://www\.youtube\.com/watch\?v=([0-9A-Za-z_-]{11})')

T = TypeVar('T')


# --- Redaction -------------------------------------------------------------
#
# A stream url names the address that asked for it (ip=<address>, or
# /ip/<address>/ in a manifest's path). Nothing the suite prints, asserts on
# or writes may show one, so every message goes through redact().

_URL = re.compile(r'https?://[^\s"\'<>\\]+')
# An address parameter, plain or percent-encoded, in a url that _URL did not
# take whole (a url cut by an escape, say).
_ADDRESS_PARAMETER = re.compile(r'((?:[?&/]|%26|%3[Ff]|%2[Ff]|\\u0026)ip(?:=|/|%3[Dd]|%2[Ff]))(?:[0-9A-Fa-f.:]|%3[Aa]|%253[Aa])+')
_IPV4 = re.compile(r'(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])')
# Four or more colon-separated hex groups: an IPv6 address, never a length
# such as 1:02:03, which has three at most.
_IPV6 = re.compile(r'(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){3,7}[0-9A-Fa-f]{1,4}(?![\w:.])')


def _is_stream_url(url: str) -> bool:
    """A url that may carry the requester's address: a media host's, or any with an ip parameter."""
    host = (urlsplit(url).hostname or '').lower()
    return host.endswith('googlevideo.com') or bool(re.search(r'[?&]ip=|/ip/', url))


def _reduce_url(match: 're.Match[str]') -> str:
    url = match.group(0)
    if not _is_stream_url(url):
        return url
    # The host is left out too: the proof that a run leaks no stream url is a
    # search of its output for the media host's name, which must find nothing.
    itag = itag_of(url)
    return f'<stream url, itag {itag}>' if itag else '<stream url>'


def redact(text: str) -> str:
    """text with every stream url reduced to its itag, and every address but the placeholder hidden."""
    text = _URL.sub(_reduce_url, text)
    text = _ADDRESS_PARAMETER.sub(lambda m: m.group(1) + '<redacted>', text)
    text = _IPV4.sub(lambda m: m.group(0) if m.group(0) == PLACEHOLDER_ADDRESS else '<redacted>', text)
    return _IPV6.sub('<redacted>', text)


def ipv4_addresses_in(text: str) -> List[str]:
    """Every dotted-quad address in text, the placeholder among them."""
    return _IPV4.findall(text)


# --- Urls ------------------------------------------------------------------

def query_value(url: str, name: str) -> Optional[str]:
    """The first value of a query parameter, or None."""
    values = parse_qs(urlsplit(url).query).get(name)
    return values[0] if values else None


def itag_of(url: str) -> Optional[str]:
    return query_value(url, 'itag')


def expire_of(url: str) -> Optional[int]:
    """The unix time a stream url stops working, when it says."""
    value = query_value(url, 'expire')
    return int(value) if value and value.isdigit() else None


def host_of(url: str) -> str:
    return (urlsplit(url).hostname or '').lower()


def video_id_of(watch_url: str) -> Optional[str]:
    """The id in a canonical https://www.youtube.com/watch?v=<id>, or None for any other form."""
    match = WATCH_URL.fullmatch(watch_url)
    return match.group(1) if match else None


def is_video_id(text: str) -> bool:
    return VIDEO_ID.fullmatch(text) is not None


def list_id_of(target: str) -> str:
    """The playlist id in a link's list= parameter, or target itself when it is a bare id."""
    if '://' in target:
        return query_value(target, 'list') or ''
    return target


def playlist_url(list_id: str) -> str:
    return 'https://www.youtube.com/playlist?list=' + list_id


# --- Running a program -----------------------------------------------------

@dataclass(frozen=True, repr=False)
class Completed:
    """A finished (or killed) program: what it printed, decoded as UTF-8."""
    command: Tuple[str, ...]
    exit_code: Optional[int]  # None: it ran out of time and was killed
    stdout: str
    stderr: str
    seconds: float

    def __repr__(self) -> str:
        # pytest shows the repr of what an assertion touched; it must not
        # show a stream url.
        return redact(f'Completed(exit_code={self.exit_code}, seconds={self.seconds:.1f}, '
                      f'stdout={self.stdout!r}, stderr={self.stderr!r})')


def _decode(data: bytes) -> str:
    # Windows' text-mode stdout turns \n into \r\n. Only that pair is undone,
    # rather than splitlines(), which would also split a title at U+2028.
    return data.decode('utf-8', 'replace').replace('\r\n', '\n')


def lines_of(text: str) -> List[str]:
    """The lines of a program's output, without the empty one after the last newline."""
    return text.split('\n')[:-1] if text.endswith('\n') else ([text] if text else [])


# How long killing a program and reading what it left may take; taskkill is
# a program too, and nothing here waits without a limit.
KILL_TIMEOUT_SECONDS = 30.0


def kill_tree(process: 'subprocess.Popen[bytes]') -> None:
    """Kills process and whatever it started."""
    if sys.platform == 'win32':
        # yt-dlp.exe is a PyInstaller launcher that runs yt-dlp as its child.
        # Killing the launcher alone leaves the child holding the pipes, and
        # reading them then waits for as long as the child lives.
        try:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True, check=False,
                           timeout=KILL_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            pass
        process.kill()  # in case taskkill could not; harmless when it did
    else:
        # The process leads a session of its own (see run()), so its group
        # is it and its children.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


def quiet_crashes() -> None:
    """On Windows, no program this process starts may show a crash window.

    A child inherits its parent's error mode. With these flags a crash in
    ytres_cli or yt-dlp ends the program with an exit code instead of
    Windows Error Reporting's "has stopped working" window, which would sit
    on the owner's screen until someone closed it. ytres_cli's own Debug
    reports (assertions, abort()) already go to stderr; this covers what
    the C runtime does not see, such as an access violation.
    """
    if sys.platform != 'win32':
        return
    import ctypes
    kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
    wanted = _SEM_FAILCRITICALERRORS | _SEM_NOGPFAULTERRORBOX | _SEM_NOOPENFILEERRORBOX
    kernel32.SetErrorMode(kernel32.GetErrorMode() | wanted)


def run(command: Sequence[str], timeout: float = RUN_TIMEOUT_SECONDS,
        env: Optional[Mapping[str, str]] = None) -> Completed:
    """Runs command without a shell and waits up to timeout seconds for it; past that it is killed."""
    quiet_crashes()
    arguments = [str(part) for part in command]
    extra = {} if sys.platform == 'win32' else {'start_new_session': True}
    start = time.monotonic()
    process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=dict(env) if env is not None else None, **extra)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        code: Optional[int] = process.returncode
    except subprocess.TimeoutExpired:
        kill_tree(process)
        code = None
        try:
            stdout, stderr = process.communicate(timeout=KILL_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            # Something outside the tree still holds the pipes: give up on
            # what it printed rather than wait for it.
            stdout, stderr = b'', b''
    return Completed(tuple(arguments), code, _decode(stdout), _decode(stderr), time.monotonic() - start)


class Pacer:
    """At least `interval` seconds between one program start and the next.

    The one place a live run's pace is kept, so that YouTube sees no burst.
    """

    def __init__(self, interval: float = PACE_SECONDS, clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep) -> None:
        self.interval = interval
        self._clock = clock
        self._sleep = sleep
        self._last: Optional[float] = None

    def wait(self) -> float:
        """Waits until the next start may happen, marks it, and returns how long it waited."""
        now = self._clock()
        waited = 0.0
        if self._last is not None and now < self._last + self.interval:
            waited = self._last + self.interval - now
            self._sleep(waited)
            now = self._clock()
        self._last = now
        return waited


# ytres_cli's BotCheck, YouTube's own words (with either apostrophe), and a
# 429 from either program. Read on stderr only: a title may say anything.
# ytres says "HTTP 429" in an Http failure, yt-dlp "HTTP Error 429: Too Many
# Requests".
_BOT_CHECK = re.compile(r'^BotCheck:|not a bot|\bHTTP (?:Error )?429\b|Too Many Requests', re.IGNORECASE)


def bot_check_in(stderr: str) -> Optional[str]:
    """The line of stderr that shows a bot check or a rate limit, redacted, or None."""
    for line in lines_of(stderr):
        if _BOT_CHECK.search(line):
            return redact(line.strip())
    return None


class BotCheckStop(Exception):
    """YouTube asked for proof that we are no bot: every live request stops here."""


class Halt(Exception):
    """ytres_cli hung or died: no program is started again in this run."""


@dataclass
class Brake:
    """Why a run stopped starting programs, shared by every launcher of the run.

    bot_check stops the live requests only; halt stops everything, since a
    ytres_cli that hangs or dies once would only do it again - and a hung
    one may be a window on the owner's screen.
    """
    bot_check: Optional[str] = None
    halt: Optional[str] = None


def _ytres_died(done: Completed) -> Optional[str]:
    """Why a ytres_cli run counts as hung or dead, or None for an answer within its contract."""
    if done.exit_code is None:
        return f'ytres_cli was still running after {done.seconds:.0f} s and was killed (a hang, or a window?)'
    if done.exit_code not in YTRES_EXIT_CODES:
        last = (lines_of(done.stderr) or ['nothing on stderr'])[-1]
        return redact(f'ytres_cli exited with code {done.exit_code}, outside its contract: {last}')
    return None


class Launcher:
    """Starts every program of a run: paced, counted, timed out, and stopped by its Brake.

    live: whether what it starts talks to YouTube. A bot check stops only the
    live launchers; one behind the dead proxy asks YouTube nothing, so its
    tests still run after one.
    """

    def __init__(self, pacer: Optional[Pacer] = None, timeout: float = RUN_TIMEOUT_SECONDS,
                 env: Optional[Mapping[str, str]] = None, brake: Optional[Brake] = None,
                 live: bool = True) -> None:
        self.pacer = pacer
        self.timeout = timeout
        self.env = env
        self.brake = brake if brake is not None else Brake()
        self.live = live
        self.starts: Counter[str] = Counter()

    @property
    def stopped(self) -> Optional[str]:
        """Why this launcher starts nothing more, or None."""
        return self.brake.halt or (self.brake.bot_check if self.live else None)

    def __call__(self, name: str, command: Sequence[str], timeout: Optional[float] = None) -> Completed:
        if self.brake.halt:
            raise Halt(self.brake.halt)
        if self.live and self.brake.bot_check:
            raise BotCheckStop(self.brake.bot_check)
        if self.pacer:
            self.pacer.wait()
        self.starts[name] += 1
        done = run(command, timeout if timeout is not None else self.timeout, self.env)
        died = _ytres_died(done) if name == 'ytres_cli' else None
        if died:
            self.brake.halt = died
            raise Halt(died)
        reason = bot_check_in(done.stderr)
        if reason:
            self.brake.bot_check = f'{name} met a bot check: {reason}'
            raise BotCheckStop(self.brake.bot_check)
        return done


# --- What the programs said ------------------------------------------------

@dataclass(frozen=True)
class Failure:
    """A program's refusal: ytres_cli's Error name (or the harness's word for it) and its message."""
    code: str
    message: str

    def __str__(self) -> str:
        return redact(f'{self.code}: {self.message}')


@dataclass(frozen=True, repr=False)
class Answer(Generic[T]):
    """What one run of a program came to: the value read from stdout, and the failure, if any.

    A search or a playlist that fails on a later page has both.
    """
    value: Optional[T]
    failure: Optional[Failure]
    completed: Completed

    @property
    def ok(self) -> bool:
        return self.failure is None and self.completed.exit_code == 0

    @property
    def seconds(self) -> float:
        return self.completed.seconds

    @property
    def code(self) -> str:
        """The Error name the run ended with: Ok, or its failure's."""
        return 'Ok' if self.ok else (self.failure.code if self.failure else 'Unknown')

    def __repr__(self) -> str:
        return redact(f'Answer(value={self.value!r}, failure={self.failure!r}, exit_code={self.completed.exit_code})')


class OutputError(ValueError):
    """A program printed something that does not follow its contract."""


_ERROR_LINE = re.compile(r'([A-Za-z]+): (.*)')


def parse_failure(stderr: str) -> Optional[Failure]:
    """The last "<Error>: <message>" line of ytres_cli's stderr, or None.

    Log warnings come on stderr too, before the error line; the error line
    is the last one that names one of ytres::Error.
    """
    for line in reversed(lines_of(stderr)):
        match = _ERROR_LINE.fullmatch(line.strip())
        if match and match.group(1) in ERROR_NAMES:
            return Failure(match.group(1), match.group(2))
    return None


@dataclass(frozen=True, repr=False)
class Video:
    """ytres_cli's three lines for a video that plays."""
    title: str
    page_url: str
    stream_url: str

    @property
    def itag(self) -> Optional[str]:
        return itag_of(self.stream_url)

    def __repr__(self) -> str:
        return redact(f'Video(title={self.title!r}, page_url={self.page_url!r}, stream_url={self.stream_url!r})')


def parse_video(stdout: str) -> Video:
    lines = lines_of(stdout)
    if len(lines) != 3:
        raise OutputError(f'expected three lines (title, page url, stream url), got {len(lines)}')
    return Video(*lines)


@dataclass(frozen=True)
class SearchRow:
    """One line of ytres_cli --search."""
    url: str
    length: str  # seconds, "live" or "upcoming"
    channel: str
    title: str

    @property
    def video_id(self) -> Optional[str]:
        return video_id_of(self.url)


def parse_search(stdout: str) -> List[SearchRow]:
    rows = []
    for number, line in enumerate(lines_of(stdout), 1):
        # The title is last, so a tab inside it stays in it.
        fields = line.split('\t', 3)
        if len(fields) != 4:
            raise OutputError(f'search line {number}: expected four tab-separated fields, got {len(fields)}')
        rows.append(SearchRow(*fields))
    return rows


@dataclass(frozen=True)
class PlaylistEntry:
    """One video line of ytres_cli --playlist."""
    url: str
    seconds: int
    title: str

    @property
    def video_id(self) -> Optional[str]:
        return video_id_of(self.url)


@dataclass(frozen=True)
class Listing:
    """ytres_cli --playlist's output: the title and count line, then the entries."""
    title: str
    count: int
    entries: Tuple[PlaylistEntry, ...]

    @property
    def ids(self) -> List[Optional[str]]:
        return [entry.video_id for entry in self.entries]


def parse_playlist(stdout: str) -> Optional[Listing]:
    """The listing, or None when ytres_cli printed nothing (a failure on the first page)."""
    lines = lines_of(stdout)
    if not lines:
        return None
    # The count is last on the first line, so a tab inside the title stays in it.
    title, tab, count = lines[0].rpartition('\t')
    if not tab or not count.isdigit():
        raise OutputError('playlist line 1: expected the title, a tab and the count')
    entries = []
    for number, line in enumerate(lines[1:], 2):
        fields = line.split('\t', 2)
        if len(fields) != 3 or not fields[1].isdigit():
            raise OutputError(f'playlist line {number}: expected url, seconds and title, separated by tabs')
        entries.append(PlaylistEntry(fields[0], int(fields[1]), fields[2]))
    return Listing(title, int(count), tuple(entries))


def answer_from(completed: Completed, parse: Callable[[str], T], partial: bool) -> Answer[T]:
    """completed as an Answer: stdout parsed when it should hold a value, the failure from stderr.

    partial: whether stdout may hold a value beside a failure (a search or a
    playlist that failed on a later page prints what it read first).
    """
    if completed.exit_code is None:
        return Answer(None, Failure('NoAnswer', f'no answer in {completed.seconds:.0f} s'), completed)
    failure = None
    if completed.exit_code != 0:
        failure = parse_failure(completed.stderr) or Failure(
            'Unknown', (lines_of(completed.stderr) or [f'exit code {completed.exit_code}'])[-1])
    value = None
    if completed.exit_code == 0 or partial:
        try:
            value = parse(completed.stdout)
        except OutputError as error:
            if failure is None:
                failure = Failure('Unreadable', str(error))
    return Answer(value, failure, completed)


class Ytres:
    """ytres_cli as Python functions."""

    def __init__(self, cli: Path, launch: Launcher) -> None:
        self.cli = cli
        self.launch = launch

    def raw(self, arguments: Sequence[str]) -> Completed:
        """ytres_cli with these arguments, as it finished, for the tests of its command line."""
        return self.launch('ytres_cli', [str(self.cli), *arguments], YTRES_TIMEOUT_SECONDS)

    @staticmethod
    def _dump(dump: Optional[Path]) -> List[str]:
        return ['--dump', str(dump)] if dump is not None else []

    def resolve(self, target: str, dump: Optional[Path] = None) -> Answer[Video]:
        """A video's title, page url and best-audio stream url; dump writes the scrubbed player response."""
        return answer_from(self.raw([target, *self._dump(dump)]), parse_video, partial=False)

    def search(self, query: str, max: Optional[int] = None, dump: Optional[Path] = None) -> Answer[List[SearchRow]]:
        """Up to max videos for query; dump writes the scrubbed first search page."""
        count = ['--max', str(max)] if max is not None else []
        return answer_from(self.raw(['--search', query, *count, *self._dump(dump)]), parse_search, partial=True)

    def playlist(self, target: str, max: Optional[int] = None, dump: Optional[Path] = None) -> Answer[Listing]:
        """The first max videos of a playlist; dump writes the scrubbed first browse response."""
        count = ['--max', str(max)] if max is not None else []
        return answer_from(self.raw(['--playlist', target, *count, *self._dump(dump)]), parse_playlist, partial=True)


# --- yt-dlp ----------------------------------------------------------------

@dataclass(frozen=True)
class YtVideo:
    """yt-dlp -f bestaudio's title, page url and format id (251-drc is itag 251 too)."""
    title: str
    page_url: str
    format_id: str

    @property
    def itag(self) -> str:
        return self.format_id.split('-')[0]


# The titles YouTube gives the videos it lists but nobody can play. yt-dlp
# asks for them (browse params wgYCCAA=); the library does not, and drops any
# YouTube lists anyway, so a comparison leaves them out of yt-dlp's side.
UNAVAILABLE_TITLES = ('[Private video]', '[Deleted video]')


@dataclass(frozen=True)
class YtPlaylist:
    """yt-dlp --flat-playlist's title and count of a playlist, and the ids, titles and durations it listed."""
    title: str
    count: Optional[int]
    ids: Tuple[str, ...]
    entry_titles: Tuple[str, ...]
    durations: Tuple[Optional[float], ...]  # seconds; None where yt-dlp does not know (NA)

    @property
    def playable_ids(self) -> List[str]:
        """The ids, without the videos YouTube lists as private or deleted."""
        return [video_id for video_id, title in zip(self.ids, self.entry_titles)
                if title.strip() not in UNAVAILABLE_TITLES + ('', 'NA')]


def parse_ytdlp_failure(stderr: str) -> Failure:
    """yt-dlp's last ERROR line, or its last line."""
    lines = [line.strip() for line in lines_of(stderr) if line.strip()]
    errors = [line for line in lines if line.startswith('ERROR:')]
    if errors:
        return Failure('yt-dlp', errors[-1][len('ERROR:'):].strip())
    return Failure('yt-dlp', lines[-1] if lines else 'no output')


def parse_ytdlp_video(stdout: str) -> YtVideo:
    lines = lines_of(stdout)
    if len(lines) != 3:
        raise OutputError(f'yt-dlp: expected three lines (title, page url, format id), got {len(lines)}')
    return YtVideo(*lines)


def parse_ytdlp_search(stdout: str) -> List[str]:
    """The video ids among the ids yt-dlp printed for a search.

    yt-dlp lists the channels a search finds as well (a UC... id of 24
    characters for an artist's name); ytres_cli skips them, so they are no
    part of the comparison.
    """
    return [line for line in lines_of(stdout) if is_video_id(line)]


# What YtDlp.playlist() has yt-dlp print per entry, a tab between fields; the
# entry's title last, so that a tab inside it stays in it.
YTDLP_PLAYLIST_LINE = '%(id)s\t%(playlist_count)s\t%(duration)s\t%(playlist_title)s\t%(title)s'


def _seconds_or_none(text: str) -> Optional[float]:
    """A duration yt-dlp printed (213, or 213.0), or None for NA."""
    try:
        return float(text)
    except ValueError:
        return None


def parse_ytdlp_playlist(stdout: str) -> YtPlaylist:
    """Lines of YTDLP_PLAYLIST_LINE, one per entry; an empty playlist has none."""
    ids = []
    entry_titles = []
    durations = []
    title = ''
    count: Optional[int] = None
    for number, line in enumerate(lines_of(stdout), 1):
        fields = line.split('\t', 4)
        if len(fields) != 5 or not is_video_id(fields[0]):
            raise OutputError(f'yt-dlp: line {number} is not "<id> <count> <duration> <playlist title> <title>"')
        ids.append(fields[0])
        durations.append(_seconds_or_none(fields[2]))
        entry_titles.append(fields[4])
        title = fields[3]
        count = int(fields[1]) if fields[1].isdigit() else None  # "NA" when yt-dlp does not know
    return YtPlaylist(title, count, tuple(ids), tuple(entry_titles), tuple(durations))


class YtDlp:
    """yt-dlp as Python functions."""

    # No user configuration (it could add options that change the output),
    # no warnings among the errors, and UTF-8 whatever the console's code page.
    COMMON = ('--ignore-config', '--no-warnings', '--encoding', 'utf-8')

    def __init__(self, exe: Path, launch: Launcher) -> None:
        self.exe = exe
        self.launch = launch

    def _run(self, arguments: Sequence[str], parse: Callable[[str], T]) -> Answer[T]:
        completed = self.launch('yt-dlp', [str(self.exe), *self.COMMON, *arguments], RUN_TIMEOUT_SECONDS)
        if completed.exit_code is None:
            return Answer(None, Failure('NoAnswer', f'no answer in {completed.seconds:.0f} s'), completed)
        if completed.exit_code != 0:
            return Answer(None, parse_ytdlp_failure(completed.stderr), completed)
        try:
            return Answer(parse(completed.stdout), None, completed)
        except OutputError as error:
            return Answer(None, Failure('Unreadable', str(error)), completed)

    def best_audio(self, video_id: str) -> Answer[YtVideo]:
        """-f bestaudio's title, page url and format id. The stream url is not asked for: nothing needs it."""
        return self._run(['--no-playlist', '-f', 'bestaudio', '--print', 'title', '--print', 'webpage_url',
                          '--print', 'format_id', 'https://www.youtube.com/watch?v=' + video_id], parse_ytdlp_video)

    def search_ids(self, query: str, count: int) -> Answer[List[str]]:
        """The video ids among ytsearch<count>:<query>'s results, best match first."""
        return self._run(['--flat-playlist', '--print', 'id', f'ytsearch{count}:{query}'], parse_ytdlp_search)

    def playlist(self, url: str, count: int) -> Answer[YtPlaylist]:
        """The first count entries of a playlist, with its title and count."""
        return self._run(['--flat-playlist', '--playlist-items', f':{count}', '--print', YTDLP_PLAYLIST_LINE, url],
                         parse_ytdlp_playlist)


# --- Dumps -----------------------------------------------------------------
#
# What ytres_cli --dump promises (README "Running"): the response, with the
# requesting address replaced by the placeholder and visitor data by FIXTURE.
# These read a dump's text and say what it still holds. They return what
# they find so that a test can count it; a test never prints the values.

PLACEHOLDER_VISITOR_DATA = 'FIXTURE'

# ip=<address> in a query (& as itself, & or %26; = as itself or %3D),
# and /ip/<address>/ in a path (/ as itself or %2F): the forms the scrubber
# rewrites, each taking the address that follows.
_DUMP_ADDRESS = re.compile(
    r'(?:(?:[?&]|\\u0026|%26|%3[Ff])ip(?:=|%3[Dd])|(?:/|%2[Ff])ip(?:/|%2[Ff]))((?:[0-9A-Fa-f.:]|%3[Aa]|%253[Aa])+)')


def foreign_ipv4_in(text: str) -> List[str]:
    """Every dotted-quad address in text but the placeholder."""
    return [address for address in ipv4_addresses_in(text) if address != PLACEHOLDER_ADDRESS]


def foreign_ipv6_in(text: str) -> List[str]:
    """Every IPv6-like address in text, wherever it stands (the placeholder is IPv4, so none is allowed).

    Four or more colon-separated hex groups: a length such as 1:02:03 is no match.
    """
    return _IPV6.findall(text)


def foreign_address_parameters_in(text: str) -> List[str]:
    """Every address after an ip= or /ip/ in text but the placeholder: IPv6 ones too."""
    return [match.group(1) for match in _DUMP_ADDRESS.finditer(text) if match.group(1) != PLACEHOLDER_ADDRESS]


def visitor_data_in(node: Any) -> List[str]:
    """Every visitor data value in a parsed response, under either name it travels by.

    "visitorData": <value>, and the web client's tracking parameter
    {"key": "visitor_data", "value": <value>}.
    """
    found: List[str] = []
    if isinstance(node, dict):
        if node.get('key') == 'visitor_data' and 'value' in node:
            found.append(str(node['value']))
        for key, value in node.items():
            if key == 'visitorData':
                found.append(str(value))
            found.extend(visitor_data_in(value))
    elif isinstance(node, list):
        for value in node:
            found.extend(visitor_data_in(value))
    return found


def longest_string_in(node: Any) -> int:
    """The length of the longest string value in a parsed response."""
    if isinstance(node, dict):
        return max((longest_string_in(value) for value in node.values()), default=0)
    if isinstance(node, list):
        return max((longest_string_in(value) for value in node), default=0)
    return len(node) if isinstance(node, str) else 0


# --- The corpus files ------------------------------------------------------
#
# One entry per line, in tests/corpus.txt's format: fields separated by
# spaces, then a # and what the line is for. Blank lines and lines that are
# only a comment are skipped.

class CorpusError(ValueError):
    """A corpus line that cannot be read; the message names the file and the line."""


@dataclass(frozen=True)
class CorpusLine:
    body: str     # what comes before the #, stripped
    purpose: str  # what comes after it, stripped
    where: str    # "<file>:<line>", for messages


def read_corpus_lines(path: Path) -> List[CorpusLine]:
    lines = []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        body, _, purpose = line.partition('#')
        if body.strip():
            lines.append(CorpusLine(body.strip(), purpose.strip(), f'{path.name}:{number}'))
    return lines


@dataclass(frozen=True)
class VideoCase:
    video_id: str
    expected: str  # the Error ytres_cli should report; Ok for one that plays
    purpose: str


def read_video_corpus(path: Path = VIDEO_CORPUS) -> List[VideoCase]:
    cases = []
    for line in read_corpus_lines(path):
        fields = line.body.split()
        if len(fields) != 2:
            raise CorpusError(f'{line.where}: expected an id and an Error, got {line.body!r}')
        video_id, expected = fields
        if not is_video_id(video_id) or expected not in ERROR_NAMES:
            raise CorpusError(f'{line.where}: {video_id!r} is no video id or {expected!r} no Error')
        cases.append(VideoCase(video_id, expected, line.purpose))
    return cases


# What a search line checks: see corpus_search.txt.
SEARCH_KINDS = ('common', 'first', 'letters', 'nothing')


@dataclass(frozen=True)
class SearchCase:
    kind: str
    expect: str  # "first": the video id; "letters": text the first result must hold; else "-"
    query: str
    purpose: str


def read_search_corpus(path: Path = SEARCH_CORPUS) -> List[SearchCase]:
    cases = []
    for line in read_corpus_lines(path):
        fields = line.body.split(None, 2)
        if len(fields) != 3:
            raise CorpusError(f'{line.where}: expected a kind, an expectation and a query, got {line.body!r}')
        kind, expect, query = fields
        if kind not in SEARCH_KINDS:
            raise CorpusError(f'{line.where}: unknown kind {kind!r}; one of {", ".join(SEARCH_KINDS)}')
        if kind == 'first' and not is_video_id(expect):
            raise CorpusError(f'{line.where}: a "first" line expects a video id, got {expect!r}')
        if kind in ('common', 'nothing') and expect != '-':
            raise CorpusError(f'{line.where}: a {kind!r} line expects nothing; write -')
        cases.append(SearchCase(kind, expect, query, line.purpose))
    return cases


@dataclass(frozen=True)
class CountRule:
    """What a playlist's count must be: exactly, at least, or anything ("-")."""
    value: Optional[int]
    at_least: bool

    @staticmethod
    def parse(text: str) -> 'CountRule':
        if text == '-':
            return CountRule(None, False)
        digits = text[:-1] if text.endswith('+') else text
        if not digits.isdigit():
            raise ValueError(f'a count is N, N+ or -, got {text!r}')
        return CountRule(int(digits), text.endswith('+'))

    def holds(self, count: int) -> bool:
        if self.value is None:
            return True
        return count >= self.value if self.at_least else count == self.value

    def __str__(self) -> str:
        return '-' if self.value is None else f'{self.value}{"+" if self.at_least else ""}'


# How a listing is compared with yt-dlp's: see corpus_playlists.txt. "-" for
# a line that lists nothing.
PLAYLIST_COMPARES = ('exact', 'curated', '-')


@dataclass(frozen=True)
class PlaylistCase:
    target: str
    expected: str  # the Error ytres_cli should report
    max: int
    count: CountRule
    compare: str  # one of PLAYLIST_COMPARES
    purpose: str


def read_playlist_corpus(path: Path = PLAYLIST_CORPUS) -> List[PlaylistCase]:
    cases = []
    for line in read_corpus_lines(path):
        fields = line.body.split()
        if len(fields) != 5:
            raise CorpusError(f'{line.where}: expected a target, an Error, a max, a count and a comparison, '
                              f'got {line.body!r}')
        target, expected, max_text, count_text, compare = fields
        if expected not in ERROR_NAMES:
            raise CorpusError(f'{line.where}: {expected!r} is no Error')
        if compare not in PLAYLIST_COMPARES or (compare == '-') != (expected != 'Ok'):
            raise CorpusError(f'{line.where}: an Ok line is compared "exact" or "curated", any other "-"; '
                              f'got {compare!r}')
        if not max_text.isdigit() or int(max_text) == 0:
            raise CorpusError(f'{line.where}: the max must be a positive number, got {max_text!r}')
        try:
            count = CountRule.parse(count_text)
        except ValueError as error:
            raise CorpusError(f'{line.where}: {error}') from None
        cases.append(PlaylistCase(target, expected, int(max_text), count, compare, line.purpose))
    return cases
