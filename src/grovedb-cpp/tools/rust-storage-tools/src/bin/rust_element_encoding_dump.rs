use grovedb::Element;
use grovedb_version::version::GroveVersion;

fn to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push_str(&format!("{:02x}", b));
    }
    out
}

fn main() {
    let version = GroveVersion::latest();
    let item = Element::new_item(b"v1".to_vec())
        .serialize(version)
        .expect("serialize item");
    let tree = Element::empty_tree()
        .serialize(version)
        .expect("serialize tree");
    println!("ITEM_V1={}", to_hex(&item));
    println!("TREE_EMPTY={}", to_hex(&tree));
}
