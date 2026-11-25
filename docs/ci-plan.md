# Build check on GitHub: plan

Implementation plan for whoever codes it. Everything needed is in this file
and `README.md` ("Building", "Black-box tests"). Branch: `ci/build` off
`master`. Kamil merges through pull requests; nothing is pushed by anyone
but him.

## What this is

Today the library is only ever compiled and tested on Kamil's PC. This adds
a GitHub Actions workflow that, for every pull request and every push to
`master`, builds everything from a clean checkout and runs the tests that
need no network. It answers one question before a merge: does this still
build and pass, on a machine that is not Kamil's?

It is a second workflow beside `.github/workflows/claude-review.yml`, which
it must not touch: that file has to stay identical to the copy on `master`
for the review action to run.

## What it never does

It never talks to YouTube. No `ytres_live_tests`, no `pytest --live`, no
`ytres_cli` against a real video. Requests from GitHub's servers come from a
cloud address, would likely meet a bot check, and would say nothing about
what the bot sees. The offline tests are built to need no network; that is
what makes them safe here.

It uses no secret and needs none.

## The workflow: `.github/workflows/build.yml`

- **Name:** `Build and test`.
- **When:** `pull_request` (its default types), `push` to `master`, and
  `workflow_dispatch` so it can be started by hand.
- **Concurrency:** one run per pull request or branch; a newer push cancels
  the older run.
- **Permissions:** `contents: read`, nothing else. It is triggered by
  `pull_request`, never `pull_request_target`.
- **Runner:** a Windows image that has Visual Studio 2022, named explicitly
  rather than `windows-latest`, because the configure step names the
  "Visual Studio 17 2022" generator. Check which images exist today and what
  each ships before choosing.
- **Time limit:** 45 minutes for the job.
- **Actions:** only GitHub's own (`actions/checkout`, `actions/cache`,
  `actions/setup-python`), at their current major versions; check what those
  are rather than assuming.

Steps, in order:

1. Check out the repository.
2. Restore vcpkg's binary cache. The dependencies (`curl` with Schannel,
   `nlohmann-json`, `doctest`) come from the `vcpkg.json` manifest and its
   pinned baseline; building curl from source takes several minutes, so the
   built packages are cached between runs with `actions/cache`, keyed on the
   runner image, the triplet and the hash of `vcpkg.json`. Point vcpkg at the
   cache directory with `VCPKG_DEFAULT_BINARY_CACHE` and create the directory
   first (vcpkg refuses a path that does not exist).
3. Configure, as the README does, with the vcpkg the runner image already
   has (`VCPKG_INSTALLATION_ROOT`), the `x64-windows` triplet, and warnings
   as errors for this build only: `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON`. The
   project's rule is zero warnings; here a warning fails the run. Nothing in
   `CMakeLists.txt` changes for it.
4. Build Debug. Run `ctest -C Debug --output-on-failure`.
5. Build Release. Run `ctest -C Release --output-on-failure`. Release has
   never been part of the routine checks; it is here because an optimized
   build can warn or fail where a Debug one does not.
6. Set up Python, make a virtual environment, install
   `tests/blackbox/requirements.txt`, and run the offline part of the
   black-box suite against the Debug harness: `pytest tests/blackbox`, with
   no `--live`.

Every step that can fail must fail the job; a failing test prints its
output in the log.

## Proving it before it is pushed

A workflow can only really be proven by running it on GitHub, which needs
Kamil's push. Before that, prove everything that can be proven locally:

- The YAML parses, and every `${{ }}` expression and key is one GitHub
  documents.
- The exact commands of steps 3 to 6 work from a **fresh clone** of the
  branch in a scratch directory, with the same flags: configure, Debug build
  and tests, Release build and tests, the offline black-box run. A Release
  build with warnings as errors has not been done before: if it turns up
  warnings, fix them on this branch, each as its own commit with the reason.
- The warning gate works: a deliberate warning in a scratch copy fails the
  build.

## Documentation

README: a short "Continuous integration" section — what runs on a pull
request, what does not and why, how to start it by hand — and a status badge
at the top. `docs/walkthrough.md`: one paragraph in the build section.

## Out of scope

Running anything live. Linux or Clang builds (worth doing one day for the
portability claim, but a separate piece of work). A static vcpkg triplet
(that belongs to the bot integration). Required-status-check settings on the
repository: those are Kamil's to set in GitHub's settings once the workflow
has run green.
