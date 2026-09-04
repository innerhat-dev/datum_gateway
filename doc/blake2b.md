# Mining BLAKE2b with the DATUM Gateway

This covers the gateway side of the Bitcoin Knots BLAKE2b proof-of-work
change: which build, what goes in the config, how the headline gets into
your coinbase, and what to watch. The node side (installing the Knots
release, the fork height, what a node that does not upgrade does) is in
the Knots operator notes,
https://github.com/bitcoinknots/bitcoin/discussions/379, and the guide
in bitcoinknots/bitcoin#378.

Checked against this repository at commit 2fea7e5 and Bitcoin Knots
v29.4.1.knots20260508rc3 on testnet4. Mainnet parameters (the fork
height, the starting difficulty) arrive with rc4; the Mainnet section
below is provisional until then, and the rc4 release notes win over
anything here.

## Which gateway

Only this fork (innerhat-dev/datum_gateway, known as
justinfilip/datum_gateway until the account was renamed on 2026-08-29)
produces BLAKE2b work. The
OCEAN DATUM Gateway release does not, and there is no BLAKE2b pool yet,
so this is solo mining: the whole reward of any block you find goes to
`mining.pool_address`.

Build from `master`. The banner printed at startup shows the git commit;
`--version` shows only the protocol version. Keep the binary apart from
a production SHA256d gateway (a different name under `/usr/local/bin`,
or leave it in the build directory) and do not `make install` over one.

## Node

- Bitcoin Knots with the fork: rc3 for testnet4, rc4 for mainnet once
  it is released.
- `blake2b_headline=<string>` in bitcoin.conf. The node refuses to start
  without it ("This version requires blake2b_headline set manually").
  Until the real headline is published use a placeholder; the value only
  matters for the fork block itself.
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
		"coinbase_tag_secondary": "PUT THE HEADLINE HERE",
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

The fork block, and only the fork block, must carry a news headline in
its coinbase. The node checks it with a byte search of the coinbase
scriptSig (`validation.cpp`, reject reason `bad-headline`): the exact
bytes have to appear in it, contiguous, and nothing about their position
matters.

The gateway does not insert the headline for you. `getblocktemplate`
publishes it (`coinbaseaux.blake2b_headline`, hex) but the gateway only
reads that key to recognize a BLAKE2b template. The way to get the bytes
into your coinbase is `mining.coinbase_tag_secondary`, which the
coinbaser writes into the scriptSig as-is.

- Copy and paste the published string. Do not retype it. One character
  off and the block you find is invalid to every node.
- ASCII only. Other text around it is fine (the primary tag, for
  instance) since the check is a substring match.
- Length. The config check allows 60 characters per tag and 88 combined,
  but the coinbaser has 84 bytes for both tags together and trims the
  secondary tag to fit without a warning (`datum_coinbaser.c`,
  `MAX_COINBASE_TAG_SPACE`). Keep primary + secondary at 84 or under, or
  the end of the headline is missing from the block and it fails
  `bad-headline`. A short primary tag keeps you clear of it.
- Set it, restart the gateway, and read it back from the status page's
  "Secondary/Miner Tag" row before the node reaches the fork height.
  Once the node hands out the fork-height template, work built with the
  wrong tag is wasted.

After the fork block the tag is ordinary coinbase text again and you can
change it back.

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

When a block is found, `bitcoin-cli getblockheader <hash>` on rc3 shows
`header_version` 2, and `getblock <hash> 2` shows your tags in the
coinbase.

## Test on testnet4 first

testnet4 forked at height 150027 on 2026-08-27 (rc3), fork-block
difficulty 2^24, and has been mined by BLAKE2b hardware since. The same
gateway config works with `chain=testnet4` on the node, the testnet4 RPC
port (48332 by default) in `rpcurl`, and a testnet address in
`pool_address`. rc3 refuses to run mainnet without
`-allow_mainnet_test_only`, so there is no hitting mainnet by accident
with it.

## Mainnet

Provisional, written 2026-08-29 before rc4 was published. This section
is updated when the rc4 release notes are out, and they win over
anything here.

- Release: `v29.4.1.knots20260508rc4` is the first build with mainnet
  parameters. rc3 refuses to run mainnet at all.
- Fork height: set by rc4. The announced plan is the block after the
  BIP110 chain's current tip, 961639, so 961640 unless a SHA256d block
  lands on that chain first. The rc4 notes state the final number.
- Headline: announced separately, after the height is locked, when the
  Sunday New York Post front page publishes (lately between 12:30 and
  1:30 AM EDT). Nothing to mine before it. Run with a placeholder in
  `blake2b_headline` until then; it only matters for the fork block.
- Fork-block difficulty: set by rc4. testnet4 used a constant 2^24.
  Whatever the start, expect the first retarget to lower it, since the
  window spans the slow weeks before the fork.
- SHA256d hardware: off once the height is locked. A SHA256d block at
  or after the fork height is invalid.
- Until the Knots maintainers declare the chain final, treat blocks
  after the fork height as provisional when accepting payments.

Nothing in the gateway config changes between testnet4 and mainnet
except the node's RPC port, the address, and the tag.

## Fork night, in order

1. Node on rc4 with `blake2b_headline=PLACEHOLDER`, started, sitting at
   the height before the fork.
2. Gateway from this repository, config as above, started, with a
   `NEW NETWORK BLOCK` line in the log or a job on the dashboard.
3. Headline published: paste it into `blake2b_headline` and
   `coinbase_tag_secondary`, restart the node, then the gateway. Read
   the tag back from the status page.
4. Point the hardware at the gateway.
5. Watch for the first BLAKE2b template, then for accepted shares.

## If something is wrong

| Symptom | Cause |
|---|---|
| Node will not start: "requires blake2b_headline set manually" | No `blake2b_headline=` in bitcoin.conf |
| Startup log: "DATUM server connection could not be established" | `pooled_mining_only` still `true` with an empty `pool_host`; harmless in solo mode |
| Gateway cannot fetch templates from the height on; node debug.log shows error -8 | `pow_algorithm` set to `sha256d` |
| Block submitted, node says `bad-headline` | Wrong tag bytes, or the tag was trimmed (primary + secondary over 84) |
| BLAKE2b miner gets jobs, every share rejected | Node still below the fork height; the gateway is serving SHA256d work |
| Miner will not connect | `.local` hostname in the miner; use the IP |
