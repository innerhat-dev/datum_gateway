# Share logging and node-side share validation

These options are for solo miners who need to see what happens to each
share, and to confirm the local node would accept the constructed block
even when the share is below block difficulty.

## Node check order

`submitblock` runs `CheckBlockHeader` / `CheckProofOfWork` first. A share
that is valid work but below the block target is rejected as `high-hash`
and later checks (merkle, coinbase, witness, Blake2b headline) are skipped.

To validate *everything except proof-of-work*, use BIP22 GBT proposal mode:

```
getblocktemplate {"mode":"proposal","data":"<block hex>"}
```

That calls `TestBlockValidity(..., check_pow=false)`.

- `null` — structurally valid (would be a block if PoW met nBits)
- a string — reject reason (`bad-txnmrklroot`, `stale-prevblk`, …)
- HTTP/transport failure — node did not return JSON

## Config

```json
"logger": {
  "log_level_console": 2,
  "log_shares": true,
  "debug_blake2b_pow": false
},
"mining": {
  "blake2b_force_version_high_bit": true,
  "dump_submitblock_path": "",
  "validate_shares_on_node": true,
  "share_node_check": "proposal",
  "share_node_check_every": 16,
  "share_node_check_missingzeros": -1
}
```

| key | default | meaning |
|---|---|---|
| `logger.log_shares` | false | INFO line per accepted/rejected share that passes the missingzeros gate + `missingzeros` (leading bits short of the block target; `0` is a block candidate) |
| `logger.debug_blake2b_pow` | false | INFO dump of BLAKE2b H1 (119 bytes) and final LE hash |
| `mining.blake2b_force_version_high_bit` | true | OR `0x80000000` onto header/H1 version. Leave **true** unless `job` version already has the v2 bit *and* GBT did not strip it (the common GBT path clears `0x80000000` from `version` before serialization). |
| `mining.dump_submitblock_path` | `""` | If set, write each submitblock JSON to this file (detached thread) |
| `mining.validate_shares_on_node` | false | Ask the node about sampled accepted shares |
| `mining.share_node_check` | `"proposal"` | `"proposal"` skips PoW; `"submitblock"` checks PoW first |
| `mining.share_node_check_every` | 16 | Only 1 of every N accepted shares is *considered*. At most one node-check RPC is in flight at a time; others are skipped. Blocks are always submitted separately. |
| `mining.share_node_check_missingzeros` | -1 | `-1` = infinity (log every share; use `share_node_check_every`). `>= 0` overrides that sampler and the share log: only print SHARE / node-check lines when `missingzeros <=` this value. `2` ≈ 4 per block, `4` ≈ 16. |

Recommended solo-debug set:

```json
"logger": { "log_level_console": 2, "log_shares": true },
"mining": {
  "blake2b_force_version_high_bit": true,
  "dump_submitblock_path": "/tmp/datum_last_submitblock.json",
  "validate_shares_on_node": true,
  "share_node_check": "proposal",
  "share_node_check_missingzeros": 2
}
```

Then look for:

```
SHARE accepted user=... host=... reason=ok diff=... missingzeros=2
SHARE accepted user=... host=... reason=block diff=... missingzeros=0
SHARE node-check user=... mode=proposal diff=... => null (structurally valid; PoW not required for proposal)
```

If proposal returns anything other than `null`, that string is the real
template/header/coinbase mismatch — not “the miner is too weak.”
