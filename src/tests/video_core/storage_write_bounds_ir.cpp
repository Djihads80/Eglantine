// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/storage_write_bounds_ir.h"

namespace {
using namespace Shader;
using namespace Shader::StorageWriteBounds;

class NativeEnvironment final : public Environment {
public:
    NativeEnvironment() { stage = Stage::Compute; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        return index == 0 && (offset == 0x150 || offset == 0x160);
    }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return {}; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    IR::Program program;

    Fixture() {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
    }
    IR::U64 Address(const IR::U32& offset, u32 slot = 0x150) {
        const auto low = ir.GetCbuf(ir.Imm32(0), ir.Imm32(slot));
        const auto high = ir.GetCbuf(ir.Imm32(0), ir.Imm32(slot + 4));
        return ir.PackUint2x32(ir.CompositeConstruct(ir.IAdd(low, offset), high));
    }
    void GlobalStore(const IR::U32& offset, u32 bytes) {
        const auto address = Address(offset);
        // Store VALUES intentionally depend on unknown guest data. Only address
        // provenance should matter; the extractor must not try to evaluate them.
        const auto value = ir.GetCbuf(ir.Imm32(3), ir.Imm32(0x78));
        if (bytes == 4) {
            ir.WriteGlobal32(address, value);
        } else if (bytes == 8) {
            ir.WriteGlobal64(address, ir.CompositeConstruct(value, value));
        } else {
            ir.WriteGlobal128(address, ir.CompositeConstruct(value, value, value, value));
        }
    }
    void Lower() {
        HostTranslateInfo host{};
        host.min_ssbo_alignment = 256;
        NativeEnvironment env;
        Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    }
    std::vector<DescriptorProof> Proof() { return Extract(program, 256); }
};

bool Covers(const std::vector<ByteRange>& ranges, u64 begin, u64 bytes) {
    return std::ranges::any_of(ranges, [=](const ByteRange& range) {
        return range.begin <= begin && range.end >= begin + bytes;
    });
}
} // namespace

TEST_CASE("Real global-to-storage IR proves all seven affine record stores",
          "[write_bounds][write_bounds_ir]") {
    // Synthetic equivalent ADDRESS shapes of the measured hot shader family.
    // No game instructions/data are embedded and no hash permits a shortcut.
    for (const auto [stride, last_field] :
         {std::pair{256U, 0xf4U}, {272U, 0x104U}, {256U, 0xf8U}}) {
        Fixture fixture;
        auto& ir = fixture.ir;
        const IR::U32 index = ir.IAdd(ir.IMul(ir.WorkgroupIdX(), ir.Imm32(64)),
                                     ir.LocalInvocationIdX());
        const auto offset = ir.IMul(index, ir.Imm32(stride));
        const std::array fields{std::pair{0x80U, 8U}, std::pair{0x88U, 4U},
                                std::pair{0x90U, 8U}, std::pair{0x98U, 4U},
                                std::pair{0xa0U, 8U}, std::pair{0xa8U, 4U},
                                std::pair{last_field, 4U}};
        for (const auto [field, bytes] : fields) {
            fixture.GlobalStore(IR::U32{ir.IAdd(offset, ir.Imm32(field))}, bytes);
        }
        fixture.Lower();
        const auto proofs = fixture.Proof();
        REQUIRE(proofs.size() == 1);
        REQUIRE(proofs[0].complete);
        CHECK(proofs[0].cbuf_offset == 0x150);
        REQUIRE(proofs[0].stores.size() == 7);
        for (const u32 groups : {1U, 3U}) {
            const Dispatch dispatch{{groups, 1, 1}, fixture.program.workgroup_size, true};
            const auto ranges = EvaluateProof(proofs[0], dispatch, 8 * 1024 * 1024, true);
            REQUIRE(ranges);
            CHECK(ranges->back().end <= (64 * groups - 1) * stride + last_field + 4 + 255);
            // Enumerate every lane and ALL possible base alignment residues. Both
            // vector-alias and scalar-fallback SPIR-V physical address forms fit.
            for (u32 group = 0; group < groups; ++group) {
                for (u32 lane = 0; lane < 64; ++lane) {
                    for (u32 bias = 0; bias < 256; ++bias) {
                        for (const auto [field, bytes] : fields) {
                            const u64 raw = bias + stride * (64 * group + lane) + field;
                            REQUIRE(Covers(*ranges, raw & ~(u64{bytes} - 1), bytes));
                            REQUIRE(Covers(*ranges, raw & ~u64{3}, bytes));
                        }
                    }
                }
            }
        }
        CHECK_FALSE(EvaluateProof(proofs[0], {{1, 1, 1}, {64, 1, 1}, true}, 8 << 20));
        CHECK_FALSE(EvaluateProof(proofs[0], {{1, 1, 1}, {64, 1, 1}, false}, 8 << 20, true));
    }
}

