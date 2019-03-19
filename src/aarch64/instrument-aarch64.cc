// Copyright 2014, VIXL authors
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

#include "instrument-aarch64.h"

namespace vixl {
namespace aarch64 {

Counter::Counter(const char* name, CounterType type)
    : count_(0), enabled_(false), type_(type) {
  VIXL_ASSERT(name != NULL);
  strncpy(name_, name, kCounterNameMaxLength);
  // Make sure `name_` is always NULL-terminated, even if the source's length is
  // higher.
  name_[kCounterNameMaxLength - 1] = '\0';
}


void Counter::Enable() { enabled_ = true; }


void Counter::Disable() { enabled_ = false; }


bool Counter::IsEnabled() { return enabled_; }


void Counter::Increment() {
  if (enabled_) {
    count_++;
  }
}


uint64_t Counter::GetCount() {
  uint64_t result = count_;
  if (type_ == Gauge) {
    // If the counter is a Gauge, reset the count after reading.
    count_ = 0;
  }
  return result;
}


const char* Counter::GetName() { return name_; }


CounterType Counter::GetType() { return type_; }


struct CounterDescriptor {
  const char* name;
  CounterType type;
};


static const CounterDescriptor kCounterList[] =
    {{"Instruction", Cumulative},

     {"Move Immediate", Gauge},
     {"Add/Sub DP", Gauge},
     {"Logical DP", Gauge},
     {"Other Int DP", Gauge},
     {"FP DP", Gauge},

     {"Conditional Select", Gauge},
     {"Conditional Compare", Gauge},

     {"Unconditional Branch", Gauge},
     {"Compare and Branch", Gauge},
     {"Test and Branch", Gauge},
     {"Conditional Branch", Gauge},

     {"Load Integer", Gauge},
     {"Load FP", Gauge},
     {"Load Pair", Gauge},
     {"Load Literal", Gauge},

     {"Store Integer", Gauge},
     {"Store FP", Gauge},
     {"Store Pair", Gauge},

     {"PC Addressing", Gauge},
     {"Other", Gauge},
     {"NEON", Gauge},
     {"SVE", Gauge},
     {"Crypto", Gauge}};


Instrument::Instrument(const char* datafile, uint64_t sample_period)
    : output_stream_(stdout), sample_period_(sample_period) {
  // Set up the output stream. If datafile is non-NULL, use that file. If it
  // can't be opened, or datafile is NULL, use stdout.
  if (datafile != NULL) {
    output_stream_ = fopen(datafile, "w");
    if (output_stream_ == NULL) {
      printf("Can't open output file %s. Using stdout.\n", datafile);
      output_stream_ = stdout;
    }
  }

  static const int num_counters =
      sizeof(kCounterList) / sizeof(CounterDescriptor);

  // Dump an instrumentation description comment at the top of the file.
  fprintf(output_stream_, "# counters=%d\n", num_counters);
  fprintf(output_stream_, "# sample_period=%" PRIu64 "\n", sample_period_);

  // Construct Counter objects from counter description array.
  for (int i = 0; i < num_counters; i++) {
    Counter* counter = new Counter(kCounterList[i].name, kCounterList[i].type);
    counters_.push_back(counter);
  }

  DumpCounterNames();
}


Instrument::~Instrument() {
  // Dump any remaining instruction data to the output file.
  DumpCounters();

  // Free all the counter objects.
  std::list<Counter*>::iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    delete *it;
  }

  if (output_stream_ != stdout) {
    fclose(output_stream_);
  }
}


void Instrument::Update() {
  // Increment the instruction counter, and dump all counters if a sample period
  // has elapsed.
  static Counter* counter = GetCounter("Instruction");
  VIXL_ASSERT(counter->GetType() == Cumulative);
  counter->Increment();

  if ((sample_period_ != 0) && counter->IsEnabled() &&
      (counter->GetCount() % sample_period_) == 0) {
    DumpCounters();
  }
}


void Instrument::DumpCounters() {
  // Iterate through the counter objects, dumping their values to the output
  // stream.
  std::list<Counter*>::const_iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    fprintf(output_stream_, "%" PRIu64 ",", (*it)->GetCount());
  }
  fprintf(output_stream_, "\n");
  fflush(output_stream_);
}


void Instrument::DumpCounterNames() {
  // Iterate through the counter objects, dumping the counter names to the
  // output stream.
  std::list<Counter*>::const_iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    fprintf(output_stream_, "%s,", (*it)->GetName());
  }
  fprintf(output_stream_, "\n");
  fflush(output_stream_);
}


void Instrument::HandleInstrumentationEvent(unsigned event) {
  switch (event) {
    case InstrumentStateEnable:
      Enable();
      break;
    case InstrumentStateDisable:
      Disable();
      break;
    default:
      DumpEventMarker(event);
  }
}


void Instrument::DumpEventMarker(unsigned marker) {
  // Dumpan event marker to the output stream as a specially formatted comment
  // line.
  static Counter* counter = GetCounter("Instruction");

  fprintf(output_stream_,
          "# %c%c @ %" PRId64 "\n",
          marker & 0xff,
          (marker >> 8) & 0xff,
          counter->GetCount());
}


