// Smoke test for the Rust binding, mirrors examples/smoke.cpp.
use nyxstone_tricore_gcc_ipc::{LabelDefinition, NyxstoneTricoreGCC};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let nx = NyxstoneTricoreGCC::new()?;
    let base: u64 = 0x80000000;

    let mut all = Vec::new();
    for src in [
        "nop",
        "ret",
        "mov %d4, %d5",
        "add %d4, %d5, %d6",
        "movh %d4, 0x1234",
        "start:\n nop\n j here\nhere:\n ret\n",
        ".byte 0x11, 0x22, 0x33, 0x44",
        ".word 0xdeadbeef",
    ] {
        let bytes = nx.assemble(src, base + all.len() as u64, &[])?;
        let hex: Vec<String> = bytes.iter().map(|b| format!("{b:02x}")).collect();
        println!("--- {src}\n    [{} bytes]: {}", bytes.len(), hex.join(" "));
        all.extend_from_slice(&bytes);
    }

    println!("\n--- external label (j ext, ext = base + 8) ---");
    let labels = [LabelDefinition::new("ext", base + 8)];
    let ext = nx.assemble("nop\n nop\n nop\n j ext\n", base, &labels)?;
    let hex: Vec<String> = ext.iter().map(|b| format!("{b:02x}")).collect();
    println!("    [{} bytes]: {}", ext.len(), hex.join(" "));

    println!("\n--- assemble_with_relocs (gcc/gas -r equivalent) ---");
    let labels = [LabelDefinition::new("ext", 0x2000)];
    let (rel_bytes, relocs) = nx.assemble_with_relocs(
        "nop\n j ext\n", 0x1000, &labels)?;
    let hex: Vec<String> = rel_bytes.iter().map(|b| format!("{b:02x}")).collect();
    println!("    bytes ({}) : {}", rel_bytes.len(), hex.join(" "));
    for r in &relocs {
        println!("    reloc: off=0x{:x} type={} sym={}({:#x}) addend={:?}",
                 r.offset, r.relocation_type, r.symbol.name,
                 r.symbol.address, r.addend);
    }

    println!("\n--- disassemble_to_instructions ---");
    let insns = nx.disassemble_to_instructions(&all, base, 0)?;
    for ins in &insns {
        let hex: Vec<String> = ins.bytes.iter().map(|b| format!("{b:02x}")).collect();
        println!("    0x{:08x}  [{}]  {}", ins.address, hex.join(" "), ins.assembly);
    }

    println!("\n--- disassemble (text, first 3) ---");
    print!("{}", nx.disassemble(&all, base, 3)?);
    Ok(())
}