TEST_CASE("IR proof accounts for atomics and rejects any unsupported additional writer",
          "[write_bounds][write_bounds_ir]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    fixture.GlobalStore(ir.Imm32(0x80), 16);
    fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::GlobalAtomicIAdd32,
                                {fixture.Address(ir.Imm32(0x200)), ir.Imm32(1)});
    fixture.Lower();
    auto proof = fixture.Proof();
    REQUIRE(proof.size() == 1);
    REQUIRE(proof[0].complete);
    REQUIRE(proof[0].stores.size() == 2);
    const auto ranges = EvaluateProof(proof[0], {{1, 1, 1}, {64, 1, 1}, true}, 4096, true);
    REQUIRE(ranges);
    CHECK(Covers(*ranges, 0x200, 4));

    SECTION("An unsupported subword storage write invalidates the whole descriptor") {
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::WriteStorageU8,
                                    {ir.Imm32(0), ir.Imm32(0x900), ir.Imm32(7)});
    }
    SECTION("A remaining global write invalidates all descriptors") {
        ir.WriteGlobal32(ir.Imm64(u64{0x8000}), ir.Imm32(7));
    }
    SECTION("Dynamic storage indexing invalidates all descriptors") {
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::WriteStorage32,
                                    {ir.LocalInvocationIdX(), ir.Imm32(0x900), ir.Imm32(7)});
    }
    proof = fixture.Proof();
    REQUIRE(proof.size() == 1);
    CHECK_FALSE(proof[0].complete);
}

TEST_CASE("IR proof rejects ambiguous base, unknown offsets, loops and missing stores",
          "[write_bounds][write_bounds_ir]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    SECTION("A scalar uniform in the offset is not read from CPU memory as proof") {
        fixture.GlobalStore(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x180)), 4);
        fixture.Lower();
    }
    SECTION("Two possible base pointers retain ambiguous global addressing") {
        const auto first = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x150));
        const auto second = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x160));
        const IR::U32 base{ir.Select(ir.Imm1(true), first, second)};
        ir.WriteGlobal32(ir.PackUint2x32(ir.CompositeConstruct(base, ir.Imm32(1))), ir.Imm32(0));
        fixture.Lower();
    }
    SECTION("A loop rejects even an apparently invariant constant store") {
        fixture.GlobalStore(ir.Imm32(0x80), 4);
        fixture.Lower();
        fixture.program.syntax_list.push_back({{}, IR::AbstractSyntaxNode::Type::Loop});
    }
    SECTION("A written descriptor without an accounted store is not a zero-write proof") {
        fixture.program.info.storage_buffers_descriptors.push_back({0, 0x150, 1, true});
    }
    SECTION("Two added copies of the same base are not canceled as one") {
        fixture.GlobalStore(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x150)), 4);
        fixture.Lower();
    }
    const auto proofs = fixture.Proof();
    REQUIRE_FALSE(proofs.empty());
    for (const auto& proof : proofs) {
        CHECK_FALSE(proof.complete);
    }
}

TEST_CASE("A writer in another block cannot be omitted from the proof",
          "[write_bounds][write_bounds_ir]") {
    Fixture fixture;
    fixture.GlobalStore(fixture.ir.Imm32(0x80), 4);
    fixture.Lower();
    REQUIRE(fixture.Proof()[0].complete);
    IR::Block branch{fixture.pool};
    branch.PrependNewInst(branch.end(), IR::Opcode::WriteStorage32,
                         {fixture.ir.Imm32(0), fixture.ir.Imm32(0x900), fixture.ir.Imm32(7)});
    // Deliberately absent from program.blocks: post-order or syntax ownership
    // must still make the unsupported offset invalidate the whole proof.
    fixture.program.post_order_blocks.push_back(&branch);
    CHECK_FALSE(fixture.Proof()[0].complete);
}

