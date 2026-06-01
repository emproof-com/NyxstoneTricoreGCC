// NyxstoneTricoreGCC: in-process TriCore assembler/disassembler library.
//
// Thin C++17 wrapper around the GNU assembler's `md_assemble` encoder and
// `libopcodes`' `print_insn_tricore` decoder.  Drives just enough of gas's
// per-pass machinery (symbol table, frag/obstack arenas, BFD section
// objects) to encode in-process, no file I/O, no fork, no debug/dwarf
// /cfi/listing.
//
// .text-only:  any directive that would switch the active section (e.g.
// `.data`, `.bss`, `.section .foo`, `.pushsection`) makes assembly fail.
//
// Threading: single-threaded only.  All GAS globals are process-wide;
// holding two `NyxstoneTricoreGCC` instances concurrently from different
// threads is undefined behaviour.
//
// API shape mirrors the sibling project
// [Nyxstone](https://github.com/emproof-com/nyxstone), same method names,
// same argument order, same `tl::expected<T, std::string>` error channel.
// Nyxstone is built on LLVM-MC and covers the architectures LLVM supports;
// NyxstoneTricoreGCC is an independent codebase using GNU binutils to cover
// TriCore (where LLVM-MC has no backend).

#pragma once

#include "nyxstone/expected.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nyxstone {

using u8      = uint8_t;
using u64     = uint64_t;
using Address = uint64_t;

/// @brief A symbol referenced by a relocation.
struct RelocationSymbol {
    /// Symbol name (typically the name passed in a LabelDefinition).
    std::string name;
    /// Resolved address (copied from the matching LabelDefinition; 0 if the
    /// caller did not supply one).
    Address address = 0;
};

/// @brief One relocation produced by an `*_with_relocs` call.
///
/// The shape mirrors what `gas -r` would write into an ELF object: an
/// offset within the produced byte stream, a relocation type (ELF
/// R_TRICORE_*), a symbol reference, and an addend.
struct RelocationInfo {
    /// Section-relative offset of the relocation site.
    Address offset = 0;
    /// Addend used by the linker (TriCore uses RELA so this is always set).
    std::optional<int64_t> addend;
    /// Symbol the relocation refers to.
    RelocationSymbol symbol;
    /// ELF relocation type, the R_TRICORE_* number that ends up in the
    /// reloc section.  Width-fixed so binding consumers don't need
    /// architecture-specific size knowledge.
    uint32_t relocation_type = 0;
};

/// @brief NyxstoneTricoreGCC class for assembling and disassembling TriCore code.
class NyxstoneTricoreGCC {
public:
    /// @brief Defines the location of a label by absolute address.
    struct LabelDefinition {
        /// The label name.
        std::string name;
        /// The absolute address of the label.
        uint64_t address;
    };

    /// @brief Complete instruction information.
    struct Instruction {
        /// The absolute address of the instruction.
        uint64_t address = 0;
        /// The assembly text of the instruction (as printed by libopcodes).
        std::string assembly;
        /// The encoded bytes of the instruction.
        std::vector<uint8_t> bytes;

        bool operator==(const Instruction& other) const {
            return address == other.address
                && assembly == other.assembly
                && bytes    == other.bytes;
        }
    };

    /// @brief Result bundle for `assemble_with_relocs`: the encoded bytes plus
    ///        the list of relocations for unresolved external symbols.
    struct AssembleWithRelocsResult {
        std::vector<uint8_t> bytes;
        std::vector<RelocationInfo> relocations;
    };

    /// @brief Result bundle for `assemble_to_instructions_with_relocs`.
    struct AssembleInstructionsWithRelocsResult {
        std::vector<Instruction> instructions;
        std::vector<RelocationInfo> relocations;
    };

    /// @brief Constructs a NyxstoneTricoreGCC instance.
    ///
    /// Performs the one-time process-wide gas init on first call.
    ///
    /// @return A unique_ptr holding the created NyxstoneTricoreGCC instance on
    ///         success, an error string otherwise.
    static tl::expected<std::unique_ptr<NyxstoneTricoreGCC>, std::string> create();

