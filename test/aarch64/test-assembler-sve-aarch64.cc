// Copyright 2019, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <sys/mman.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "test-runner.h"
#include "test-utils.h"
#include "aarch64/test-utils-aarch64.h"

#include "aarch64/cpu-aarch64.h"
#include "aarch64/disasm-aarch64.h"
#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/simulator-aarch64.h"
#include "test-assembler-aarch64.h"

namespace vixl {
namespace aarch64 {

// Call masm->Insr repeatedly to allow test inputs to be set up concisely. This
// is optimised for call-site clarity, not generated code quality, so it doesn't
// exist in the MacroAssembler itself.
//
// Usage:
//
//    int values[] = { 42, 43, 44 };
//    InsrHelper(&masm, z0.VnS(), values);    // Sets z0.S = { ..., 42, 43, 44 }
//
// The rightmost (highest-indexed) array element maps to the lowest-numbered
// lane.
template <typename T, size_t N>
void InsrHelper(MacroAssembler* masm,
                const ZRegister& zdn,
                const T (&values)[N]) {
  for (size_t i = 0; i < N; i++) {
    masm->Insr(zdn, values[i]);
  }
}

// Conveniently initialise P registers. This is optimised for call-site clarity,
// not generated code quality.
//
// Usage:
//
//     int values[] = { 0x0, 0x1, 0x2 };
//     Initialise(&masm, p0.VnS(), values);  // Sets p0 = 0b'0000'0001'0010
//
// The rightmost (highest-indexed) array element maps to the lowest-numbered
// lane.
//
// Each element of the `values` array is mapped onto a lane in `pd`. The
// architecture only respects the lower bit, and writes zero the upper bits, but
// other (encodable) values can be specified if required by the test.
template <typename T, size_t N>
void Initialise(MacroAssembler* masm,
                const PRegisterWithLaneSize& pd,
                const T (&values)[N]) {
  UseScratchRegisterScope temps(masm);
  Register temp = temps.AcquireX();
  Label data;
  Label done;

  // There is no 'insr' for P registers. The easiest way to initialise one with
  // an arbitrary value is to load it from a literal pool.

  int p_bits_per_lane = pd.GetLaneSizeInBits() / kZRegBitsPerPRegBit;
  VIXL_ASSERT((N * p_bits_per_lane) <= kPRegMaxSize);
  uint64_t p_lane_mask = GetUintMask(p_bits_per_lane);

  // For most lane sizes, each value contributes less than a byte. We need to
  // pack them into chunks which we can store directly. It's sensible for the
  // chunk to be the same size as an instruction because we need to pad to an
  // instruction boundary anyway.
  typedef Instr Chunk;
  const size_t kChunkSizeInBits = sizeof(Chunk) * kBitsPerByte;
  VIXL_ASSERT((kPRegMaxSize % kChunkSizeInBits) == 0);
  const size_t kPRegMaxSizeInChunks = kPRegMaxSize / kChunkSizeInBits;

  masm->Adr(temp, &data);
  // TODO: Use `Ldr(pd, MemOperand(temp))` once available.
  masm->Ldr(PRegister(pd.GetCode()), temp);
  masm->B(&done);
  {
    ExactAssemblyScope total(masm, kPRegMaxSizeInBytes);
    masm->bind(&data);
    // Put the last-specified value at the lowest address.
    int values_index = N - 1;
    for (size_t c = 0; c < kPRegMaxSizeInChunks; c++) {
      Chunk chunk = 0;
      // Whilst we still have values left, use them to populate the chunk.
      for (size_t chunk_bit = 0;
           (chunk_bit < kChunkSizeInBits) && (values_index >= 0);
           chunk_bit += p_bits_per_lane) {
        Chunk value = values[values_index] & p_lane_mask;
        VIXL_ASSERT(static_cast<T>(value) == values[values_index]);
        chunk |= value << chunk_bit;
        values_index--;
      }
      masm->dc(chunk);
    }
  }
  masm->Bind(&done);
}

// Ensure that basic test infrastructure works.
TEST(sve_test_infrastructure_z) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  __ Mov(x0, 0x0123456789abcdef);

  // Test basic `Insr` behaviour.
  __ Insr(z0.VnB(), 1);
  __ Insr(z0.VnB(), 2);
  __ Insr(z0.VnB(), x0);
  __ Insr(z0.VnB(), -42);
  __ Insr(z0.VnB(), 0);

