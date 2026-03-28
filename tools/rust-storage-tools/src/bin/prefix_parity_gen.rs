use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;

fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

fn print_prefix(label: &str, path: Vec<Vec<u8>>) {
    let segments: Vec<&[u8]> = path.iter().map(|v| v.as_slice()).collect();
    let subtree = SubtreePath::from(segments.as_slice());
    let prefix = RocksDbStorage::build_prefix(subtree).value;
    println!("{}={}", label, to_hex(&prefix));
}

fn main() {
    print_prefix("empty", vec![]);
    print_prefix("one", vec![b"a".to_vec()]);
    print_prefix("two", vec![b"root".to_vec(), b"child".to_vec()]);
    print_prefix(
        "three",
        vec![b"root".to_vec(), b"child".to_vec(), b"leaf".to_vec()],
    );
    print_prefix("binary", vec![vec![0x00, 0xff, 0x10]]);
}
