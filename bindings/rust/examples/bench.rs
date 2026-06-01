// Throughput benchmark for the Rust binding, mirrors examples/bench.cpp.
// Usage: cargo run --release --example bench -- [seconds]
use nyxstone_tricore_gcc::NyxstoneTricoreGCC;
use std::time::Instant;

const INSNS: [&str; 10] = [
    "mov %d4, %d5", "add %d4, %d5", "sub %d4, %d5", "nop", "ret",
    "mov.aa %a4, %a5", "ld.w %d4, [%a4]", "st.w [%a4], %d4",
    "and %d4, %d5", "or %d4, %d5",
];

fn make_pkg(n: usize) -> String {
    let mut s = String::new();
    for i in 0..n {
        if i > 0 { s.push_str("; "); }
        s.push_str(INSNS[i % INSNS.len()]);
    }
    s
}

fn run_for<F: FnMut()>(secs: f64, mut f: F) -> f64 {
    for _ in 0..3 { f(); }
    let t0 = Instant::now();
    let mut count = 0u64;
    loop {
        for _ in 0..10 { f(); count += 1; }
        let e = t0.elapsed().as_secs_f64();
        if e >= secs { return (count as f64) / e; }
    }
}

fn fmt(v: f64) -> String {
    if v >= 1e6 { format!("{:>8.2} M", v / 1e6) }
    else if v >= 1e3 { format!("{:>8.2} k", v / 1e3) }
    else { format!("{v:>10.2}") }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let secs = std::env::args().nth(1)
        .and_then(|s| s.parse::<f64>().ok())
        .unwrap_or(1.0);

    let nx = NyxstoneTricoreGCC::new()?;
    println!("NyxstoneTricoreGCC (Rust) bench, target {secs}s/measurement");

    for &pkg in &[1usize, 10] {
        let text = make_pkg(pkg);
        let bytes = nx.assemble(&text, 0, &[])?;
        let r_asm = run_for(secs, || { let _ = nx.assemble(&text, 0, &[]); });
        let r_dis = run_for(secs, || { let _ = nx.disassemble_to_instructions(&bytes, 0, 0); });
        println!("  Package {pkg} ({pkg} insns, {} bytes)", bytes.len());
        println!("    assemble    : {} ops/s   {} insns/s",
                 fmt(r_asm), fmt(r_asm * pkg as f64));
        println!("    disassemble : {} ops/s   {} insns/s",
                 fmt(r_dis), fmt(r_dis * pkg as f64));
    }
    Ok(())
}
