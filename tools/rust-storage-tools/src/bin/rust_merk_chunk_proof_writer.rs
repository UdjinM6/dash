use std::env;
use std::fs;
use std::path::Path;

use grovedb_merk::ed::Encode;
use grovedb_merk::proofs::chunk::chunk_op::ChunkOp;
use grovedb_merk::proofs::chunk::util::generate_traversal_instruction;
use grovedb_merk::{ChunkProducer, Merk, Op, TreeFeatureType, TreeType};
use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use grovedb_storage::{Storage, StorageBatch};
use grovedb_version::version::GroveVersion;

fn write_file(path: &Path, name: &str, data: &[u8]) {
    let out_path = path.join(name);
    fs::write(out_path, data).expect("write output file");
}

fn encode_chunk_bytes(instruction: Vec<bool>, chunk: Vec<grovedb_merk::proofs::Op>) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(
        &ChunkOp::ChunkId(instruction)
            .encode()
            .expect("encode chunk id"),
    );
    out.extend_from_slice(&ChunkOp::Chunk(chunk).encode().expect("encode chunk"));
    out
}

fn encode_multi_chunk_bytes(chunk_ops: Vec<ChunkOp>) -> Vec<u8> {
    let mut out = Vec::new();
    for op in chunk_ops {
        out.extend_from_slice(&op.encode().expect("encode multi chunk op"));
    }
    out
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let db_path = Path::new(&path);

    let storage = RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
    let batch = StorageBatch::new();
    let transaction = storage.start_transaction();
    let subtree = SubtreePath::from(&[b"root" as &[u8]]);
    let ctx = storage
        .get_transactional_storage_context(subtree, Some(&batch), &transaction)
        .unwrap();
    let grove_version = GroveVersion::latest();
    let mut merk = Merk::open_base(
        ctx,
        TreeType::NormalTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

    let mut ops = Vec::new();
    for i in 0..15u8 {
        let key = format!("k{:02}", i).into_bytes();
        let value = format!("v{:02}", i).into_bytes();
        ops.push((key, Op::Put(value, TreeFeatureType::BasicMerkNode)));
    }
    merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
        .unwrap()
        .expect("apply ops");
    storage
        .commit_multi_context_batch(batch, None)
        .unwrap()
        .expect("commit batch");

    let transaction = storage.start_transaction();
    let ctx = storage
        .get_transactional_storage_context(
            SubtreePath::from(&[b"root" as &[u8]]),
            None,
            &transaction,
        )
        .unwrap();
    let merk = Merk::open_base(
        ctx,
        TreeType::NormalTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

    let height = merk.height().expect("expected merk height after inserts") as usize;
    let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

    let chunk_count = chunk_producer.len();
    write_file(
        db_path,
        "chunk_count.txt",
        chunk_count.to_string().as_bytes(),
    );

    let mut encoded_chunks: Vec<Vec<u8>> = Vec::with_capacity(chunk_count);
    for i in 1..=chunk_count {
        let (chunk_ops, _) = chunk_producer
            .chunk_with_index(i, &grove_version)
            .expect("chunk with index");
        let instruction = generate_traversal_instruction(height, i).expect("chunk instruction");
        let chunk_bytes = encode_chunk_bytes(instruction, chunk_ops);
        encoded_chunks.push(chunk_bytes.clone());
        let file_name = format!("chunk_{}.bin", i);
        write_file(db_path, &file_name, &chunk_bytes);
    }

    let multi_unlimited = chunk_producer
        .multi_chunk_with_limit_and_index(2, None, &grove_version)
        .expect("multichunk unlimited");
    write_file(
        db_path,
        "multichunk_2_unlimited.bin",
        &encode_multi_chunk_bytes(multi_unlimited.chunk),
    );
    if let Some(next) = multi_unlimited.next_index {
        write_file(db_path, "multichunk_2_unlimited_next.bin", &next);
    } else {
        write_file(db_path, "multichunk_2_unlimited_next.bin", &[]);
    }

    let limit_two_chunks = encoded_chunks[1].len() + encoded_chunks[2].len() + 5;
    let multi_limited = chunk_producer
        .multi_chunk_with_limit_and_index(2, Some(limit_two_chunks), &grove_version)
        .expect("multichunk limited");
    write_file(
        db_path,
        "multichunk_2_limited.bin",
        &encode_multi_chunk_bytes(multi_limited.chunk),
    );
    if let Some(next) = multi_limited.next_index {
        write_file(db_path, "multichunk_2_limited_next.bin", &next);
    } else {
        write_file(db_path, "multichunk_2_limited_next.bin", &[]);
    }
    let remaining = multi_limited
        .remaining_limit
        .expect("remaining limit expected");
    write_file(
        db_path,
        "multichunk_2_limited_remaining.txt",
        remaining.to_string().as_bytes(),
    );

    let limit_too_small = chunk_producer.multi_chunk_with_limit_and_index(
        2,
        Some(encoded_chunks[1].len() - 1),
        &grove_version,
    );
    write_file(
        db_path,
        "multichunk_2_limit_small_ok.txt",
        if limit_too_small.is_err() { b"1" } else { b"0" },
    );
}