Counter* Instrument::GetCounter(const char* name) {
  // Get a Counter object by name from the counter list.
  std::list<Counter*>::const_iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    if (strcmp((*it)->GetName(), name) == 0) {
      return *it;
    }
  }

  // A Counter by that name does not exist: print an error message to stderr
  // and the output file, and exit.
  static const char* error_message =
      "# Error: Unknown counter \"%s\". Exiting.\n";
  fprintf(stderr, error_message, name);
  fprintf(output_stream_, error_message, name);
  exit(1);
}


void Instrument::Enable() {
  std::list<Counter*>::iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    (*it)->Enable();
  }
}


void Instrument::Disable() {
  std::list<Counter*>::iterator it;
  for (it = counters_.begin(); it != counters_.end(); it++) {
    (*it)->Disable();
  }
}


void Instrument::VisitPCRelAddressing(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("PC Addressing");
  counter->Increment();
}


void Instrument::VisitAddSubImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Add/Sub DP");
  counter->Increment();
}


void Instrument::VisitLogicalImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Logical DP");
  counter->Increment();
}


void Instrument::VisitMoveWideImmediate(const Instruction* instr) {
  Update();
  static Counter* counter = GetCounter("Move Immediate");

  if (instr->IsMovn() && (instr->GetRd() == kZeroRegCode)) {
    unsigned imm = instr->GetImmMoveWide();
    HandleInstrumentationEvent(imm);
  } else {
    counter->Increment();
  }
}


void Instrument::VisitBitfield(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other Int DP");
  counter->Increment();
}


void Instrument::VisitExtract(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other Int DP");
  counter->Increment();
}


void Instrument::VisitUnconditionalBranch(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Unconditional Branch");
  counter->Increment();
}


void Instrument::VisitUnconditionalBranchToRegister(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Unconditional Branch");
  counter->Increment();
}


void Instrument::VisitCompareBranch(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Compare and Branch");
  counter->Increment();
}


void Instrument::VisitTestBranch(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Test and Branch");
  counter->Increment();
}


void Instrument::VisitConditionalBranch(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Branch");
  counter->Increment();
}