TEST_CASE("Aligned storage addressing rejects intermediate wrap at dispatch evaluation",
          "[write_bounds][write_bounds_ir]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    const auto large = ir.IMul(ir.LocalInvocationIdX(), ir.Imm32(0x80000000U));
    fixture.GlobalStore(large, 4);
    fixture.Lower();
    const auto proof = fixture.Proof();
    REQUIRE(proof[0].complete); // Structural proof; bounds still depend on dispatch.
    CHECK_FALSE(EvaluateProof(proof[0], {{1, 1, 1}, {64, 1, 1}, true}, 8 << 20, true));
    CHECK_FALSE(EvaluateProof(proof[0], {{1, 1, 1}, {1, 1, 1}, true}, 128, true));
    CHECK(EvaluateProof(proof[0], {{1, 1, 1}, {1, 1, 1}, true}, 4096, true));
}

TEST_CASE("Production storage IR proves a decomposed unsigned multiply address",
          "[write_bounds][write_bounds_ir][write_bounds_bitfield]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    const IR::U32 index = ir.IAdd(ir.ShiftLeftLogical(ir.WorkgroupIdX(), ir.Imm32(6)),
                                 ir.LocalInvocationIdX());
    const auto low = ir.BitFieldExtract(index, ir.Imm32(0), ir.Imm32(16), false);
    const auto high = ir.BitFieldExtract(index, ir.Imm32(16), ir.Imm32(16), false);
    const auto low_product = ir.IMul(low, ir.Imm32(272));
    const IR::U32 high_product = ir.IAdd(ir.IMul(low, ir.Imm32(0)), ir.IMul(high, ir.Imm32(272)));
    const IR::U32 offset = ir.IAdd(ir.ShiftLeftLogical(high_product, ir.Imm32(16)), low_product);
    const std::array fields{std::pair{0x80U, 8U}, std::pair{0x88U, 4U},
        std::pair{0x90U, 8U}, std::pair{0x98U, 4U}, std::pair{0xa0U, 8U},
        std::pair{0xa8U, 4U}, std::pair{0x104U, 4U}};
    for (const auto [field, bytes] : fields)
        fixture.GlobalStore(IR::U32{ir.IAdd(offset, ir.Imm32(field))}, bytes);
    fixture.Lower();
    const auto proofs = fixture.Proof();
    REQUIRE(proofs.size() == 1);
    REQUIRE(proofs[0].complete);
    REQUIRE(proofs[0].stores.size() == 7);
    for (const u32 groups : {1U, 3U, 1025U}) {
        const auto ranges = EvaluateProof(proofs[0], {{groups, 1, 1}, {64, 1, 1}, true},
                                          64 * 1024 * 1024, true);
        REQUIRE(ranges);
        // Exercise both sides of the16-bit split without iterating a million
        // invocations. The scalar helper test separately enumerates every value.
        for (const u32 group : {0U, groups - 1, groups > 1 ? groups - 2 : 0U}) {
            for (u32 lane = 0; lane < 64; ++lane) {
                for (u32 bias = 0; bias < 256; ++bias) {
                    for (const auto [field, bytes] : fields) {
                        const u64 raw = bias + u64{272} * (64 * group + lane) + field;
                        REQUIRE(Covers(*ranges, raw & ~(u64{bytes} - 1), bytes));
                        REQUIRE(Covers(*ranges, raw & ~u64{3}, bytes));
                    }
                }
            }
        }
    }
    CHECK_FALSE(EvaluateProof(proofs[0], {{1, 1, 1}, {64, 1, 1}, false}, 8 << 20, true));
    // Large grid makes the high16 contribution's left shift overflow. Masking
    // earlier bits does not authorize modular addressing outside this subset.
    CHECK_FALSE(EvaluateProof(proofs[0], {{1048576, 1, 1}, {64, 1, 1}, true},
                                          u64{1} << 33, true));
}

