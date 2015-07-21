// Copyright 2015, ARM Limited
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

#include "examples.h"

#define BASE_BUF_SIZE (4096)
#define __ masm.

#ifdef USE_SIMULATOR
int64_t LiteralExample(int64_t a, int64_t b) {
  // Create and initialize the macro-assembler and the simulator.
  MacroAssembler masm(BASE_BUF_SIZE);
  Decoder decoder;
  Simulator simulator(&decoder);

  Literal<int64_t> automatically_placed_literal(111, masm.GetLiteralPool());
  Literal<int64_t> manually_placed_literal(222);

  // Generate some code.
  Label start;
  masm.Bind(&start);
  {
    CodeBufferCheckScope scope(&masm,
                               kInstructionSize + sizeof(int64_t),
                               CodeBufferCheckScope::kCheck,
                               CodeBufferCheckScope::kExactSize);
    Label over_literal;
    __ b(&over_literal);
    __ place(&manually_placed_literal);
    __ bind(&over_literal);
  }
  __ Ldr(x1, &manually_placed_literal);
  __ Ldr(x2, &automatically_placed_literal);
  __ Add(x0, x1, x2);
  __ Ret();

  masm.FinalizeCode();

  // Usually, compilers will move the code to another place in memory before
  // executing it. Emulate that.
  size_t code_size = masm.SizeOfCodeGenerated();
  uint8_t* code = reinterpret_cast<uint8_t*>(malloc(code_size));
  if (code == NULL) {
    return 1;
  }
  memcpy(code, masm.GetStartAddress<void*>(), code_size);

  // Run the code.
  simulator.RunFrom(masm.GetLabelAddress<Instruction*>(&start));
  printf("111 + 222 = %ld\n", simulator.xreg(0));

  // Now let's modify the values of the literals.
  automatically_placed_literal.UpdateValue(a, code);
  manually_placed_literal.UpdateValue(b, code);

  // Run the code again.
  simulator.RunFrom(reinterpret_cast<Instruction*>(code));
  printf("%" PRId64 " + %" PRId64 " = %" PRId64 "\n", a, b, simulator.xreg(0));

  return simulator.xreg(0);
}
#endif

#ifndef TEST_EXAMPLES
#ifdef USE_SIMULATOR
int main(void) {
  LiteralExample(1, 2);
  return 0;
}
#else
// Without the simulator there is nothing to test.
int main(void) { return 0; }
#endif  // USE_SIMULATOR
#endif  // TEST_EXAMPLES
