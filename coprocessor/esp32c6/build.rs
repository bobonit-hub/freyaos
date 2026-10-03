use std::path::Path;

fn main() {
    // The console certificate's private key is compiled in but never
    // committed (see the README).
    let key = Path::new("certs/freya.key");
    println!("cargo:rerun-if-changed={}", key.display());
    if !key.exists() {
        panic!(
            "\n\ncerts/freya.key is missing: put the private key of certs/freya.crt there, or make \
             a new pair:\n\n  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
             -days 3650 -subj /CN=freya -keyout certs/freya.key -out certs/freya.crt\n\n"
        );
    }
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("espidf") {
        embuild::espidf::sysenv::output();
    }
}