TEST_CASE("Unsigned extraction never substitutes a pointer, dynamic bit width or signed operation",
          "[write_bounds][write_bounds_ir][write_bounds_bitfield]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    SECTION("Pointer-dependent extracted bits") {
        const auto pointer_low = ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x150));
        fixture.GlobalStore(ir.BitFieldExtract(pointer_low, ir.Imm32(0), ir.Imm32(16), false), 4);
    }
    SECTION("Dynamic bit offset") {
        fixture.GlobalStore(ir.BitFieldExtract(ir.LocalInvocationIdX(), ir.WorkgroupIdX(),
                                               ir.Imm32(16), false), 4);
    }
    SECTION("Dynamic bit count") {
        fixture.GlobalStore(ir.BitFieldExtract(ir.LocalInvocationIdX(), ir.Imm32(0),
                                               ir.WorkgroupIdX(), false), 4);
    }
    SECTION("Signed extraction remains unsupported") {
        fixture.GlobalStore(ir.BitFieldExtract(ir.LocalInvocationIdX(), ir.Imm32(0),
                                               ir.Imm32(16), true), 4);
    }
    SECTION("Generic bitwise OR remains unsupported") {
        fixture.GlobalStore(ir.BitwiseOr(ir.LocalInvocationIdX(), ir.Imm32(16)), 4);
    }
    SECTION("Extraction outside the32-bit input is undefined") {
        fixture.GlobalStore(ir.BitFieldExtract(ir.LocalInvocationIdX(), ir.Imm32(20),
                                               ir.Imm32(16), false), 4);
    }
    fixture.Lower();
    const auto proofs = fixture.Proof();
    REQUIRE_FALSE(proofs.empty());
    for (const auto& proof : proofs) CHECK_FALSE(proof.complete);
}

TEST_CASE("Joint IR proof covers every read and write without accepting write-only metadata",
          "[write_bounds][write_bounds_ir][proven_sync]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    const IR::U32 index = ir.IAdd(ir.IMul(ir.WorkgroupIdX(), ir.Imm32(64)), ir.LocalInvocationIdX());
    const IR::U32 record = ir.IMul(index, ir.Imm32(256));
    for (const u32 field : {128U, 144U, 160U})
        (void)ir.LoadGlobal128(fixture.Address(IR::U32{ir.IAdd(record, ir.Imm32(field))}));
    for (const u32 field : {236U, 240U, 244U})
        (void)ir.LoadGlobal32(fixture.Address(IR::U32{ir.IAdd(record, ir.Imm32(field))}));
    fixture.GlobalStore(IR::U32{ir.IAdd(record, ir.Imm32(128))}, 8);
    fixture.GlobalStore(IR::U32{ir.IAdd(record, ir.Imm32(244))}, 4);
    fixture.Lower();
    const auto writes = fixture.Proof();
    const auto access = ExtractReadWrites(fixture.program, 256);
    REQUIRE(access.size() == 1);
    REQUIRE(writes[0].complete);
    REQUIRE(access[0].bounds.complete);
    CHECK(writes[0].stores.size() == 2);
    CHECK(access[0].bounds.stores.size() == 8);
    const auto ranges = Evaluate(access[0].bounds.nodes, access[0].bounds.stores,
        {{1, 1, 1}, {64, 1, 1}, true}, 0, 8 << 20, true);
    REQUIRE(ranges);
    REQUIRE(ranges->size() == 1);
    CHECK((*ranges)[0] == ByteRange{128, 16631});
}

