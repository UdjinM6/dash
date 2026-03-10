# Merk Range Proofs

## Overview

Dash Core exposes authenticated compact-filter delivery via a single range-proof protocol:

- P2P: `getmerkrange` / `merkrange`
- RPC: `getmerkroot`, `getmerkrange`
- Index: `MerkRangeIndex`

## Components

- `src/index/merkrangeindex.{h,cpp}`: `MerkRangeIndex` (`BaseIndex` subclass)
- `src/net_processing.cpp`: `getmerkrange` request handling
- `src/protocol.{h,cpp}`: `getmerkrange`/`merkrange` message declarations and registration
- `src/init.cpp`: `-merkrangeindex=<type>` configuration and service-bit advertisement
- `src/rpc/blockchain.cpp`: range-proof RPC endpoints
- `test/functional/p2p_merkrange.py`: protocol functional coverage
- `src/test/merkrangeindex_tests.cpp`: unit tests

## P2P Wire Format

### `getmerkrange` (request)

| Field | Type | Description |
|---|---|---|
| `filter_type` | `uint8_t` | Filter type (`BASIC_FILTER = 0`) |
| `start_height` | `uint32_t` | Range start (inclusive) |
| `stop_hash` | `uint256` | End block hash (must be on active chain) |

### `merkrange` (response)

| Field | Type | Description |
|---|---|---|
| `filter_type` | `uint8_t` | Echoed filter type |
| `target_stop_hash` | `uint256` | Echoed request target hash |
| `served_stop_hash` | `uint256` | Block hash at response `end_height` |
| `snapshot_tip_hash` | `uint256` | Merkrange index tip hash for the snapshot used to build `proof` and `root_hash` |
| `root_hash` | `uint256` | Merk root hash |
| `start_height` | `uint32_t` | Echoed start height |
| `end_height` | `uint32_t` | Response end height (may be below requested target end) |
| `proof` | `CompactSize + bytes` | Proof bytes valid for `[start_height, end_height]` |

## Validation Rules

- Service bit required: `NODE_MERKRANGE`
- Protocol version required: `MERKRANGE_P2P_VERSION`
- `start_height <= end_height`
- Max request span: `MAX_GETMERKRANGE_SIZE = 10000`
- Max proof size: `MAX_MERKRANGE_PROOF_BYTES = 2 MiB`
- If `stop_hash` is not on the active chain, request is ignored

## Runtime Behavior

- If the index is still syncing, server may serve a prefix of the requested range up to indexed height (instead of dropping the request).
- Server estimates a candidate `end_height` from per-height filter sizes, then generates a proof and refines with binary search to keep payload
  within the 2 MiB cap.
- `snapshot_tip_hash` and `root_hash` are returned from the same locked `MerkRangeIndex` snapshot as `proof`.
- After proof generation, server rechecks chain/index coherence. If the active chain or indexed snapshot moved underneath response assembly, the
  response is discarded and rebuilt, up to 3 attempts.
- Client continues with `start_height = end_height + 1` until `served_stop_hash == target_stop_hash`.
- This continuation is transport-level pagination, not construction of a single cross-response authenticated proof.

## Validation Semantics

- `proof` proves consistency with `root_hash`.
- `snapshot_tip_hash` is a response-side snapshot label, not proof-authenticated data.
- `served_stop_hash` identifies the served end block, but it is not the snapshot anchor.
- Each `merkrange` response is independently verifiable and valid on its own, regardless of `snapshot_tip_hash` values seen in earlier or later responses.
- Responses are only directly comparable as products of the same index snapshot when `snapshot_tip_hash` matches.
- Different `snapshot_tip_hash` values do not invalidate any individual response by themselves.
- A change in `snapshot_tip_hash` between sequential `getmerkrange` responses does not invalidate either response and must not by itself cause rejection.
- If a client chooses to perform additional cross-response consistency checks or analytics across multiple independently valid responses,
  `snapshot_tip_hash` can be used as a consistency signal to detect that those responses came from different merkrange snapshots.
- `snapshot_tip_hash == 0` is invalid in both P2P and RPC responses.

### Example

- A client requests `0..9999`, then `10000..19999`.
- If both responses verify individually, both responses are acceptable even if `snapshot_tip_hash` differs between them.
- Matching `snapshot_tip_hash` values only mean the two responses came from the same merkrange index snapshot; they are not required for validity.

## RPC

- `getmerkrange` returns:
  - `snapshot_tip_hash`
  - `root_hash`
  - `proof`
  - `proof_size`

## CLI / Indexing

- Enable with: `-merkrangeindex=basic`
- Index appears as: `basic merk range index`

## Testing

- Unit: `./src/test/test_dash --run_test=merkrangeindex_tests`
- Functional: `test/functional/p2p_merkrange.py`
- Functional (index visibility): `test/functional/rpc_misc.py`
