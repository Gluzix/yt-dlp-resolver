"""What needs no network: the suite's own pure functions, then ytres_cli's command line.

Nothing here talks to YouTube. ytres_cli runs behind a dead proxy (see
conftest.py), so a request it should not have made fails on this machine.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import List

import pytest

import harness

# Addresses for redact() to hide, built at run time: the repository holds no
# address but the placeholder 203.0.113.7. Both are from the documentation
# ranges (RFC 5737, RFC 3849), never anyone's.
ADDRESS = '.'.join(['198', '51', '100', '23'])
ADDRESS6 = ':'.join(['2001', 'db8', '85a3', '0', '0', '8a2e', '370', '7334'])
PYTHON = [sys.executable, '-I', '-c']


def stream_url(address: str = ADDRESS, itag: str = '251') -> str:
    return (f'https://rr3---sn-abcdefgh.googlevideo.com/videoplayback?expire=1764000000&ei=x&ip={address}'
            f'&id=o-AB&itag={itag}&source=youtube&c=VISIONOS')


# --- redact ----------------------------------------------------------------

def test_redact_reduces_a_stream_url_to_its_itag() -> None:
    text = harness.redact(f'picked {stream_url()} for you')
    assert text == 'picked <stream url, itag 251> for you'


@pytest.mark.parametrize('address', [ADDRESS, ADDRESS6, ADDRESS6.replace(':', '%3A')])
def test_redact_leaves_no_trace_of_the_address_or_the_host(address: str) -> None:
    text = harness.redact(stream_url(address))
    for trace in (address, 'googlevideo', 'ip='):
        assert trace not in text


def test_redact_hides_a_manifest_url_with_the_address_in_its_path() -> None:
    url = f'https://manifest.googlevideo.com/api/manifest/hls_variant/expire/1764000000/ip/{ADDRESS}/itag/0/file/x.m3u8'
    assert harness.redact(url) == '<stream url>'


def test_redact_hides_an_address_parameter_in_escaped_json() -> None:
    # A url inside JSON text may come with its ampersands as &, which
    # ends the url where the redaction of whole urls stops.
    text = harness.redact('"url":"https://i.ytimg.com/vi/x/hq720.jpg?sqp=-oay\\u0026ip=' + ADDRESS + '\\u0026rs=AOn"')
    assert ADDRESS not in text
    assert '\\u0026ip=<redacted>' in text


def test_redact_hides_bare_addresses_but_the_placeholder() -> None:
    text = harness.redact(f'from {ADDRESS} and {ADDRESS6}, fixtures say 203.0.113.7')
    assert text == 'from <redacted> and <redacted>, fixtures say 203.0.113.7'


@pytest.mark.parametrize('text', [
    'https://www.youtube.com/watch?v=dQw4w9WgXcQ',
    'yt-dlp 2026.08.19 took 2.4 s',
    'a length of 1:02:03 and 10:00:00',
    'Unavailable: The playlist does not exist.',
])
def test_redact_keeps_what_is_no_stream_url_or_address(text: str) -> None:
    assert harness.redact(text) == text


def test_ipv4_addresses_in_finds_the_placeholder_and_others() -> None:
    assert harness.ipv4_addresses_in(f'a {ADDRESS} b 203.0.113.7 c 2026.08.19') == [ADDRESS, '203.0.113.7']


# --- Urls and ids ----------------------------------------------------------

def test_the_parts_of_a_stream_url() -> None:
    url = stream_url(itag='140')
    assert harness.itag_of(url) == '140'
    assert harness.expire_of(url) == 1764000000
    assert harness.host_of(url) == 'rr3---sn-abcdefgh.googlevideo.com'
    assert harness.itag_of('https://www.youtube.com/') is None
    assert harness.expire_of('https://www.youtube.com/?expire=soon') is None


@pytest.mark.parametrize('url, expected', [
    ('https://www.youtube.com/watch?v=dQw4w9WgXcQ', 'dQw4w9WgXcQ'),
    ('https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1', None),
    ('https://youtu.be/dQw4w9WgXcQ', None),
    ('https://www.youtube.com/watch?v=short', None),
])
def test_video_id_of_takes_the_canonical_page_url_only(url: str, expected: str) -> None:
    assert harness.video_id_of(url) == expected


@pytest.mark.parametrize('target, expected', [
    ('PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk', 'PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk'),
    ('https://www.youtube.com/watch?v=v1X9Fqs5a14&list=PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4',
     'PLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4'),
    ('https://www.youtube.com/watch?v=v1X9Fqs5a14', ''),
])
def test_list_id_of(target: str, expected: str) -> None:
    assert harness.list_id_of(target) == expected


# --- Lines and failures ----------------------------------------------------

@pytest.mark.parametrize('text, lines', [
    ('', []),
    ('a', ['a']),
    ('a\nb\n', ['a', 'b']),
    ('a\n\nb\n', ['a', '', 'b']),
    ('one title\n', ['one title']),  # a line separator inside a title does not split it
])
def test_lines_of(text: str, lines: List[str]) -> None:
    assert harness.lines_of(text) == lines


def test_parse_failure_takes_the_last_error_line_after_the_log() -> None:
    stderr = ('No watch page (Could not connect to server), so no new visitor data\n'
              'Note: something\n'
              'Unavailable: The playlist does not exist.\n')
    assert harness.parse_failure(stderr) == harness.Failure('Unavailable', 'The playlist does not exist.')


def test_parse_failure_keeps_a_colon_in_the_message() -> None:
    failure = harness.parse_failure('NoFormats: visionos: no audio: none\n')
    assert failure == harness.Failure('NoFormats', 'visionos: no audio: none')


def test_parse_failure_is_none_without_an_error_line() -> None:
    assert harness.parse_failure('No response to dump\nNote: nothing\n') is None
    assert harness.parse_failure('') is None


def test_a_failure_prints_redacted() -> None:
    assert str(harness.Failure('Http', f'from {ADDRESS}')) == 'Http: from <redacted>'


# --- ytres_cli's output ----------------------------------------------------

def test_parse_video_reads_three_lines() -> None:
    video = harness.parse_video(f'Never Gonna Give You Up\nhttps://www.youtube.com/watch?v=dQw4w9WgXcQ\n{stream_url()}\n')
    assert video.title == 'Never Gonna Give You Up'
    assert video.page_url == 'https://www.youtube.com/watch?v=dQw4w9WgXcQ'
    assert video.itag == '251'


def test_a_video_shows_no_stream_url_in_its_repr() -> None:
    # pytest prints the repr of what a failing assertion touched.
    video = harness.parse_video(f't\nhttps://www.youtube.com/watch?v=dQw4w9WgXcQ\n{stream_url()}\n')
    assert ADDRESS not in repr(video)
    assert 'googlevideo' not in repr(video)


@pytest.mark.parametrize('stdout', ['', 'title\n', 'a\nb\nc\nd\n'])
def test_parse_video_refuses_anything_but_three_lines(stdout: str) -> None:
    with pytest.raises(harness.OutputError):
        harness.parse_video(stdout)


def test_parse_search_reads_each_row() -> None:
    rows = harness.parse_search('https://www.youtube.com/watch?v=dQw4w9WgXcQ\t213\tRick Astley\tNever\tGonna\n'
                                'https://www.youtube.com/watch?v=jfKfPfyJRdk\tlive\tLofi Girl\tradio\n'
                                'https://www.youtube.com/watch?v=aaaaaaaaaaa\tupcoming\tSomeone\tsoon\n')
    assert rows[0] == harness.SearchRow('https://www.youtube.com/watch?v=dQw4w9WgXcQ', '213', 'Rick Astley',
                                        'Never\tGonna')
    assert rows[0].video_id == 'dQw4w9WgXcQ'
    assert [row.length for row in rows] == ['213', 'live', 'upcoming']


def test_parse_search_of_nothing_is_no_rows() -> None:
    assert harness.parse_search('') == []


def test_parse_search_refuses_a_short_row() -> None:
    with pytest.raises(harness.OutputError):
        harness.parse_search('https://www.youtube.com/watch?v=dQw4w9WgXcQ\t213\tRick Astley\n')


def test_parse_playlist_reads_the_title_line_and_the_entries() -> None:
    listing = harness.parse_playlist('Lex\tFridman Podcast\t450\n'
                                     'https://www.youtube.com/watch?v=v1X9Fqs5a14\t9108\tElon\tMusk\n')
    assert listing is not None
    assert (listing.title, listing.count) == ('Lex\tFridman Podcast', 450)
    assert listing.entries == (harness.PlaylistEntry('https://www.youtube.com/watch?v=v1X9Fqs5a14', 9108, 'Elon\tMusk'),)
    assert listing.ids == ['v1X9Fqs5a14']


def test_parse_playlist_of_nothing_is_none() -> None:
    assert harness.parse_playlist('') is None


@pytest.mark.parametrize('stdout', [
    'no count\n',
    'title\tmany\n',
    'title\t7\nhttps://www.youtube.com/watch?v=v1X9Fqs5a14\tlong\ttitle\n',
    'title\t7\nhttps://www.youtube.com/watch?v=v1X9Fqs5a14\t5\n',
])
def test_parse_playlist_refuses_a_broken_line(stdout: str) -> None:
    with pytest.raises(harness.OutputError):
        harness.parse_playlist(stdout)


def completed(exit_code: int, stdout: str = '', stderr: str = '') -> harness.Completed:
    return harness.Completed(('ytres_cli',), exit_code, stdout, stderr, 0.1)


def test_an_answer_of_success() -> None:
    answer = harness.answer_from(completed(0, 'https://www.youtube.com/watch?v=dQw4w9WgXcQ\t1\tc\tt\n'),
                                 harness.parse_search, partial=True)
    assert answer.ok and answer.code == 'Ok' and answer.value is not None and len(answer.value) == 1


def test_an_answer_that_failed_on_a_later_page_keeps_the_rows() -> None:
    answer = harness.answer_from(completed(1, 'https://www.youtube.com/watch?v=dQw4w9WgXcQ\t1\tc\tt\n',
                                           'Http: YouTube answered HTTP 500\n'), harness.parse_search, partial=True)
    assert answer.code == 'Http'
    assert answer.value is not None and len(answer.value) == 1


def test_an_answer_without_an_error_line_says_what_it_has() -> None:
    answer = harness.answer_from(completed(3), harness.parse_video, partial=False)
    assert answer.failure == harness.Failure('Unknown', 'exit code 3')


def test_an_answer_that_breaks_the_contract_is_unreadable() -> None:
    answer = harness.answer_from(completed(0, 'only a title\n'), harness.parse_video, partial=False)
    assert answer.code == 'Unreadable'


def test_an_answer_that_timed_out() -> None:
    answer = harness.answer_from(harness.Completed(('ytres_cli',), None, '', '', 120.0), harness.parse_video,
                                 partial=False)
    assert answer.code == 'NoAnswer'


# --- yt-dlp's output -------------------------------------------------------

def test_parse_ytdlp_video() -> None:
    video = harness.parse_ytdlp_video('Title\nhttps://www.youtube.com/watch?v=dQw4w9WgXcQ\n251-drc\n')
    assert (video.title, video.itag, video.format_id) == ('Title', '251', '251-drc')


def test_parse_ytdlp_search_leaves_out_the_channels() -> None:
    assert harness.parse_ytdlp_search('UCiGm_E4ZwYSHV3bcW1pnSeQ\ndQw4w9WgXcQ\njfKfPfyJRdk\n') == ['dQw4w9WgXcQ',
                                                                                              'jfKfPfyJRdk']


def test_parse_ytdlp_playlist_and_its_playable_ids() -> None:
    playlist = harness.parse_ytdlp_playlist('DxREm3s1scA\t7\tOur favourite\tElon\tMusk\n'
                                            'XW0QZmtbjvs\t7\tOur favourite\t[Private video]\n'
                                            '4dC_nRYIDZU\tNA\tOur favourite\tSam Harris\n')
    assert playlist.title == 'Our favourite'
    assert playlist.count is None  # the last line said NA
    assert playlist.ids == ('DxREm3s1scA', 'XW0QZmtbjvs', '4dC_nRYIDZU')
    assert playlist.entry_titles[0] == 'Elon\tMusk'
    assert playlist.playable_ids == ['DxREm3s1scA', '4dC_nRYIDZU']


def test_parse_ytdlp_playlist_refuses_a_broken_line() -> None:
    with pytest.raises(harness.OutputError):
        harness.parse_ytdlp_playlist('DxREm3s1scA 7 title\n')


def test_parse_ytdlp_failure_takes_the_last_error() -> None:
    stderr = '[youtube] x: Downloading\nERROR: first\nERROR: [youtube] x: Video unavailable\n'
    assert harness.parse_ytdlp_failure(stderr) == harness.Failure('yt-dlp', '[youtube] x: Video unavailable')
    assert harness.parse_ytdlp_failure('') == harness.Failure('yt-dlp', 'no output')


# --- Pacing, the bot check and running a program ---------------------------

class FakeClock:
    """A clock that moves only when slept on or told to."""

    def __init__(self) -> None:
        self.now = 100.0
        self.slept: List[float] = []

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.slept.append(seconds)
        self.now += seconds


def test_the_pacer_keeps_starts_apart() -> None:
    clock = FakeClock()
    pacer = harness.Pacer(1.5, clock=clock, sleep=clock.sleep)
    assert pacer.wait() == 0.0       # the first start waits for nothing
    clock.now += 0.5
    assert pacer.wait() == pytest.approx(1.0)
    clock.now += 2.0
    assert pacer.wait() == 0.0       # long enough since the last
    assert clock.slept == [pytest.approx(1.0)]


@pytest.mark.parametrize('stderr', [
    'BotCheck: Sign in to confirm you’re not a bot\n',
    'LoginRequired: Sign in to confirm you\'re not a bot\n',
    'Http: YouTube answered HTTP 429\n',
    'ERROR: [youtube] dQw4w9WgXcQ: HTTP Error 429: Too Many Requests\n',
    'ERROR: [youtube] dQw4w9WgXcQ: Sign in to confirm you’re not a bot. Use --cookies-from-browser\n',
])
def test_a_bot_check_is_seen(stderr: str) -> None:
    assert harness.bot_check_in(stderr)


@pytest.mark.parametrize('stderr', [
    'AgeRestricted: Sign in to confirm your age\n',
    'Http: YouTube answered HTTP 400\n',
    'Unavailable: The playlist does not exist.\n',
    '',
])
def test_no_bot_check_is_seen_where_there_is_none(stderr: str) -> None:
    assert harness.bot_check_in(stderr) is None


def test_run_reads_utf8_and_the_exit_code() -> None:
    done = harness.run([*PYTHON, 'import sys; sys.stdout.buffer.write("Podsiadło\\r\\n".encode()); sys.exit(1)'])
    assert done.exit_code == 1
    assert done.stdout == 'Podsiadło\n'


def test_run_kills_a_program_that_runs_out_of_time() -> None:
    done = harness.run([*PYTHON, 'import time; time.sleep(60)'], timeout=1.0)
    assert done.exit_code is None
    assert done.seconds < 30


def test_the_launcher_stops_at_the_first_bot_check() -> None:
    launch = harness.Launcher()
    with pytest.raises(harness.BotCheckStop):
        launch('ytres_cli', [*PYTHON, 'import sys; sys.stderr.write("BotCheck: not a bot\\n"); sys.exit(1)'])
    with pytest.raises(harness.BotCheckStop):
        launch('ytres_cli', [*PYTHON, 'pass'])  # never started
    assert launch.starts == {'ytres_cli': 1}
    assert launch.stopped is not None and 'BotCheck' in launch.stopped


# Stand-ins for a ytres_cli that dies or hangs: Python, never ytres_cli
# itself, so that no test here makes a ytres program abort on purpose.
@pytest.mark.parametrize('script, timeout, says', [
    pytest.param('import sys; sys.stderr.write("abort() has been called\\n"); sys.exit(3)', 30.0,
                 'code 3, outside its contract: abort() has been called', id='an abort'),
    pytest.param('import time; time.sleep(60)', 1.0, 'still running', id='a hang'),
])
def test_a_ytres_cli_that_dies_or_hangs_halts_every_launcher(script: str, timeout: float, says: str) -> None:
    brake = harness.Brake()
    live, offline = harness.Launcher(brake=brake), harness.Launcher(brake=brake)
    with pytest.raises(harness.Halt, match=re.escape(says)):
        live('ytres_cli', [*PYTHON, script], timeout)
    for launch in (live, offline):
        with pytest.raises(harness.Halt):
            launch('yt-dlp', [*PYTHON, 'pass'])  # never started
    assert live.starts == {'ytres_cli': 1} and not offline.starts


def test_ytdlp_failing_halts_nothing() -> None:
    launch = harness.Launcher()
    done = launch('yt-dlp', [*PYTHON, 'import sys; sys.exit(2)'])
    assert done.exit_code == 2
    assert launch.stopped is None


# --- Dumps -----------------------------------------------------------------

def test_a_scrubbed_dump_holds_no_foreign_address() -> None:
    text = ('{"url":"https://rr1---sn-x.googlevideo.com/videoplayback?expire=1\\u0026ip=203.0.113.7\\u0026fvip=4",'
            '"hls":"https://manifest.googlevideo.com/api/manifest/hls_variant/ip/203.0.113.7/id/x",'
            '"title":"Live, 23.06.2024"}')
    assert harness.foreign_ipv4_in(text) == []
    assert harness.foreign_address_parameters_in(text) == []


@pytest.mark.parametrize('text', [
    pytest.param('videoplayback?expire=1&ip=' + ADDRESS + '&id=x', id='query'),
    pytest.param('videoplayback?expire=1\\u0026ip=' + ADDRESS6.replace(':', '%3A'), id='escaped query, IPv6'),
    pytest.param('signatureCipher=url%3Dhttps%253A%252F%252Fx%26ip%3D' + ADDRESS6.replace(':', '%253A') + '%26id',
                 id='cipher, encoded twice'),
    pytest.param('hls_variant/expire/1/ip/' + ADDRESS6 + '/id/x', id='manifest path, IPv6'),
    pytest.param('hls_variant%2Fip%2F' + ADDRESS + '%2Fid', id='encoded manifest path'),
])
def test_a_dump_with_the_address_left_in_is_caught(text: str) -> None:
    assert len(harness.foreign_address_parameters_in(text)) == 1


def test_visitor_data_is_found_under_both_names() -> None:
    response = {'responseContext': {'visitorData': 'CgtFAKE', 'serviceTrackingParams': [
        {'params': [{'key': 'visitor_data', 'value': 'CgtOTHER'}, {'key': 'c', 'value': 'WEB'}]}]}}
    assert sorted(harness.visitor_data_in(response)) == ['CgtFAKE', 'CgtOTHER']
    assert harness.visitor_data_in({'visitorData': 'FIXTURE', 'a': [1, 'x']}) == ['FIXTURE']


def test_longest_string_in() -> None:
    assert harness.longest_string_in({'a': ['xx', {'b': 'x' * 1000}], 'c': 5}) == 1000
    assert harness.longest_string_in([]) == 0


# --- The report ------------------------------------------------------------

def test_the_report_table_gives_each_row_its_verdict() -> None:
    import conftest
    settings = conftest.Settings(False, Path('ytres_cli'), None, 'none')
    brake = harness.Brake()
    state = conftest.RunState(settings, brake, harness.Launcher(brake=brake), harness.Launcher(brake=brake))
    state.rows[('video', 'dQw4w9WgXcQ')] = conftest.Row('video', 'dQw4w9WgXcQ', 'itag 251', 1.25, 'format 251', 3.0)
    state.rows[('search', 'q')] = conftest.Row('search', 'q', f'Http: from {ADDRESS}')
    state.row_of_test = {'a': [('video', 'dQw4w9WgXcQ')], 'b': [('video', 'dQw4w9WgXcQ'), ('search', 'q')]}
    state.outcomes = {'a': 'passed', 'b': 'failed'}
    lines = conftest.report_lines(state)
    assert lines[1].startswith('video') and 'itag 251' in lines[1] and '1.2 s' in lines[1]
    assert lines[1].endswith('FAILED') and lines[2].endswith('FAILED')
    assert ADDRESS not in '\n'.join(lines)
    assert lines[-1].startswith('programs started: live none')


# --- The corpus files ------------------------------------------------------

def write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / 'corpus.txt'
    path.write_text(text, encoding='utf-8')
    return path


def test_the_video_corpus_reads() -> None:
    cases = harness.read_video_corpus()
    assert any(case.expected == 'Ok' for case in cases)
    assert any(case.expected != 'Ok' for case in cases)


def test_read_video_corpus(tmp_path: Path) -> None:
    path = write(tmp_path, '# a comment\n\ndQw4w9WgXcQ  Ok  # an ordinary video\n00000000000 Unavailable\n')
    assert harness.read_video_corpus(path) == [harness.VideoCase('dQw4w9WgXcQ', 'Ok', 'an ordinary video'),
                                               harness.VideoCase('00000000000', 'Unavailable', '')]


@pytest.mark.parametrize('line', ['dQw4w9WgXcQ\n', 'dQw4w9WgXcQ Fine\n', 'short Ok\n', 'dQw4w9WgXcQ Ok extra\n'])
def test_read_video_corpus_names_a_bad_line(tmp_path: Path, line: str) -> None:
    with pytest.raises(harness.CorpusError, match='corpus.txt:2'):
        harness.read_video_corpus(write(tmp_path, '# header\n' + line))


def test_read_search_corpus_keeps_the_query_whole(tmp_path: Path) -> None:
    path = write(tmp_path, 'letters  Podsiadło   Dawid Podsiadło  # Polish\n')
    assert harness.read_search_corpus(path) == [harness.SearchCase('letters', 'Podsiadło', 'Dawid Podsiadło',
                                                                   'Polish')]


@pytest.mark.parametrize('line', ['common - \n', 'sometimes - query\n', 'first notanid query\n', 'common x query\n'])
def test_read_search_corpus_names_a_bad_line(tmp_path: Path, line: str) -> None:
    with pytest.raises(harness.CorpusError, match='corpus.txt:1'):
        harness.read_search_corpus(write(tmp_path, line))


def test_read_playlist_corpus(tmp_path: Path) -> None:
    path = write(tmp_path, 'https://www.youtube.com/watch?v=v1X9Fqs5a14&list=PLx  Ok  100  150+  # a link\n'
                           'PLmissing  Unavailable  5  -\n')
    first, second = harness.read_playlist_corpus(path)
    assert (first.target, first.expected, first.max, str(first.count)) == (
        'https://www.youtube.com/watch?v=v1X9Fqs5a14&list=PLx', 'Ok', 100, '150+')
    assert (second.expected, second.max, second.count.value) == ('Unavailable', 5, None)


@pytest.mark.parametrize('line', ['PLx Ok 5\n', 'PLx Fine 5 7\n', 'PLx Ok 0 7\n', 'PLx Ok 5 seven\n'])
def test_read_playlist_corpus_names_a_bad_line(tmp_path: Path, line: str) -> None:
    with pytest.raises(harness.CorpusError, match='corpus.txt:1'):
        harness.read_playlist_corpus(write(tmp_path, line))


@pytest.mark.parametrize('rule, count, holds', [
    ('7', 7, True), ('7', 8, False), ('150+', 450, True), ('150+', 149, False), ('-', 0, True)])
def test_count_rules(rule: str, count: int, holds: bool) -> None:
    assert harness.CountRule.parse(rule).holds(count) is holds


# --- ytres_cli's command line ----------------------------------------------
#
# Every answer below comes back before ytres_cli makes a request.

USAGE_FIRST_LINE = 'usage: ytres_cli <url-or-id>'


@pytest.mark.parametrize('arguments', [
    pytest.param([], id='no arguments'),
    pytest.param(['--bogus', 'x'], id='an unknown option'),
    pytest.param(['--search', 'a', '--search', 'b'], id='a repeated option'),
    pytest.param(['dQw4w9WgXcQ', '--search', 'a'], id='two modes at once'),
    pytest.param(['--search', 'a', '--playlist', 'PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk'], id='search and playlist'),
    pytest.param(['--search', 'a', '--max'], id='--max with no value'),
    pytest.param(['--search', 'a', '--max', 'five'], id='--max not a number'),
    pytest.param(['--search', 'a', '--max', '-3'], id='--max negative'),
    pytest.param(['dQw4w9WgXcQ', '--max', '5'], id='--max with a resolve'),
    pytest.param(['--search', 'a', '--formats'], id='--formats with a search'),
])
def test_a_bad_command_line_gets_the_usage(offline_ytres: harness.Ytres, arguments: List[str]) -> None:
    done = offline_ytres.raw(arguments)
    assert done.exit_code == 1
    assert done.stdout == ''
    assert done.stderr.startswith(USAGE_FIRST_LINE), done.stderr


@pytest.mark.parametrize('arguments, message', [
    pytest.param(['notalink'], 'Not a YouTube video link or id', id='not a YouTube link'),
    pytest.param(['https://example.com/watch?v=dQw4w9WgXcQ'], 'Not a YouTube video link or id', id='another site'),
    pytest.param(['--search', ''], 'query', id='an empty search'),
    pytest.param(['--search', 'x', '--max', '0'], 'at least one video', id='a search for no videos'),
    pytest.param(['--playlist', 'RDdQw4w9WgXcQ'], 'only public playlists', id='a mix'),
    pytest.param(['--playlist', 'PLC2bGavj05vj1BDIQbhCDiXywCU4_P9Pk', '--max', '0'], 'at least one video',
                 id='a playlist for no videos'),
])
def test_bad_input_is_refused_before_any_request(offline_ytres: harness.Ytres, arguments: List[str],
                                                 message: str) -> None:
    done = offline_ytres.raw(arguments)
    assert done.exit_code == 1
    assert done.stdout == ''
    failure = harness.parse_failure(done.stderr)
    assert failure is not None and failure.code == 'BadInput', done.stderr
    # "<Error>: <message>" is the last line: nothing comes after it.
    assert harness.lines_of(done.stderr)[-1] == f'BadInput: {failure.message}'
    assert message.lower() in failure.message.lower()


def test_a_dump_of_bad_input_says_there_is_nothing_to_dump(offline_ytres: harness.Ytres, tmp_path: Path) -> None:
    dump = tmp_path / 'dump.json'
    answer = offline_ytres.resolve('notalink', dump=dump)
    assert answer.code == 'BadInput'
    assert 'No response to dump' in answer.completed.stderr
    assert not dump.exists()


def test_the_network_trap_catches_a_request(offline_ytres: harness.Ytres) -> None:
    """The proof that the offline tests send nothing: a request ytres_cli does make fails here.

    The dead proxy is on this machine, so this run reaches no further than
    the loopback interface either.
    """
    answer = offline_ytres.resolve('dQw4w9WgXcQ')
    assert answer.code == 'Network', str(answer.failure)