TEST_CASE("Joint IR proof invalidates unknown reads while preserving the independent store proof",
          "[write_bounds][write_bounds_ir][proven_sync]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    fixture.GlobalStore(ir.Imm32(128), 4);
    SECTION("Runtime scalar cbuf offset cannot become a cached constant") {
        (void)ir.LoadGlobal32(fixture.Address(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x180))));
        fixture.Lower();
    }
    SECTION("Unsupported byte load has wider fallback semantics") {
        fixture.Lower();
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::LoadStorageU8,
                                    {ir.Imm32(0), ir.Imm32(128)});
    }
    SECTION("A remaining global read invalidates all access proofs") {
        fixture.Lower();
        (void)ir.LoadGlobal32(ir.Imm64(u64{0x8000}));
    }
    SECTION("Dynamic storage descriptor invalidates all access proofs") {
        fixture.Lower();
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::LoadStorage32,
                                    {ir.LocalInvocationIdX(), ir.Imm32(128)});
    }
    const auto writes = fixture.Proof();
    const auto access = ExtractReadWrites(fixture.program, 256);
    REQUIRE(writes.size() == 1);
    REQUIRE(access.size() == 1);
    CHECK(writes[0].complete);
    CHECK_FALSE(access[0].bounds.complete);
}

TEST_CASE("Joint IR proof covers read-only descriptors and atomic read-modify-write",
          "[write_bounds][write_bounds_ir][proven_sync]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    SECTION("Read-only descriptor has a real narrow read footprint") {
        (void)ir.LoadGlobal32(fixture.Address(ir.Imm32(364)));
        fixture.Lower();
        const auto writes = fixture.Proof();
        REQUIRE(writes.size() == 1);
        CHECK_FALSE(writes[0].complete);
        const auto access = ExtractReadWrites(fixture.program, 256);
        REQUIRE(access.size() == 1);
        REQUIRE(access[0].bounds.complete);
        CHECK(access[0].bounds.stores.size() == 1);
    }
    SECTION("Supported atomic contributes its entire operand width") {
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::GlobalAtomicIAdd32,
                                    {fixture.Address(ir.Imm32(512)), ir.Imm32(1)});
        fixture.Lower();
        const auto access = ExtractReadWrites(fixture.program, 256);
        REQUIRE(access.size() == 1);
        REQUIRE(access[0].bounds.complete);
        CHECK(access[0].bounds.stores.size() == 1);
        CHECK(access[0].bounds.stores[0].bytes == 4);
    }
}

TEST_CASE("Unresolved global reads may supply values but never write addresses",
          "[write_bounds_ir][global_read_write_bounds]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    const auto value = ir.LoadGlobal32(ir.Imm64(u64{0x8000}));
    fixture.GlobalStore(ir.IAdd(ir.Imm32(128), ir.IMul(ir.LocalInvocationIdX(), ir.Imm32(16))), 4);
    for (auto& inst : fixture.block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::WriteGlobal32) inst.SetArg(1, value);
    }
    fixture.Lower();
    fixture.program.info.uses_global_memory = true;
    REQUIRE(Extract(fixture.program, 256).size() == 1);
    CHECK_FALSE(Extract(fixture.program, 256)[0].complete);
    const auto proofs = Extract(fixture.program, 256, true);
    REQUIRE(proofs.size() == 1);
    REQUIRE(proofs[0].complete);
    REQUIRE(proofs[0].stores.size() == 1);
    const auto ranges = EvaluateProof(proofs[0], {{1,1,1}, {64,1,1}, true}, 4096, true);
    REQUIRE(ranges);
    for (u32 lane = 0; lane < 64; ++lane) {
        for (u32 bias = 0; bias < 256; ++bias) {
            CHECK(Covers(*ranges, (128 + lane * 16 + bias) & ~u64{3}, 4));
        }
    }
    const auto access = ExtractReadWrites(fixture.program, 256);
    REQUIRE(access.size() == 1);
    CHECK_FALSE(access[0].bounds.complete); // Never widens the sync-only policy.
}

