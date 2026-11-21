# Security policy

## Supported versions

Only the latest release of quarry receives fixes. quarry is at version 0.1 and has no long-term support
line.

| Version | Supported |
| --- | --- |
| 0.1.x | yes |

## Reporting a vulnerability

**Please do not put the details of a security problem in a public issue.**

Use GitHub's private vulnerability reporting: open the repository's **Security** tab, choose **Report a
vulnerability**, and fill in the form. The report is visible only to you and the maintainer,
[@jason-te-sde](https://github.com/jason-te-sde), and the report and its fix are discussed there. This is
the only channel: there is no email address for reports.

Please include:

- what you found and what it lets an attacker do;
- the version or commit, the compiler and the operating system;
- the smallest input that shows it: the SQL text, and the Parquet file or a description of its shape. A
  fuzzer's crash file is ideal: the targets `fuzz_parser` and `fuzz_query` that the `fuzz` preset builds
  (`build/fuzz/fuzz/fuzz_parser`, `build/fuzz/fuzz/fuzz_query`) reproduce it when the file is their only
  argument.

I aim to acknowledge a report within a week, to say whether I accept it as a vulnerability within two
weeks, and to publish a fix and an advisory together. This is a one-person project, and those are goals,
not guarantees.

## What counts

quarry is an embedded library and a command-line tool with no network surface. It reads SQL text and
Parquet files, and a process that runs it on text or files an attacker chose is the threat. These are in
scope:

- a crash, an out-of-bounds read or write, a use after free, an integer overflow that leads to either,
  an unbounded recursion or any other undefined behaviour, on any SQL text. The parser is fuzzed for
  this (`fuzz/fuzz_parser.cc`) and nesting is limited ([docs/sql.md](docs/sql.md));
- the same on a query over a Parquet file that Arrow's reader accepts, where the fault is in quarry's own
  code: the handling of the footer's statistics for row-group pruning, the conversion of decoded
  columns, the kernels, the hash tables, the spill files. A flaw that is in Arrow's or Parquet's own
  reader should be reported to that project; tell me too if quarry's use of it makes it reachable;
- a query whose memory use grows without bound relative to its input and to `--memory-limit`, as
  [docs/operations.md](docs/operations.md) documents that limit: it bounds the operators' state, not the
  Parquet reader's buffers, and a sort that does not fit stops with an error.

These are **out of scope**:

- a wrong query result. That is a bug and welcome as an issue, with the query and the files, but it is
  not a vulnerability: the comparison with DuckDB, the differential test and its known differences are
  in [docs/testing.md](docs/testing.md).
- anything that needs write access to the directory the spill files are in, or to the Parquet files
  being queried, and intends only what that access already allows. quarry does not authenticate or
  encrypt its spill files, and does not protect against another process that can write there.
- a denial of service by a legitimate workload: a large join takes a long time, and a query can be
  ended with `--timeout` or Ctrl-C.
- a flaw in a dependency used only for tests or measurements (GoogleTest, DuckDB, the Python tools):
  report it upstream.

## What happens to a fix

A confirmed vulnerability is fixed on a private branch, released as a patch version (see
[RELEASING.md](RELEASING.md)), and described in a GitHub security advisory and in
[CHANGELOG.md](CHANGELOG.md) once users have a fixed version. The report is credited unless you ask not
to be.
