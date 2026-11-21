# Releasing quarry

A release is a tag. The tag is what users build from, so the checks that matter run before it is pushed
and again, by a workflow, on the commit it names.

## Versions

quarry uses [Semantic Versioning](https://semver.org/), with one thing to know.

- **Before 1.0, a minor release may change what quarry accepts or how it behaves**: the SQL subset, the
  `quarry::Engine` API in [include/quarry/engine.h](include/quarry/engine.h), the command line and the
  `EXPLAIN` text. `0.2.0` may not be a drop-in for `0.1.x`. A release that does this says so at the top of
  its changelog entry.

The version is written in one place, `project(quarry VERSION x.y.z ...)` in `CMakeLists.txt`.
`pyproject.toml` describes the Python tooling only, is never published, and carries `0.0.0` on purpose.

## To make a release

1. **Check that the release branch is green**: every job of CI on the commit you mean to release, and
   `scripts/preflight.sh` on your machine if you have one that can run it. The macOS job and the nightly
   differential job run only while the repository is public (they are metered while it is private), so
   for the first release they have not yet run on this code. Go public, then do the three things
   below, and fix what they find before tagging.

   - **Turn on private vulnerability reporting.** [SECURITY.md](SECURITY.md) names it as the only
     channel, and it is off by default on a repository owned by a user. Once the repository is public:

     ```
     gh api -X PUT repos/jason-te-sde/quarry/private-vulnerability-reporting
     gh api repos/jason-te-sde/quarry/private-vulnerability-reporting
     ```

     The second command must print `{"enabled":true}`. Then open the repository's Security tab in a
     browser that is signed out, or signed in as someone else, and check that it offers "Report a
     vulnerability".
   - **Run CI on `main`.** Pushing does nothing now (the commit is already on `main`), and re-running an
     old run reuses its event payload, in which the repository was still private, so the macOS job would
     stay skipped. Start a new run:

     ```
     gh workflow run ci.yml --ref main
     gh run list --workflow ci.yml --branch main --limit 1
     gh run view RUN_ID
     ```

     Confirm that `build-test-macos` ran and passed, including its `nm -C` object check
     (`scripts/check-objects.sh`), and that every other job passed.
   - **Run the nightly workflow once**: `gh workflow run nightly.yml --ref main`. Its `visibility` job
     asks the API whether the repository is public, so the differential job runs only then.

2. **Write the changelog.** In [CHANGELOG.md](CHANGELOG.md) rename `[Unreleased]` to the new version
   with today's date, start a new empty `[Unreleased]`, and fix the compare links at the bottom. List
   what a user of the library, the command line or the SQL needs to know: changes of behaviour first, then
   additions, then fixes. The heading is `## [X.Y.Z] - YYYY-MM-DD`: the section under it, up to the
   next `## ` heading or to the link definitions at the bottom, is the text of the release page, so
   write its links inline. The workflow makes a relative link absolute; it does not carry a
   `[text][name]` reference's definition over.
3. **Set the version.** Edit `VERSION` in `CMakeLists.txt` to match. Commit both changes together with
   the subject `chore(build): release x.y.z`, through a pull request like any other change.
4. **Check the numbers.** If the measurements changed, `bench/render_tables.py` has rendered them and
   `scripts/check-readme-numbers.sh` passes; the README must say what the newest results say.
5. **Tag the merged commit** and push the tag. The tag is `v` and the version, annotated. The first two
   commands ask the questions that the workflow asks, so that a mistake is found here and not on the
   Actions page: the first says whether the tag is the version of `CMakeLists.txt`, and the second prints
   the text of the release page (the workflow also makes its relative links absolute).

   ```
   git switch main && git pull
   scripts/release-check.sh check vX.Y.Z
   scripts/release-check.sh notes X.Y.Z
   git tag -a vX.Y.Z -m "quarry X.Y.Z"
   git push origin vX.Y.Z
   ```

6. **The release workflow** (`.github/workflows/release.yml`) runs on a pushed `v*` tag, in two jobs.
   - `verify` cannot write to the repository. It **refuses** a tag that does not equal `v` plus the
     `VERSION` in `CMakeLists.txt` (step 5's check, which prints both versions), so a tag can never name
     a version the source does not say it is. It also refuses a version that the changelog has no entry
     for. It then builds Release on Ubuntu 24.04 with GCC 14 and runs the tests: `cmake --preset release`,
     `cmake --build --preset release` and `ctest --preset release`. Arrow is restored from the cache of
     the default branch, or built by `scripts/build_arrow.sh`, which takes long enough that the job's
     time limit allows for it.
   - `publish` runs only when `verify` passed, and is the only job with write access to the repository
     (`contents: write`). It checks the tag again and makes `git archive --prefix=quarry-X.Y.Z/` of the
     tagged commit, as `quarry-X.Y.Z.tar.gz`, and its checksum as `quarry-X.Y.Z.tar.gz.sha256`, in the
     format `sha256sum --check` reads. It unpacks the archive and asks the same question of its
     `CMakeLists.txt`, and then creates the GitHub release with both files attached and the changelog
     entry as its notes, its links pointing at the tagged tree.
7. **Look at the release.** Download the source archive and its checksum and check one against the other
   (`sha256sum --check quarry-X.Y.Z.tar.gz.sha256`, or `shasum -a 256 --check` on macOS), then unpack it
   in a clean directory and follow the README from "Try it": build Arrow if it is not built, then
   `cmake --preset release`, `cmake --build --preset release` and the commands that follow. The workflow
   tests the tagged commit and not the unpacked archive, so this is the step that tests the archive.

## If something goes wrong

- **The workflow refused the tag, or `verify` failed**: the tag does not match the version, the changelog
  has no entry for it, or a test failed. Nothing was published, because `publish` waits for `verify`.
  Delete the tag (`git push origin :refs/tags/vX.Y.Z` and `git tag -d vX.Y.Z`), fix the cause through a
  pull request, and tag the new commit.
- **`publish` failed, and nothing in the source is to blame**: a runner or the network. Re-run the failed
  job from the Actions page. If it stopped after it had made the release page, delete that page (not the
  tag) first: `gh release create` refuses to replace a release, and the page that is left holds fewer
  files than the release should.
- **A published release is bad**: do not move or delete its tag, since people may have built from it.
  Release a patch version that fixes it and say so in the changelog. If the problem is a vulnerability,
  see [SECURITY.md](SECURITY.md).
- **A fix for a released version** goes to `main` first and is released from there; there are no release
  branches while only one release is supported.