TEST_CASE("Global-read write analysis rejects loaded offsets and ambiguous native routing",
          "[write_bounds_ir][global_read_write_bounds]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    SECTION("A global load used as the offset cannot become a constant") {
        fixture.GlobalStore(ir.LoadGlobal32(ir.Imm64(u64{0x8000})), 4);
        fixture.Lower();
    }
    SECTION("Runtime-routed native addresses still reject all proofs") {
        fixture.GlobalStore(ir.Imm32(128), 4);
        fixture.Lower();
        fixture.program.info.native_global_bounds = true;
    }
    SECTION("Global-store metadata remains a conservative veto") {
        fixture.GlobalStore(ir.Imm32(128), 4);
        fixture.Lower();
        fixture.program.info.stores_global_memory = true;
    }
    SECTION("Texture dimensions used as an address remain unknown") {
        const IR::Value dimensions{&*fixture.block.PrependNewInst(fixture.block.end(),
            IR::Opcode::ImageQueryDimensions, {ir.Imm32(0), ir.Imm32(0), ir.Imm1(false)})};
        fixture.GlobalStore(IR::U32{ir.CompositeExtract(dimensions, 0)}, 4);
        fixture.Lower();
    }
    fixture.program.info.uses_global_memory = true;
    const auto proofs = Extract(fixture.program, 256, true);
    REQUIRE(proofs.size() == 1);
    CHECK_FALSE(proofs[0].complete);
}

TEST_CASE("Whole-IR scan vetoes unaccounted side effects despite stale metadata",
          "[write_bounds_ir][global_read_write_bounds]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    fixture.GlobalStore(ir.Imm32(128), 4);
    fixture.Lower();
    fixture.program.info.uses_global_memory = true;
    // Deliberately leave stores_global_memory false: instructions must be checked too.
    SECTION("A later global store") {
        ir.WriteGlobal32(ir.Imm64(u64{0x8000}), ir.Imm32(7));
    }
    SECTION("A global atomic") {
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::GlobalAtomicIAdd32,
                                    {ir.Imm64(u64{0x8000}), ir.Imm32(1)});
    }
    SECTION("A private-memory writer is outside this initial allowlist") {
        ir.WriteLocal(ir.Imm32(0), ir.Imm32(7));
    }
    SECTION("A subword global read has unsupported backend semantics") {
        (void)ir.LoadGlobalU8(ir.Imm64(u64{0x8000}));
    }
    SECTION("An unsupported storage write width") {
        fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::WriteStorageU8,
                                    {ir.Imm32(0), ir.Imm32(128), ir.Imm32(7)});
    }
    SECTION("A writable image descriptor") {
        fixture.program.info.image_descriptors.emplace_back();
    }
    SECTION("An image write instruction with missing descriptor metadata") {
        ir.ImageWrite(ir.Imm32(0), ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)),
                      ir.CompositeConstruct(ir.Imm32(7), ir.Imm32(7), ir.Imm32(7), ir.Imm32(7)), {});
    }
    const auto proofs = Extract(fixture.program, 256, true);
    REQUIRE(proofs.size() == 1);
    CHECK_FALSE(proofs[0].complete);
}

TEST_CASE("Global-read proof allows barriers and scans non-entry blocks",
          "[write_bounds_ir][global_read_write_bounds]") {
    Fixture fixture;
    auto& ir = fixture.ir;
    fixture.GlobalStore(ir.Imm32(128), 4);
    fixture.Lower();
    (void)ir.LoadGlobal64(ir.Imm64(u64{0x8000}));
    (void)ir.LoadGlobal128(ir.Imm64(u64{0x9000}));
    fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::ImageQueryDimensions,
                                {ir.Imm32(0), ir.Imm32(0), ir.Imm1(false)});
    fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::ImageFetch,
        {ir.Imm32(0), ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)),
         IR::Value{}, ir.Imm32(0), IR::Value{}});
    fixture.block.PrependNewInst(fixture.block.end(), IR::Opcode::ImageSampleExplicitLod,
        {ir.Imm32(0), ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)),
         ir.Imm32(0.0f), IR::Value{}});
    ir.Barrier();
    ir.WorkgroupMemoryBarrier();
    ir.DeviceMemoryBarrier();
    fixture.program.info.uses_global_memory = true;
    REQUIRE(Extract(fixture.program, 256, true)[0].complete);
    IR::Block other{fixture.pool};
    IR::IREmitter other_ir{other};
    other_ir.WriteGlobal32(other_ir.Imm64(u64{0xa000}), other_ir.Imm32(7));
    fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(),
        IR::AbstractSyntaxNode{{.block = &other}, IR::AbstractSyntaxNode::Type::Block});
    CHECK_FALSE(Extract(fixture.program, 256, true)[0].complete);
}
