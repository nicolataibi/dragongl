# Quality scorecard

The state of the project against sixteen criteria, as of release
2026.11.22.12 (23 November 2026). It is the candidate for the first
stable release (docs/RELEASING.md, "A stable release"); its week of
nights is followed in [issue #28](https://github.com/nicolataibi/dragongl/issues/28).

A section scores 10 only when every part of its criterion has a green
check that anyone can see: a required check of CI, a scheduled workflow
of GitHub Actions, or a command of `tools/quality.sh` to run locally.
Where a check is still missing, the section scores 9 and says what is
missing.

**Score: 160 of 160.**

| Section | Score | How it is checked |
|---|:---:|---|
| Security | 10 | CodeQL (security-and-quality) on every push and every week, no open alert; the accepted path findings are explained in [SECURITY.md](../SECURITY.md), section G. TLS 1.3 only; the client checks the identity of the server (a fingerprint given to it, a certificate a trusted authority signed, or trust on first use with the fingerprint printed). The systemd unit reads the passwords as credentials (`LoadCredential=`), never from the environment, and runs as a dynamic user in a strict sandbox. |
| Rules and balance | 10 | Every effect of a spell and of an item has a test of its own. The balance simulator runs in CI. Every night a bot of every class plays a whole campaign (150,000 rounds, the server stopped and started again every 20,000): the world of seed 1 must give at least `campaign_conquests_min` conquests of [tools/quality_limits.json](../tools/quality_limits.json) (4 of 12 today, a ratchet that only goes up), and a world of the day must keep every invariant. |
| Code quality | 10 | clang-format and clang-tidy clean on every file; `-Werror` with gcc and clang, `-Wshadow` and `-Wconversion` included; cppcheck in CI; no function over 120 lines (`tools/quality.sh metrics`). |
| Interface and HUD | 10 | The HUD scales from 0.5x to 4x (HiDPI aware), keeps a WCAG contrast of 4.5:1, has a reduce-motion mode and rebindable keys ([ACCESSIBILITY.md](../ACCESSIBILITY.md)). |
| Architecture | 10 | 174 files in 6 layers, the includes checked by `tools/quality.sh deps` with no violation; no file over 1,000 lines; no global game state (one `Game` passed explicitly). |
| Network protocol | 10 | The messages are generated from one specification ([protocol/messages.json](../protocol/messages.json), [PROTOCOL.md](../PROTOCOL.md)); shared test vectors for the server and the client; the decoder is fuzzed on every push and every night. |
| Vulkan client | 10 | The checklist of [docs/GPU_TESTING.md](GPU_TESTING.md) completed on an Intel Arc A770: 0 messages of the validation layer in 9,227 frames. CI runs the client with the validation layer on a software renderer. Not tested yet on AMD or NVIDIA hardware. |
| Game content | 10 | Schema, cross references and ASCII text of the game data checked on every push (`tools/check_data.py`); spelling checked on every file. |
| Tests | 10 | 137 integration tests and 11 unit tests, also under ASan/UBSan and TSan; line coverage of the server and shared code 85.5% (CI fails under 81%); the protocol, the save files, the text commands and the item effects are fuzzed with libFuzzer on every push (60 s a target) and longer every night. |
| Performance | 10 | 64 clients play on every push of CI, and for 30 minutes every night: the round p99 must stay under 50 ms and a command be answered within 250 ms p99. The benchmark of a round runs on every push (a crowd of monsters: p50 2.5 ms). The server plays a round every 200 ms. |
| Documentation | 10 | A man page for every command installed; the command reference generated from the server ([docs/COMMANDS.md](COMMANDS.md)); the links of every document checked every night. |
| Server loop and network | 10 | Every week the real server, built with ASan/UBSan, plays a character of every class for four hours, stopped and started again six times on the same world; then `tools/analyze_log.py --strict` reads its whole log. |
| Persistence | 10 | Every reader of the state directory is fuzzed. The campaign crashes the server three times and checks that a clean restart changes nothing; every 1,000 rounds it checks that every floor keeps its creatures and the table of the NPCs keeps room. |
| Build system | 10 | The build and the release tarball are checked to be reproducible in CI. |
| Packaging | 10 | The RPMs are built and checked with rpmlint in CI; the COPR builds are green on Fedora 44, 45 and rawhide (x86_64 and i386) and EPEL 10. How the spec meets the Fedora packaging guidelines, point by point: [docs/PACKAGING.md](PACKAGING.md). |
| Continuous integration | 10 | `main` is protected by 17 required checks, with no bypass. Every night: load, fuzzing, links and two campaigns; every week: the soak. |

## Checking it yourself

```bash
tools/quality.sh all                 # format, tidy, data, links, man pages, coverage,
                                     # ThreadSanitizer, reproducibility, fuzzing
tools/quality.sh load bench          # 64 clients, the benchmark of a round
tools/quality.sh campaign            # the campaign of reference (about 10 minutes)
tools/quality.sh campaign 150000 20261123   # a world of another seed
tools/playtest.py --seconds 600      # the real server, a character of every class
```

The campaign is deterministic: the same seed and rounds give the same
report and the same server log on any machine, so its numbers above can
be compared line for line with a local run.

## Known limits

- The Vulkan client has been tested on Intel hardware only.
- No build on an architecture other than x86 yet (docs/PACKAGING.md,
  "Known gaps").
- In the campaign of reference four classes of twelve conquer the
  dungeon. Of the others, some are still going down when the 150,000
  rounds end; the casters stop at the rooms of the last tier of monsters,
  which a single bot does not draw out one by one (DEVELOPMENT.md).