  // Test array inputs.
  int z1_inputs[] = {3, 4, 5, -42, 0};
  InsrHelper(&masm, z1.VnH(), z1_inputs);

  // Test that sign-extension works as intended for various lane sizes.
  __ Dup(z2.VnD(), 0);            // Clear the register first.
  __ Insr(z2.VnB(), -42);         //                       0xd6
  __ Insr(z2.VnB(), 0xfe);        //                       0xfe
  __ Insr(z2.VnH(), -42);         //                     0xffd6
  __ Insr(z2.VnH(), 0xfedc);      //                     0xfedc
  __ Insr(z2.VnS(), -42);         //                 0xffffffd6
  __ Insr(z2.VnS(), 0xfedcba98);  //                 0xfedcba98
  // Use another register for VnD(), so we can support 128-bit Z registers.
  __ Insr(z3.VnD(), -42);                 // 0xffffffffffffffd6
  __ Insr(z3.VnD(), 0xfedcba9876543210);  // 0xfedcba9876543210

  END();

  if (CAN_RUN()) {
    RUN();

    // Test that array checks work properly on a register initialised
    // lane-by-lane.
    int z0_inputs_b[] = {0x01, 0x02, 0xef, 0xd6, 0x00};
    ASSERT_EQUAL_SVE(z0_inputs_b, z0.VnB());

    // Test that lane-by-lane checks work properly on a register initialised
    // by array.
    for (size_t i = 0; i < ArrayLength(z1_inputs); i++) {
      // The rightmost (highest-indexed) array element maps to the
      // lowest-numbered lane.
      int lane = static_cast<int>(ArrayLength(z1_inputs) - i - 1);
      ASSERT_EQUAL_SVE_LANE(z1_inputs[i], z1.VnH(), lane);
    }

    uint64_t z2_inputs_d[] = {0x0000d6feffd6fedc, 0xffffffd6fedcba98};
    ASSERT_EQUAL_SVE(z2_inputs_d, z2.VnD());
    uint64_t z3_inputs_d[] = {0xffffffffffffffd6, 0xfedcba9876543210};
    ASSERT_EQUAL_SVE(z3_inputs_d, z3.VnD());
  }
}

// Ensure that basic test infrastructure works.
TEST(sve_test_infrastructure_p) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  // Simple cases: move boolean (0 or 1) values.

  int p0_inputs[] = {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0};
  Initialise(&masm, p0.VnB(), p0_inputs);

  int p1_inputs[] = {1, 0, 1, 1, 0, 1, 1, 1};
  Initialise(&masm, p1.VnH(), p1_inputs);

  int p2_inputs[] = {1, 1, 0, 1};
  Initialise(&masm, p2.VnS(), p2_inputs);

  int p3_inputs[] = {0, 1};
  Initialise(&masm, p3.VnD(), p3_inputs);

  // Advanced cases: move numeric value into architecturally-ignored bits.

  // B-sized lanes get one bit in a P register, so there are no ignored bits.

  // H-sized lanes get two bits in a P register.
  int p4_inputs[] = {0x3, 0x2, 0x1, 0x0, 0x1, 0x2, 0x3};
  Initialise(&masm, p4.VnH(), p4_inputs);

  // S-sized lanes get four bits in a P register.
  int p5_inputs[] = {0xc, 0x7, 0x9, 0x6, 0xf};
  Initialise(&masm, p5.VnS(), p5_inputs);

  // D-sized lanes get eight bits in a P register.
  int p6_inputs[] = {0x81, 0xcc, 0x55};
  Initialise(&masm, p6.VnD(), p6_inputs);

  // The largest possible P register has 32 bytes.
  int p7_inputs[] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                     0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
                     0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
                     0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
  Initialise(&masm, p7.VnD(), p7_inputs);

  END();

