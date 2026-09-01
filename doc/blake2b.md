# Mining BLAKE2b with the DATUM Gateway

This covers the gateway side of the Bitcoin Knots BLAKE2b proof-of-work
change: which build, what goes in the config, and what to watch. The node side (installing the Knots
release, the fork height, what a node that does not upgrade does) is in
the Knots operator notes,
https://github.com/bitcoinknots/bitcoin/discussions/379, and the guide
in bitcoinknots/bitcoin#378.

Checked against this repository at commit 2fea7e5 and Bitcoin Knots
v29.4.1.knots20260508rc4, on testnet4 and on mainnet across the fork
itself. Mainnet activated at height 961640 on 2026-08-30, so this now
describes joining a running chain rather than preparing for the fork.
rc5 (tagged 2026-08-31) changes only node configuration — the headline
is built in — and is noted where it matters.

## Which gateway

Only this fork (innerhat-dev/datum_gateway, known as
justinfilip/datum_gateway until the account was renamed on 2026-08-29)
produces BLAKE2b work. The
OCEAN DATUM Gateway release does not. Pooled BLAKE2b mining exists
(OCEAN has mined blocks on this chain) but no public endpoint is
documented, so this guide covers solo mining: the whole reward of any
block you find goes to `mining.pool_address`.

Build from `master`. The banner printed at startup shows the git commit;
`--version` shows only the protocol version. Keep the binary apart from
a production SHA256d gateway (a different name under `/usr/local/bin`,
or leave it in the build directory) and do not `make install` over one.

## Node

- Bitcoin Knots v29.4.1.knots20260508rc5, or rc4 (the first build that
  runs mainnet; both use the same fork heights, including testnet4's,
  below).
- Headline configuration depends on the release. rc5 ships
  bitcoinknots/bitcoin#385: the headline is built in, no configuration
  is needed, and a leftover `blake2b_headline=` line in bitcoin.conf
  is ignored on mainnet. rc4 refuses to start without
  `blake2b_headline=8-30 NYPost Deride And Conquer` in bitcoin.conf,
  exactly those bytes, no quotes: the config parser keeps quotation
  marks, and a mismatched value makes the node reject the real fork
  block during sync.
- RPC access for the gateway, `blocknotify=killall -USR1 datum_gateway`
  (or the `/NOTIFY` endpoint), and the `blockmaxsize`/`blockmaxweight`
  reservation from the README. None of that changes for BLAKE2b.

## Gateway config

The example config in `doc/` works with these settings:

```json
{
	"bitcoind": { "rpcuser": "datum", "rpcpassword": "...", "rpcurl": "http://127.0.0.1:8332", "notify_fallback": true },
	"stratum": { "listen_addr": "0.0.0.0", "listen_port": 23334 },
	"mining": {
		"pool_address": "bc1q...",
		"coinbase_tag_primary": "DATUM Gateway",
		"coinbase_tag_secondary": "your tag",
		"pow_algorithm": "auto"
	},
	"api": { "listen_addr": "127.0.0.1", "listen_port": 7152, "admin_password": "...", "modify_conf": false },
	"datum": { "pool_host": "", "pooled_mining_only": false }
}
```

