# C++ Rewrite Compatibility Invariants (GroveDB v4.0.0)

This document freezes the cross-language invariants that the C++ rewrite must preserve for GroveDB v4.0.0.
Any change requires a version bump and explicit sign-off.
Last validated: 2026-03-04

## Proof Encoding
- Proof bytes must be byte-for-byte identical to Rust output for the same input and version.
- Merk proof ops encoding matches Rust (opcode set, field order, endianness).
- GroveDB proof encoding is bincode v2, big-endian, no size limit.
- LayerProof nesting and key ordering are stable.
- Absence proofs must use the same boundary key selection rules as Rust.
- Reference rewriting must preserve proof validity and target element bytes.

## Element Serialization
- Element variants and field order must match Rust enum layout.
- Binary encoding uses bincode v2, big-endian, no size limit.
- Empty tree insertions use `Tree(None, flags)` semantics.
- Reference encoding preserves path kind and optional max-hop.

## Query Semantics
- Query/PathQuery/SizedQuery/QueryItem behavior matches Rust for all edge cases.
- Key ordering is lexicographic byte order.
- Limit/offset handling matches Rust (including invalid offsets for proved queries).
- Left-to-right traversal semantics match Rust.
- Subquery rules, conditional branches, and path additions match Rust.

## Cost Accounting
- OperationCost fields and aggregation match Rust: seek_count, storage_loaded_bytes,
  storage_added_bytes, storage_replaced_bytes, storage_removed_bytes, hash_node_calls.
- Storage byte accounting uses the same varint length rules as Rust.
- Batch cost application semantics match Rust (including deferred/pending cost behavior through commit flow).

## Storage Layout
- Prefixing matches Rust `RocksDbStorage::build_prefix` semantics.
- Column families: default, aux, roots, meta.
- Root keys stored at key "r" in roots CF, prefixed by subtree path.
- On-disk schema is mutually readable across Rust and C++ for supported version-gated APIs.

## Versioning
- Invariants apply per GroveDB version gate.
- Any divergence requires explicit version-specific behavior and documentation.