  if (CAN_RUN()) {
    RUN();

    // Test that lane-by-lane checks work properly. The rightmost
    // (highest-indexed) array element maps to the lowest-numbered lane.
    for (size_t i = 0; i < ArrayLength(p0_inputs); i++) {
      int lane = static_cast<int>(ArrayLength(p0_inputs) - i - 1);
      ASSERT_EQUAL_SVE_LANE(p0_inputs[i], p0.VnB(), lane);
    }
    for (size_t i = 0; i < ArrayLength(p1_inputs); i++) {
      int lane = static_cast<int>(ArrayLength(p1_inputs) - i - 1);
      ASSERT_EQUAL_SVE_LANE(p1_inputs[i], p1.VnH(), lane);
    }
    for (size_t i = 0; i < ArrayLength(p2_inputs); i++) {
      int lane = static_cast<int>(ArrayLength(p2_inputs) - i - 1);
      ASSERT_EQUAL_SVE_LANE(p2_inputs[i], p2.VnS(), lane);
    }
    for (size_t i = 0; i < ArrayLength(p3_inputs); i++) {
      int lane = static_cast<int>(ArrayLength(p3_inputs) - i - 1);
      ASSERT_EQUAL_SVE_LANE(p3_inputs[i], p3.VnD(), lane);
    }

    // Test that array checks work properly on predicates initialised with a
    // possibly-different lane size.
    // 0b...11'10'01'00'01'10'11
    int p4_expected[] = {0x39, 0x1b};
    ASSERT_EQUAL_SVE(p4_expected, p4.VnD());

    ASSERT_EQUAL_SVE(p5_inputs, p5.VnS());

    // 0b...10000001'11001100'01010101
    int p6_expected[] = {2, 0, 0, 1, 3, 0, 3, 0, 1, 1, 1, 1};
    ASSERT_EQUAL_SVE(p6_expected, p6.VnH());

    // 0b...10011100'10011101'10011110'10011111
    int p7_expected[] = {1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1,
                         1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1};
    ASSERT_EQUAL_SVE(p7_expected, p7.VnB());
  }
}

static void MlaMlsHelper(unsigned lane_size_in_bits) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  int zd_inputs[] = {0xbb, 0xcc, 0xdd, 0xee};
  int za_inputs[] = {-39, 1, -3, 2};
  int zn_inputs[] = {-5, -20, 9, 8};
  int zm_inputs[] = {9, -5, 4, 5};

  ZRegister zd = z0.WithLaneSize(lane_size_in_bits);
  ZRegister za = z1.WithLaneSize(lane_size_in_bits);
  ZRegister zn = z2.WithLaneSize(lane_size_in_bits);
  ZRegister zm = z3.WithLaneSize(lane_size_in_bits);

  // TODO: Use a simple `Dup` once it accepts arbitrary immediates.
  InsrHelper(&masm, zd, zd_inputs);
  InsrHelper(&masm, za, za_inputs);
  InsrHelper(&masm, zn, zn_inputs);
  InsrHelper(&masm, zm, zm_inputs);

  int p0_inputs[] = {1, 1, 0, 1};
  int p1_inputs[] = {1, 0, 1, 1};
  int p2_inputs[] = {0, 1, 1, 1};
  int p3_inputs[] = {1, 1, 1, 0};

  Initialise(&masm, p0.WithLaneSize(lane_size_in_bits), p0_inputs);
  Initialise(&masm, p1.WithLaneSize(lane_size_in_bits), p1_inputs);
  Initialise(&masm, p2.WithLaneSize(lane_size_in_bits), p2_inputs);
  Initialise(&masm, p3.WithLaneSize(lane_size_in_bits), p3_inputs);

  // The Mla macro automatically selects between mla, mad and movprfx + mla
  // based on what registers are aliased.
  ZRegister mla_da_result = z10.WithLaneSize(lane_size_in_bits);
  ZRegister mla_dn_result = z11.WithLaneSize(lane_size_in_bits);
  ZRegister mla_dm_result = z12.WithLaneSize(lane_size_in_bits);
  ZRegister mla_d_result = z13.WithLaneSize(lane_size_in_bits);

  __ Mov(mla_da_result, za);
  __ Mla(mla_da_result, p0.Merging(), mla_da_result, zn, zm);

  __ Mov(mla_dn_result, zn);
  __ Mla(mla_dn_result, p1.Merging(), za, mla_dn_result, zm);

  __ Mov(mla_dm_result, zm);
  __ Mla(mla_dm_result, p2.Merging(), za, zn, mla_dm_result);

  __ Mov(mla_d_result, zd);
  __ Mla(mla_d_result, p3.Merging(), za, zn, zm);

  // The Mls macro automatically selects between mls, msb and movprfx + mls
  // based on what registers are aliased.
  ZRegister mls_da_result = z20.WithLaneSize(lane_size_in_bits);
  ZRegister mls_dn_result = z21.WithLaneSize(lane_size_in_bits);
  ZRegister mls_dm_result = z22.WithLaneSize(lane_size_in_bits);
  ZRegister mls_d_result = z23.WithLaneSize(lane_size_in_bits);

  __ Mov(mls_da_result, za);
  __ Mls(mls_da_result, p0.Merging(), mls_da_result, zn, zm);

  __ Mov(mls_dn_result, zn);
  __ Mls(mls_dn_result, p1.Merging(), za, mls_dn_result, zm);

  __ Mov(mls_dm_result, zm);
  __ Mls(mls_dm_result, p2.Merging(), za, zn, mls_dm_result);

  __ Mov(mls_d_result, zd);
  __ Mls(mls_d_result, p3.Merging(), za, zn, zm);

  END();

  if (CAN_RUN()) {
    RUN();

    ASSERT_EQUAL_SVE(za_inputs, z1.WithLaneSize(lane_size_in_bits));
    ASSERT_EQUAL_SVE(zn_inputs, z2.WithLaneSize(lane_size_in_bits));
    ASSERT_EQUAL_SVE(zm_inputs, z3.WithLaneSize(lane_size_in_bits));

    int mla[] = {-84, 101, 33, 42};
    int mls[] = {6, -99, -39, -38};

    int mla_da_expected[] = {mla[0], mla[1], za_inputs[2], mla[3]};
    ASSERT_EQUAL_SVE(mla_da_expected, mla_da_result);

    int mla_dn_expected[] = {mla[0], zn_inputs[1], mla[2], mla[3]};
    ASSERT_EQUAL_SVE(mla_dn_expected, mla_dn_result);

    int mla_dm_expected[] = {zm_inputs[0], mla[1], mla[2], mla[3]};
    ASSERT_EQUAL_SVE(mla_dm_expected, mla_dm_result);

    int mla_d_expected[] = {mla[0], mla[1], mla[2], zd_inputs[3]};
    ASSERT_EQUAL_SVE(mla_d_expected, mla_d_result);

    int mls_da_expected[] = {mls[0], mls[1], za_inputs[2], mls[3]};
    ASSERT_EQUAL_SVE(mls_da_expected, mls_da_result);

    int mls_dn_expected[] = {mls[0], zn_inputs[1], mls[2], mls[3]};
    ASSERT_EQUAL_SVE(mls_dn_expected, mls_dn_result);

    int mls_dm_expected[] = {zm_inputs[0], mls[1], mls[2], mls[3]};
    ASSERT_EQUAL_SVE(mls_dm_expected, mls_dm_result);

    int mls_d_expected[] = {mls[0], mls[1], mls[2], zd_inputs[3]};
    ASSERT_EQUAL_SVE(mls_d_expected, mls_d_result);
  }
}

