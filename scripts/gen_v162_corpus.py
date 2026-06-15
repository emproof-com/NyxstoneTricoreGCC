#!/usr/bin/env python3
# Regenerate tests/tricore_v162_insns.inc from the binutils TriCore opcode
# table (the authoritative instruction list).  Run after bumping the pinned
# binutils commit:
#
#   NYX_OPC=third_party/binutils-tricore/.../opcodes/tricore-opc.c \
#       python3 scripts/gen_v162_corpus.py
#
# Emits every instruction/format available in the v1.6.2 ISA as
# { mnemonic, operand-arg-spec, is_32bit }.  tests/roundtrip_all.cpp turns
# each into assembly, round-trips it, and validates reference resolution.
import re, sys, os
SRC = os.environ.get("NYX_OPC", "/tmp/tricore-src/opcodes/tricore-opc.c")
# ISA bit values (from include/opcode/tricore.h)
V = {'TRICORE_RIDER_A':0x1,'TRICORE_V1_1':0x1,'TRICORE_V1_2':0x2,'TRICORE_V1_3':0x4,
     'TRICORE_V1_3_1':0x100,'TRICORE_V1_6':0x200,'TRICORE_V1_6_1':0x400,
     'TRICORE_V1_6_2':0x800,'TRICORE_V1_8':0x1000}
V['TRICORE_V1_8_UP']=0x1000
V['TRICORE_V1_6_2_UP']=0x800|V['TRICORE_V1_8_UP']
V['TRICORE_V1_6_1_UP']=0x400|V['TRICORE_V1_6_2_UP']
V['TRICORE_V1_6_UP']=0x200|V['TRICORE_V1_6_1_UP']
V['TRICORE_V1_3_1_UP']=0x100|V['TRICORE_V1_6_UP']
V['TRICORE_V1_3_UP']=0x4|V['TRICORE_V1_3_1_UP']
V['TRICORE_V1_2_UP']=0x2|V['TRICORE_V1_3_UP']
V['TRICORE_V1_2_DN']=0x2
V['TRICORE_V1_3_DN']=0x4|0x2
V['TRICORE_V1_3_X_DN']=0x4|0x2|0x100
V['TRICORE_V1_3_1_DN']=0x100|V['TRICORE_V1_3_DN']
V['TRICORE_V1_6_DN']=0x200|V['TRICORE_V1_3_1_DN']
V['TRICORE_V1_6_1_DN']=0x400|V['TRICORE_V1_6_DN']
V['TRICORE_V1_6_2_DN']=0x800|V['TRICORE_V1_6_1_DN']
V['TRICORE_V1_8_DN']=0x1000|V['TRICORE_V1_6_2_DN']
MASK=0x1f0f; CUR=0x800  # current_isa = V1_6_2

txt = open(SRC).read()
# isolate tricore_opcodes[] array body
m = txt.index("struct tricore_opcode tricore_opcodes[]")
body = txt[m:]
# entries look like: {"name", len32, 0x..., 0x..., F(FMT), nr, "args", "fields", ISA},
pat = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*,\s*0x[0-9a-fA-F]+\s*,\s*0x[0-9a-fA-F]+\s*,\s*F\(\w+\)\s*,\s*(\d+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"[^"]*"\s*,\s*([A-Z0-9_|() ]+?)\s*[,}]', re.S)
seen=set(); out=[]
for mo in pat.finditer(body):
    name, len32, nr, args, isa = mo.group(1),int(mo.group(2)),int(mo.group(3)),mo.group(4),mo.group(5)
    # evaluate isa expression (OR of macro names)
    val=0
    for tok in re.split(r'[|() ]+', isa):
        tok=tok.strip()
        if tok in V: val|=V[tok]
        elif tok=='': pass
    if not ((val & MASK) & CUR): continue   # not available in v1.6.2
    key=(name,args,len32)
    if key in seen: continue
    seen.add(key)
    out.append((name,args,len32))
print(f"// auto-generated: {len(out)} v1.6.2 instruction encodings", file=sys.stderr)
# arg-char histogram
from collections import Counter
ch=Counter(c for _,a,_ in out for c in a)
print("// arg-char freq:", dict(sorted(ch.items())), file=sys.stderr)
with open("/workspaces/NyxstoneTricoreGCC/tests/tricore_v162_insns.inc","w") as f:
    f.write("// Auto-generated from binutils tricore-opc.c (commit 4384fa19), v1.6.2 subset.\n")
    f.write("// Each entry: { mnemonic, operand-arg-spec, is_32bit }.  Do not hand-edit;\n")
    f.write("// regenerate with scripts/gen_v162_corpus.py.\n")
    f.write(f"// {len(out)} instruction/format entries.\n")
    f.write("static const InsnSpec V162_INSNS[] = {\n")
    for name,args,len32 in out:
        a=args.replace('\\','\\\\').replace('"','\\"')
        f.write(f'    {{"{name}", "{a}", {len32}}},\n')
    f.write("};\n")
print("wrote tests/tricore_v162_insns.inc", file=sys.stderr)