void Instrument::VisitSystem(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitException(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::InstrumentLoadStorePair(const Instruction* instr) {
  static Counter* load_pair_counter = GetCounter("Load Pair");
  static Counter* store_pair_counter = GetCounter("Store Pair");

  if (instr->Mask(LoadStorePairLBit) != 0) {
    load_pair_counter->Increment();
  } else {
    store_pair_counter->Increment();
  }
}


void Instrument::VisitLoadStorePairPostIndex(const Instruction* instr) {
  Update();
  InstrumentLoadStorePair(instr);
}


void Instrument::VisitLoadStorePairOffset(const Instruction* instr) {
  Update();
  InstrumentLoadStorePair(instr);
}


void Instrument::VisitLoadStorePairPreIndex(const Instruction* instr) {
  Update();
  InstrumentLoadStorePair(instr);
}


void Instrument::VisitLoadStorePairNonTemporal(const Instruction* instr) {
  Update();
  InstrumentLoadStorePair(instr);
}


void Instrument::VisitLoadStoreExclusive(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitAtomicMemory(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitLoadLiteral(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Load Literal");
  counter->Increment();
}


void Instrument::VisitLoadStorePAC(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Load Integer");
  counter->Increment();
}


void Instrument::InstrumentLoadStore(const Instruction* instr) {
  static Counter* load_int_counter = GetCounter("Load Integer");
  static Counter* store_int_counter = GetCounter("Store Integer");
  static Counter* load_fp_counter = GetCounter("Load FP");
  static Counter* store_fp_counter = GetCounter("Store FP");

  switch (instr->Mask(LoadStoreMask)) {
    case STRB_w:
    case STRH_w:
    case STR_w:
      VIXL_FALLTHROUGH();
    case STR_x:
      store_int_counter->Increment();
      break;
    case STR_s:
      VIXL_FALLTHROUGH();
    case STR_d:
      store_fp_counter->Increment();
      break;
    case LDRB_w:
    case LDRH_w:
    case LDR_w:
    case LDR_x:
    case LDRSB_x:
    case LDRSH_x:
    case LDRSW_x:
    case LDRSB_w:
      VIXL_FALLTHROUGH();
    case LDRSH_w:
      load_int_counter->Increment();
      break;
    case LDR_s:
      VIXL_FALLTHROUGH();
    case LDR_d:
      load_fp_counter->Increment();
      break;
  }
}


void Instrument::VisitLoadStoreUnscaledOffset(const Instruction* instr) {
  Update();
  InstrumentLoadStore(instr);
}


void Instrument::VisitLoadStorePostIndex(const Instruction* instr) {
  USE(instr);
  Update();
  InstrumentLoadStore(instr);
}


void Instrument::VisitLoadStorePreIndex(const Instruction* instr) {
  Update();
  InstrumentLoadStore(instr);
}


void Instrument::VisitLoadStoreRegisterOffset(const Instruction* instr) {
  Update();
  InstrumentLoadStore(instr);
}

void Instrument::VisitLoadStoreRCpcUnscaledOffset(const Instruction* instr) {
  Update();
  switch (instr->Mask(LoadStoreRCpcUnscaledOffsetMask)) {
    case STLURB:
    case STLURH:
    case STLUR_w:
    case STLUR_x: {
      static Counter* counter = GetCounter("Store Integer");
      counter->Increment();
      break;
    }
    case LDAPURB:
    case LDAPURSB_w:
    case LDAPURSB_x:
    case LDAPURH:
    case LDAPURSH_w:
    case LDAPURSH_x:
    case LDAPUR_w:
    case LDAPURSW:
    case LDAPUR_x: {
      static Counter* counter = GetCounter("Load Integer");
      counter->Increment();
      break;
    }
  }
}


void Instrument::VisitLoadStoreUnsignedOffset(const Instruction* instr) {
  Update();
  InstrumentLoadStore(instr);
}


void Instrument::VisitLogicalShifted(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Logical DP");
  counter->Increment();
}


void Instrument::VisitAddSubShifted(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Add/Sub DP");
  counter->Increment();
}


void Instrument::VisitAddSubExtended(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Add/Sub DP");
  counter->Increment();
}


void Instrument::VisitAddSubWithCarry(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Add/Sub DP");
  counter->Increment();
}


void Instrument::VisitRotateRightIntoFlags(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitEvaluateIntoFlags(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitConditionalCompareRegister(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Compare");
  counter->Increment();
}


void Instrument::VisitConditionalCompareImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Compare");
  counter->Increment();
}


void Instrument::VisitConditionalSelect(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Select");
  counter->Increment();
}


void Instrument::VisitDataProcessing1Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other Int DP");
  counter->Increment();
}


void Instrument::VisitDataProcessing2Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other Int DP");
  counter->Increment();
}


void Instrument::VisitDataProcessing3Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other Int DP");
  counter->Increment();
}


void Instrument::VisitFPCompare(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPConditionalCompare(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Compare");
  counter->Increment();
}


void Instrument::VisitFPConditionalSelect(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Conditional Select");
  counter->Increment();
}


void Instrument::VisitFPImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPDataProcessing1Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPDataProcessing2Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPDataProcessing3Source(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPIntegerConvert(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitFPFixedPointConvert(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("FP DP");
  counter->Increment();
}


void Instrument::VisitCrypto2RegSHA(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Crypto");
  counter->Increment();
}


void Instrument::VisitCrypto3RegSHA(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Crypto");
  counter->Increment();
}


void Instrument::VisitCryptoAES(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Crypto");
  counter->Increment();
}


void Instrument::VisitNEON2RegMisc(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEON2RegMiscFP16(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEON3Same(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEON3SameFP16(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEON3SameExtra(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEON3Different(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONAcrossLanes(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONByIndexedElement(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONCopy(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONExtract(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONLoadStoreMultiStruct(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONLoadStoreMultiStructPostIndex(
    const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONLoadStoreSingleStruct(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONLoadStoreSingleStructPostIndex(
    const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONModifiedImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar2RegMisc(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar2RegMiscFP16(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar3Diff(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar3Same(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar3SameFP16(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalar3SameExtra(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalarByIndexedElement(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalarCopy(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalarPairwise(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONScalarShiftImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONShiftImmediate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONTable(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}


void Instrument::VisitNEONPerm(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("NEON");
  counter->Increment();
}

void Instrument::VisitSVEAddressGeneration(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEBitwiseImm(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEBitwiseLogicalUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEBitwiseShiftPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEBitwiseShiftUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEElementCount(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPAccumulatingReduction(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPArithmeticPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPArithmeticUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPCompareVectors(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPCompareWithZero(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPComplexAddition(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPComplexMulAdd(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPComplexMulAddIndex(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPFastReduction(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPMulIndex(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPMulAdd(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPMulAddIndex(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPUnaryOpPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEFPUnaryOpUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIncDecByPredicateCount(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIndexGeneration(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntArithmeticUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntBinaryArithmeticPredicated(
    const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntCompareScalars(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntCompareSignedImm(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntCompareUnsignedImm(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntCompareVectors(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntMiscUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntMulAddPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntMulAddUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntReduction(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntUnaryArithmeticPredicated(
    const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntWideImmPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEIntWideImmUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEMem32BitGatherAndUnsizedContiguous(
    const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEMem64BitGather(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEMemContiguousLoad(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEMemStore(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEMulIndex(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPartitionBreak(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPermutePredicate(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPermuteVectorExtract(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPermuteVectorInterleaving(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPermuteVectorPredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPermuteVectorUnpredicated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPredicateCount(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPredicateLogicalOp(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPredicateMisc(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEPropagateBreak(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEStackAllocation(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEVectorSelect(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitSVEWriteFFR(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("SVE");
  counter->Increment();
}

void Instrument::VisitReserved(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitUnallocated(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


void Instrument::VisitUnimplemented(const Instruction* instr) {
  USE(instr);
  Update();
  static Counter* counter = GetCounter("Other");
  counter->Increment();
}


}  // namespace aarch64
}  // namespace vixl