- `mining.pow_algorithm`: leave it at `auto`. The gateway asks for
  `blake2b` in every `getblocktemplate` call and switches to header-v2
  work when the node's template lists `!blake2b` in `rules`, which
  happens at the fork height with nothing to do on your side. Forcing
  `blake2b` on a node still below the height makes blocks the node
  rejects. `sha256d` makes the node refuse templates from the height on
  (RPC error -8, "Support for 'blake2b' rule requires explicit client
  support").
- `mining.allow_hasher_time_rolling`: leave it off (the default). It
  only matters once the node commits a time offset in the header, which
  Knots does not do today.
- `datum.pool_host`: empty string. That is what puts the gateway in solo
  mode; the startup log says `NON-POOLED MINING`.
- `datum.pooled_mining_only`: `false`. The option means "stop serving
  work without a pool connection", which is not what you want here.
  With a blank `pool_host` the default `true` only adds a startup error
  line ("DATUM server connection could not be established") and work
  is still served, but set it anyway so the log is clean.
- `mining.pool_address`: the address that gets paid. Stratum usernames
  are ignored in solo mode (see `doc/usernames.md`).

The gateway does not reload its config. Every change here means a
restart.

## The headline

The fork block, and only the fork block, had to carry a news headline
in its coinbase: the node checks the block at exactly the fork height
for the configured bytes with a substring search of the coinbase
scriptSig (`validation.cpp`, reject reason `bad-headline`). Mainnet's
961640 was mined on 2026-08-30 carrying
`8-30 NYPost Deride And Conquer`, so no block anyone mines now needs
the headline, and `coinbase_tag_secondary` is ordinary coinbase text —
the explorers name your blocks by it.

What survives of the old procedure: a node still validates 961640
against the headline during initial sync — built in on rc5, configured
on rc4 (see "Node" above) — and a regtest rehearsal of a fork height
needs the tag
to carry whatever headline the node is configured with. For a
rehearsal, keep primary plus secondary tag at 84 bytes or under: the
coinbaser trims the secondary tag past that without a warning
(`datum_coinbaser.c`, `MAX_COINBASE_TAG_SPACE`), and a trimmed
headline fails `bad-headline`.

## Miners

BLAKE2b and BLAKE2b-sia ASICs in their native Sia stratum mode, on the
same `stratum.listen_port` SHA256d miners use. Pool URL
`stratum+tcp://<gateway ip>:<port>` (give the ASIC an IP; most firmware
cannot resolve `.local` names), any username, any password. Stock
firmware, no changes.

The hardware people have run end to end, with share and block counts,
is kept in paulscode's compatibility matrix:
https://github.com/paulscode/datum-blake2b-startos#compatibility-matrix
(Goldshell HS-Box, SC5 Pro and SC Box II, Antminer A3, Innosilicon S11
as of 2026-08-29; a GPU running ccminer connects and rejects the work).
Add yours there once it has mined on testnet4.

Do not point BLAKE2b hardware at the gateway until the node is serving
BLAKE2b templates. Before that it gets SHA256d jobs, and none of its
shares will be accepted.

## What you will see

Before the height the gateway serves SHA256d jobs from the same node;
`NEW NETWORK BLOCK` lines in the log prove the RPC path works. At the
height:

- The node's `getblocktemplate '{"rules":["segwit","blake2b"]}'` lists
  `!blake2b` in `rules` and returns a `version` above 2147483648.
- The gateway's next template is header-v2. On master there is no log
  line for the switch, and the dashboard's "Version" row shows the same
  number with bit 31 cleared. A pending change adds a "PoW" row to the
  status page and an INFO line, `Template PoW is BLAKE2b (header v2) at
  height N`.
- A BLAKE2b miner starts getting shares accepted.

When a block is found, `bitcoin-cli getblockheader <hash>` since rc4 shows
`header_version` 2, and `getblock <hash> 2` shows your tags in the
coinbase.

## Test on testnet4 first

rc4 forked testnet4 at height 150308 on 2026-08-30 (rc3 had 150027;
blocks between the two heights are SHA256d again, so an rc3 node needs
rc4 and rewinds on its own at startup). testnet4's
20-minute minimum-difficulty rule applies to BLAKE2b blocks too. The
same gateway config works with `chain=testnet4` on the node, the
testnet4 RPC port (48332 by default) in `rpcurl`, and a testnet address
in `pool_address`.

## Mainnet

The fork activated at height 961640 on 2026-08-30. From the rc4/rc5
source and the live chain:

- Release: `v29.4.1.knots20260508rc5` (rc4, the first build that runs
  mainnet, also works with the headline configured). rc5 checkpoints
  the fork block.
- Fork block: 961640
  (`0000000000000050c1e5f69672f459293be14f46e5a494e7a8c8541396f18eeb`),
  mined 06:14 UTC at difficulty 3.0e7, the 961639 target shifted 22
  bits. Blocks ran a few minutes apart on the first day; the first
  retarget, at 963648, moves difficulty toward ten-minute pacing.
- Headline: covered above. A node wants it in bitcoin.conf; a miner no
  longer needs it anywhere.
- RDTS (BIP110) rules apply from 961640 until September 1, 2027.
- SHA256d hardware: a SHA256d block at or after 961640 is invalid.
- Absolute hashrate here is a small fraction of what SHA256d Bitcoin
  carries, so deep reorgs cost less than you are used to. Give
  payments more confirmations than you would elsewhere.

Nothing in the gateway config changes between testnet4 and mainnet
except the node's RPC port, the address, and the tag.

## Joining now, in order

1. Node on rc5 (no headline configuration; on rc4, with the headline
   line in bitcoin.conf), synced past 961640.
2. Gateway from this repository, config as above, started, with a
   `NEW NETWORK BLOCK` line in the log or a job on the dashboard.
3. Point the hardware at the gateway and watch for accepted shares.

## If something is wrong

| Symptom | Cause |
|---|---|
| Node will not start: "requires blake2b_headline set manually" | rc4 without `blake2b_headline=` in bitcoin.conf; set it, or upgrade to rc5 which drops the requirement |
| Startup log: "DATUM server connection could not be established" | `pooled_mining_only` still `true` with an empty `pool_host`; harmless in solo mode |
| Gateway cannot fetch templates from the height on; node debug.log shows error -8 | `pow_algorithm` set to `sha256d` |
| BLAKE2b miner gets jobs, every share rejected | Node not yet synced past the fork height; the gateway is serving SHA256d work |
| Miner will not connect | `.local` hostname in the miner; use the IP |