TEST(sve_mla_mls_b) { MlaMlsHelper(kBRegSize); }
TEST(sve_mla_mls_h) { MlaMlsHelper(kHRegSize); }
TEST(sve_mla_mls_s) { MlaMlsHelper(kSRegSize); }
TEST(sve_mla_mls_d) { MlaMlsHelper(kDRegSize); }

TEST(sve_bitwise_unpredicate_logical) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  uint64_t z8_inputs[] = {0xfedcba9876543210, 0x0123456789abcdef};
  InsrHelper(&masm, z8.VnD(), z8_inputs);
  uint64_t z15_inputs[] = {0xffffeeeeddddcccc, 0xccccddddeeeeffff};
  InsrHelper(&masm, z15.VnD(), z15_inputs);

  __ And(z1.VnD(), z8.VnD(), z15.VnD());
  __ Bic(z2.VnD(), z8.VnD(), z15.VnD());
  __ Eor(z3.VnD(), z8.VnD(), z15.VnD());
  __ Orr(z4.VnD(), z8.VnD(), z15.VnD());

  END();

  if (CAN_RUN()) {
    RUN();
    uint64_t z1_expected[] = {0xfedcaa8854540000, 0x0000454588aacdef};
    uint64_t z2_expected[] = {0x0000101022003210, 0x0123002201010000};
    uint64_t z3_expected[] = {0x01235476ab89fedc, 0xcdef98ba67453210};
    uint64_t z4_expected[] = {0xfffffefeffddfedc, 0xcdefddffefefffff};

    ASSERT_EQUAL_SVE(z1_expected, z1.VnD());
    ASSERT_EQUAL_SVE(z2_expected, z2.VnD());
    ASSERT_EQUAL_SVE(z3_expected, z3.VnD());
    ASSERT_EQUAL_SVE(z4_expected, z4.VnD());
  }
}

