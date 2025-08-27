//===-- CanonPtr.h - Example Transformations ------------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_CANONPTR_H
#define LLVM_TRANSFORMS_UTILS_CANONPTR_H

#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class CanonPtrPass : public PassInfoMixin<CanonPtrPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }
private:
  bool shouldInstrument(Function &F);
  bool isVtableGep(GetElementPtrInst *Gep);
  void runOnFunc(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_CANONPTR_H