    /// @brief Translates assembly instructions at a given start address to bytes.
    ///
    /// Additional label definitions by absolute address may be supplied.
    /// Does not support assembly directives that change the active section
    /// (e.g. `.section`, `.pushsection`, `.data`, `.bss`); .text-only.
    ///
    /// @param assembly The assembly instruction(s) to be assembled.
    /// @param address  The absolute address of the first instruction.
    /// @param labels   External label definitions used by @p assembly.
    ///
    /// @return The assembled bytes on success, an error string otherwise.
    tl::expected<std::vector<u8>, std::string> assemble(
        const std::string& assembly,
        uint64_t address,
        const std::vector<LabelDefinition>& labels) const;

    /// @brief Translates assembly instructions at given start address to
    /// instruction details containing bytes.
    ///
    /// @param assembly The assembly instruction(s) to be assembled.
    /// @param address  The absolute address of the first instruction.
    /// @param labels   External label definitions used by @p assembly.
    ///
    /// @return The instruction details on success, an error string otherwise.
    tl::expected<std::vector<Instruction>, std::string> assemble_to_instructions(
        const std::string& assembly,
        uint64_t address,
        const std::vector<LabelDefinition>& labels) const;

    /// @brief Like @ref assemble, but leaves @p labels unresolved and returns
    /// a relocation entry for each reference, the same behaviour you get
    /// from `gcc/gas -r` for a partially-linked object.
    ///
    /// External label references in @p assembly stay as zero-valued
    /// placeholders in the byte stream; their resolution is described by
    /// the returned `RelocationInfo` records, whose `symbol.address` field
    /// is populated from the matching @p labels entry (or 0 if none).
    ///
    /// @return The assembled bytes plus relocations on success, an error
    ///         string otherwise.
    tl::expected<AssembleWithRelocsResult, std::string> assemble_with_relocs(
        const std::string& assembly,
        uint64_t address,
        const std::vector<LabelDefinition>& labels) const;

    /// @brief Like @ref assemble_to_instructions, but with `-r`-style
    /// relocation output (see @ref assemble_with_relocs).
    tl::expected<AssembleInstructionsWithRelocsResult, std::string>
    assemble_to_instructions_with_relocs(
        const std::string& assembly,
        uint64_t address,
        const std::vector<LabelDefinition>& labels) const;

    /// @brief Translates bytes to disassembly text at a given start address.
    ///
    /// @param bytes   The byte code to be disassembled.
    /// @param address The absolute address of the first byte.
    /// @param count   The number of instructions to disassemble; 0 means all.
    ///
    /// @return The disassembly on success, an error string otherwise.
    tl::expected<std::string, std::string> disassemble(
        const std::vector<uint8_t>& bytes,
        uint64_t address,
        size_t count) const;

    /// @brief Translates bytes to instruction details containing disassembly
    /// text at a given start address.
    ///
    /// @param bytes   The byte code to be disassembled.
    /// @param address The absolute address of the first byte.
    /// @param count   The number of instructions to disassemble; 0 means all.
    ///
    /// @return The instruction details on success, an error string otherwise.
    tl::expected<std::vector<Instruction>, std::string> disassemble_to_instructions(
        const std::vector<uint8_t>& bytes,
        uint64_t address,
        size_t count) const;

    NyxstoneTricoreGCC(NyxstoneTricoreGCC&&) noexcept = default;
    NyxstoneTricoreGCC& operator=(NyxstoneTricoreGCC&&) noexcept = default;
    NyxstoneTricoreGCC(const NyxstoneTricoreGCC&) = delete;
    NyxstoneTricoreGCC& operator=(const NyxstoneTricoreGCC&) = delete;

private:
    NyxstoneTricoreGCC() = default;
};

}  // namespace nyxstone