TEST(sve_predicate_logical) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  // 0b...01011010'10110111
  int p10_inputs[] = {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1};  // Pm
  // 0b...11011001'01010010
  int p11_inputs[] = {1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0};  // Pn
  // 0b...01010101'10110010
  int p12_inputs[] = {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0};  // pg

  Initialise(&masm, p10.VnB(), p10_inputs);
  Initialise(&masm, p11.VnB(), p11_inputs);
  Initialise(&masm, p12.VnB(), p12_inputs);

  __ Ands(p0.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Mrs(x0, NZCV);
  __ Bics(p1.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Mrs(x1, NZCV);
  __ Eor(p2.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Nand(p3.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Nor(p4.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Orn(p5.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Orr(p6.VnB(), p12.Zeroing(), p11.VnB(), p10.VnB());
  __ Sel(p7.VnB(), p12, p11.VnB(), p10.VnB());

  END();

  if (CAN_RUN()) {
    RUN();

    // 0b...01010000'00010010
    int p0_expected[] = {0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    // 0b...00000001'00000000
    int p1_expected[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    // 0b...00000001'10100000
    int p2_expected[] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0};
    // 0b...00000101'10100000
    int p3_expected[] = {0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0};
    // 0b...00000100'00000000
    int p4_expected[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // 0b...01010101'00010010
    int p5_expected[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0};
    // 0b...01010001'10110010
    int p6_expected[] = {0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0};
    // 0b...01011011'00010111
    int p7_expected[] = {0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1};

    ASSERT_EQUAL_SVE(p0_expected, p0.VnB());
    ASSERT_EQUAL_SVE(p1_expected, p1.VnB());
    ASSERT_EQUAL_SVE(p2_expected, p2.VnB());
    ASSERT_EQUAL_SVE(p3_expected, p3.VnB());
    ASSERT_EQUAL_SVE(p4_expected, p4.VnB());
    ASSERT_EQUAL_SVE(p5_expected, p5.VnB());
    ASSERT_EQUAL_SVE(p6_expected, p6.VnB());
    ASSERT_EQUAL_SVE(p7_expected, p7.VnB());


    // TODO: We will add the SVE semantic aliases at some point.
    ASSERT_EQUAL_32(NFlag, w0);  // First element is active.
    ASSERT_EQUAL_32(CFlag, w1);  // Last element is not active.
  }
}

TEST(sve_bitwise_imm) {
  SETUP_WITH_FEATURES(CPUFeatures::kSVE);
  START();

  // clang-format off
  uint64_t z21_inputs[] = {0xfedcba9876543210, 0x0123456789abcdef};
  uint32_t z22_inputs[] = {0xfedcba98, 0x76543210, 0x01234567, 0x89abcdef};
  uint16_t z23_inputs[] = {0xfedc, 0xba98, 0x7654, 0x3210,
                           0x0123, 0x4567, 0x89ab, 0xcdef};
  uint8_t z24_inputs[] = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
                          0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  // clang-format on

  InsrHelper(&masm, z1.VnD(), z21_inputs);
  InsrHelper(&masm, z2.VnS(), z22_inputs);
  InsrHelper(&masm, z3.VnH(), z23_inputs);
  InsrHelper(&masm, z4.VnB(), z24_inputs);

  __ And(z1.VnD(), z1.VnD(), 0x0000ffff0000ffff);
  __ And(z2.VnS(), z2.VnS(), 0xff0000ff);
  __ And(z3.VnH(), z3.VnH(), 0x0ff0);
  __ And(z4.VnB(), z4.VnB(), 0x3f);

  InsrHelper(&masm, z5.VnD(), z21_inputs);
  InsrHelper(&masm, z6.VnS(), z22_inputs);
  InsrHelper(&masm, z7.VnH(), z23_inputs);
  InsrHelper(&masm, z8.VnB(), z24_inputs);

  __ Eor(z5.VnD(), z5.VnD(), 0x0000ffff0000ffff);
  __ Eor(z6.VnS(), z6.VnS(), 0xff0000ff);
  __ Eor(z7.VnH(), z7.VnH(), 0x0ff0);
  __ Eor(z8.VnB(), z8.VnB(), 0x3f);

  InsrHelper(&masm, z9.VnD(), z21_inputs);
  InsrHelper(&masm, z10.VnS(), z22_inputs);
  InsrHelper(&masm, z11.VnH(), z23_inputs);
  InsrHelper(&masm, z12.VnB(), z24_inputs);

  __ Orr(z9.VnD(), z9.VnD(), 0x0000ffff0000ffff);
  __ Orr(z10.VnS(), z10.VnS(), 0xff0000ff);
  __ Orr(z11.VnH(), z11.VnH(), 0x0ff0);
  __ Orr(z12.VnB(), z12.VnB(), 0x3f);

  __ Dup(z13.VnD(), 0x7ffffff800000000);
  __ Dup(z14.VnS(), 0x7ffc7ffc);
  __ Dup(z15.VnH(), 0x3ffc);
  // TODO: Enable once dup or mov is implemented.
  // __ Dup(z16.VnB(), 0xc3);

  END();

  if (CAN_RUN()) {
    RUN();

    // clang-format off
    uint64_t z1_expected[] = {0x0000ba9800003210, 0x000045670000cdef};
    uint32_t z2_expected[] = {0xfe000098, 0x76000010, 0x01000067, 0x890000ef};
    uint16_t z3_expected[] = {0x0ed0, 0x0a90, 0x0650, 0x0210,
                              0x0120, 0x0560, 0x09a0, 0x0de0};
    uint8_t z4_expected[] = {0x3e, 0x1c, 0x3a, 0x18, 0x36, 0x14, 0x32, 0x10,
                             0x01, 0x23, 0x05, 0x27, 0x09, 0x2b, 0x0d, 0x2f};

    ASSERT_EQUAL_SVE(z1_expected, z1.VnD());
    ASSERT_EQUAL_SVE(z2_expected, z2.VnS());
    ASSERT_EQUAL_SVE(z3_expected, z3.VnH());
    ASSERT_EQUAL_SVE(z4_expected, z4.VnB());

    uint64_t z5_expected[] = {0xfedc45677654cdef, 0x0123ba9889ab3210};
    uint32_t z6_expected[] = {0x01dcba67, 0x895432ef, 0xfe234598, 0x76abcd10};
    uint16_t z7_expected[] = {0xf12c, 0xb568, 0x79a4, 0x3de0,
                              0x0ed3, 0x4a97, 0x865b, 0xc21f};
    uint8_t z8_expected[] = {0xc1, 0xe3, 0x85, 0xa7, 0x49, 0x6b, 0x0d, 0x2f,
                             0x3e, 0x1c, 0x7a, 0x58, 0xb6, 0x94, 0xf2, 0xd0};

    ASSERT_EQUAL_SVE(z5_expected, z5.VnD());
    ASSERT_EQUAL_SVE(z6_expected, z6.VnS());
    ASSERT_EQUAL_SVE(z7_expected, z7.VnH());
    ASSERT_EQUAL_SVE(z8_expected, z8.VnB());

    uint64_t z9_expected[] = {0xfedcffff7654ffff, 0x0123ffff89abffff};
    uint32_t z10_expected[] = {0xffdcbaff, 0xff5432ff,  0xff2345ff, 0xffabcdff};
    uint16_t z11_expected[] = {0xfffc, 0xbff8, 0x7ff4, 0x3ff0,
                               0x0ff3, 0x4ff7, 0x8ffb, 0xcfff};
    uint8_t z12_expected[] = {0xff, 0xff, 0xbf, 0xbf, 0x7f, 0x7f, 0x3f, 0x3f,
                              0x3f, 0x3f, 0x7f, 0x7f, 0xbf, 0xbf, 0xff, 0xff};

    ASSERT_EQUAL_SVE(z9_expected, z9.VnD());
    ASSERT_EQUAL_SVE(z10_expected, z10.VnS());
    ASSERT_EQUAL_SVE(z11_expected, z11.VnH());
    ASSERT_EQUAL_SVE(z12_expected, z12.VnB());

    uint64_t z13_expected[] = {0x7ffffff800000000, 0x7ffffff800000000};
    uint32_t z14_expected[] = {0x7ffc7ffc, 0x7ffc7ffc, 0x7ffc7ffc, 0x7ffc7ffc};
    uint16_t z15_expected[] = {0x3ffc, 0x3ffc, 0x3ffc, 0x3ffc,
                               0x3ffc, 0x3ffc, 0x3ffc ,0x3ffc};
    ASSERT_EQUAL_SVE(z13_expected, z13.VnD());
    ASSERT_EQUAL_SVE(z14_expected, z14.VnS());
    ASSERT_EQUAL_SVE(z15_expected, z15.VnH());
    // clang-format on
  }
}

}  // namespace aarch64
}  // namespace vixl
