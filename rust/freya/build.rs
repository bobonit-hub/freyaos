//! Checks src/sys.rs against include/freya_api.h.
//!
//! The header is compiled with the same arm-none-eabi-gcc that links the
//! program, into assembly that spells out every struct size, every field
//! offset and every constant that src/sys.rs names (the classic
//! asm-offsets trick: nothing has to run on the target).  The numbers
//! become const assertions in $OUT_DIR/layout_check.rs, which lib.rs
//! includes, so a table that no longer matches the kernel's does not
//! build.
//!
//! FREYA_CC names the compiler (arm-none-eabi-gcc by default) and
//! FREYA_BOARD the board (blackpill by default; the layout is the same on
//! every board, but the header refuses to compile without one).

use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

struct Item {
    key: String,  // what the C side prints it as
    rust: String, // the Rust expression it is compared with
}

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../..");
    let sys_rs = manifest.join("src/sys.rs");
    let header = root.join("include/freya_api.h");
    let fat_h = root.join("src/fat.h");

    println!("cargo:rerun-if-changed={}", sys_rs.display());
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed={}", fat_h.display());
    println!("cargo:rerun-if-env-changed=FREYA_CC");
    println!("cargo:rerun-if-env-changed=FREYA_BOARD");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let check = out.join("layout_check.rs");

    // Only the target has the C layout; a host `cargo doc` has 8-byte
    // pointers and nothing to compare.
    if env::var("CARGO_CFG_TARGET_ARCH").as_deref() != Ok("arm") {
        fs::write(&check, "").unwrap();
        return;
    }

    let src = fs::read_to_string(&sys_rs).unwrap();
    let (mut c, items) = c_source(&src);
    c.insert_str(0, "#include <stddef.h>\n#include \"freya_api.h\"\n#include \"fat.h\"\n");

    let c_file = out.join("layout.c");
    fs::write(&c_file, &c).unwrap();

    let cc = env::var("FREYA_CC").unwrap_or_else(|_| "arm-none-eabi-gcc".into());
    let board = env::var("FREYA_BOARD").unwrap_or_else(|_| "blackpill".into());
    let output = Command::new(&cc)
        .args(["-S", "-o", "-", "-mthumb", "-mcpu=cortex-m3"])
        .arg(format!("-DFREYA_BOARD_{}", board.to_uppercase()))
        .arg(format!("-I{}", root.join("include").display()))
        .arg(format!("-I{}", root.join("src").display()))
        .arg(&c_file)
        .output()
        .unwrap_or_else(|e| panic!("cannot run {cc} to check the ABI layout: {e}"));
    if !output.status.success() {
        panic!(
            "{cc} could not compile the layout check:\n{}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    let asm = String::from_utf8_lossy(&output.stdout);
    let mut values = std::collections::HashMap::new();
    for part in asm.split("@@").skip(1).step_by(2) {
        let mut it = part.split_whitespace();
        if let (Some(k), Some(v)) = (it.next(), it.next()) {
            values.insert(k.to_string(), v.trim_start_matches('#').to_string());
        }
    }

    let mut rs = String::new();
    for item in &items {
        let v = values
            .get(&item.key)
            .unwrap_or_else(|| panic!("the layout check has no value for {}", item.key));
        writeln!(
            rs,
            "const _: () = assert!(({}) as i64 == {v}, \"{} does not match include/freya_api.h\");",
            item.rust, item.key
        )
        .unwrap();
    }
    fs::write(&check, rs).unwrap();
}

/// The C name of a Rust field: `yield` is a keyword in Rust.
fn c_field(name: &str) -> &str {
    name.strip_suffix('_').unwrap_or(name)
}

fn c_source(src: &str) -> (String, Vec<Item>) {
    let mut body = String::new();
    let mut items = Vec::new();
    let mut add = |key: String, c: String, rust: String| {
        writeln!(
            body,
            "    __asm__ volatile (\"\\n.ascii \\\"@@{key} %c0@@\\\"\" :: \"n\" ((long)({c})));"
        )
        .unwrap();
        items.push(Item { key, rust });
    };

    let mut current: Option<String> = None;
    for line in src.lines() {
        let t = line.trim_start();
        let indent = line.len() - t.len();

        if indent == 0 {
            if let Some(rest) = t.strip_prefix("pub struct ") {
                let name = rest.split(|ch: char| !ch.is_alphanumeric() && ch != '_').next().unwrap();
                add(
                    format!("size:{name}"),
                    format!("sizeof({name})"),
                    format!("::core::mem::size_of::<sys::{name}>()"),
                );
                current = Some(name.to_string());
                continue;
            }
            if t.starts_with('}') {
                current = None;
            }
            if let Some(rest) = t.strip_prefix("pub const ") {
                let name = rest.split(':').next().unwrap().trim();
                if name.starts_with("FREYA_") || name.starts_with("FAT_") {
                    add(format!("const:{name}"), name.to_string(), format!("sys::{name}"));
                }
            }
            continue;
        }

        // A field is `pub name:` one level into its struct; the parameter
        // lists of the function pointers are indented further.
        if let (Some(st), 4) = (&current, indent) {
            if let Some(rest) = t.strip_prefix("pub ") {
                let field = rest.split(':').next().unwrap().trim();
                add(
                    format!("off:{st}.{}", c_field(field)),
                    format!("offsetof({st}, {})", c_field(field)),
                    format!("::core::mem::offset_of!(sys::{st}, {field})"),
                );
            }
        }
    }

    (format!("void freya_layout(void)\n{{\n{body}}}\n"), items)
}
