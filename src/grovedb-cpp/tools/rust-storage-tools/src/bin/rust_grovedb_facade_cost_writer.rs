use std::env;
use std::path::Path;

use grovedb::{Element, GroveDb};
use grovedb_costs::OperationCost;
use grovedb_version::version::GroveVersion;

fn print_cost(name: &str, cost: OperationCost) {
    println!(
        "{name}\t{}\t{}\t{}\t{}\t{}\t{}",
        cost.seek_count,
        cost.storage_loaded_bytes,
        cost.hash_node_calls,
        cost.storage_cost.added_bytes,
        cost.storage_cost.replaced_bytes,
        cost.storage_cost.removed_bytes.total_removed_bytes()
    );
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let db = GroveDb::open(Path::new(&path)).expect("open grovedb");
    let gv = GroveVersion::latest();

    let c = db
        .insert(
            &[] as &[&[u8]],
            b"root",
            Element::empty_tree(),
            None,
            None,
            &gv,
        )
        .cost_as_result()
        .expect("insert root cost");
    print_cost("insert_root_tree", c);

    let c = db
        .insert(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v1".to_vec()),
            None,
            None,
            &gv,
        )
        .cost_as_result()
        .expect("insert item cost");
    print_cost("insert_item", c);

    let c = db
        .insert(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            None,
            &gv,
        )
        .cost_as_result()
        .expect("replace item cost");
    print_cost("replace_item", c);

    let c = db
        .delete(&[b"root".as_slice()], b"k1", None, None, &gv)
        .cost_as_result()
        .expect("delete item cost");
    print_cost("delete_item", c);

    let tx = db.start_transaction();
    let c = db
        .insert(
            &[b"root".as_slice()],
            b"ktx",
            Element::new_item(b"tv".to_vec()),
            None,
            Some(&tx),
            &gv,
        )
        .cost_as_result()
        .expect("tx insert item cost");
    print_cost("tx_insert_item", c);
    db.rollback_transaction(&tx).unwrap();

    let c = db
        .insert(
            &[b"root".as_slice()],
            b"k2",
            Element::new_item(b"v2".to_vec()),
            None,
            None,
            &gv,
        )
        .cost_as_result()
        .expect("insert item k2 cost");
    print_cost("insert_item_k2", c);

    let tx = db.start_transaction();
    let c = db
        .delete(&[b"root".as_slice()], b"k2", None, Some(&tx), &gv)
        .cost_as_result()
        .expect("tx delete item cost");
    print_cost("tx_delete_item", c);
    db.rollback_transaction(&tx).unwrap();
}
