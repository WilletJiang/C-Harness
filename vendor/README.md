# Vendored dependencies

`yyjson` 0.12.0 is vendored unmodified from https://github.com/ibireme/yyjson/tree/0.12.0.
Its MIT license is in `yyjson/LICENSE`. No dependency is downloaded at build time.

SHA-256:

- `yyjson.h`: `175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3`
- `yyjson.c`: `ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a`
- `LICENSE`: `45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947`

libcurl and SQLite are external system dependencies. Record their runtime versions
when comparing benchmark results. The store uses rollback journaling with FULL
synchronization and a single owner; it does not depend on concurrent WAL writers.

`tomlc17` is vendored unmodified at commit
`aa4ca1bc5fe6382ac631a8d1428077a9a084a64d` from
https://github.com/cktan/tomlc17. Its MIT license is in `tomlc17/LICENSE`.
It parses Codex configuration only after explicit extension discovery.

SHA-256:

- `tomlc17/tomlc17.h`: `29c0909092f4b23c7edce705167064f4a90548db5334b4aa2cd95d7ad801adb9`
- `tomlc17/tomlc17.c`: `7476923b6c9b8f0858b82b63ed6107f57448882983390e70c31327f7d3e2a362`
- `tomlc17/LICENSE`: `f949d0976f85076c96fc5122f0632b5a1b806d8b2badbe968c42c97230e6e79e`

QuickJS 2026-06-04 is vendored from https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz
(archive SHA-256 b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a).
Only its interpreter and dependencies are built into the flow helper. License: quickjs/LICENSE.
Local patch: the M4 opcode matcher packs its high byte with an unsigned shift
to avoid signed-shift undefined behavior, then passes the packed int to code_match.
