//===-- CanonPtr.cpp - Example Transformations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/CanonPtr.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/Utils/Local.h"
#include "llvm/IR/IntrinsicsX86.h"

using namespace llvm;

/*
 * Get the insert point after the specified instruction. For non-terminators
 * this is the next instruction. For `invoke` intructions, create a new
 * fallthrough block that jumps to the default return target, and return the
 * jump instruction.
 */
 static Instruction *getInsertPointAfter(Instruction *I) {
  if (InvokeInst *Invoke = dyn_cast<InvokeInst>(I)) {
      BasicBlock *Dst = Invoke->getNormalDest();
      BasicBlock *NewBlock = BasicBlock::Create(I->getContext(),
              "invoke_insert_point", Dst->getParent(), Dst);
      BranchInst *Br = BranchInst::Create(Dst, NewBlock);
      Invoke->setNormalDest(NewBlock);

      /* Patch references in PN nodes in original successor */
      BasicBlock::iterator It(Dst->begin());
      while (PHINode *PN = dyn_cast<PHINode>(It)) {
          int i;
          while ((i = PN->getBasicBlockIndex(Invoke->getParent())) >= 0)
              PN->setIncomingBlock(i, NewBlock);
          It++;
      }

      return Br;
  }

  if (isa<PHINode>(I))
      return &*I->getParent()->getFirstInsertionPt();

  assert(!I->isTerminator());
  return &*std::next(BasicBlock::iterator(I));
}

/*
* For function arguments, the insert point is in the entry basic block.
*/
static Instruction *getInsertPointAfter(Argument *A) {
  Function *F = A->getParent();
  assert(!F->empty());
  return &*F->getEntryBlock().getFirstInsertionPt();
}

static inline IntegerType *getPtrIntTy(LLVMContext &C) {
  return Type::getIntNTy(C, 64);
}

bool CanonPtrPass::shouldInstrument(Function &F){
  if (F.empty()) return false;
  if (F.isDeclaration()) return false;
  if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage) return false;
  if (F.getName().starts_with("__canonptr_")) return false;
  // Leave if the function doesn't need instrumentation.
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
    return false;
  return F.hasFnAttribute(llvm::Attribute::CanonPtr);
}

bool CanonPtrPass::isVtableGep(GetElementPtrInst *Gep) {
  Value *SrcPtr = Gep->getPointerOperand();
  if (SrcPtr->hasName() && SrcPtr->getName().starts_with("vtable")) {
      return true;
  }
  if (Gep->getNumIndices() == 1) {
      Value *FirstOp = Gep->getOperand(1);
      if (FirstOp->hasName() &&
          FirstOp->getName().starts_with("vbase.offset")) {
          return true;
      }
  }
  if(GlobalVariable *GV = dyn_cast<GlobalVariable>(SrcPtr)){
      if (GV->getName().starts_with("_ZTV")) {
          return true;
      }
  }
  return false;
}

void CanonPtrPass::runOnFunc(Function &F, FunctionAnalysisManager &AM){
  // errs() << "[CanonPtrPass] running on: " << F.getName() << "\n";

  Module *M = F.getParent();
  const DataLayout &DL = M->getDataLayout();

  SmallVector<GetElementPtrInst *, 16> GEPs;
  for (Instruction &I : instructions(F)){
    if(GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(&I)){
      GEPs.push_back(GEP);
    }
  }

  for(GetElementPtrInst* GEP : GEPs){
    errs() << "[CanonPtrPass] Looking at GEP: " << *GEP << "\n";
    // no effect on ptr
    if (GEP->hasAllZeroIndices()){
      continue;
    }

    // skip vtable
    if (isVtableGep(GEP)){
      continue;
    }

    /* TODO: we cannot support GEPs operating on vectors. */
    if (GEP->getType()->isVectorTy()) {
      continue;
    }

    std::string Prefix = GEP->hasName() ? GEP->getName().str() + "." : "";
    IRBuilder<> B(getInsertPointAfter(GEP));
    std::vector<User*> Users(GEP->user_begin(), GEP->user_end());
    IntegerType *PtrIntTy = getPtrIntTy(GEP->getContext());
    Instruction *PtrInt = cast<Instruction>(B.CreatePtrToInt(GEP, PtrIntTy, Prefix + "int"));

    uint64_t EnableBitSel = 48;

    // bits = ptr >> 48
    Value *UpperBits = B.CreateLShr(PtrInt, ConstantInt::get(PtrIntTy, EnableBitSel), Prefix + "upperbits");
    // sel = bits & 1
    Value *EnableSel = B.CreateAnd(UpperBits, ConstantInt::get(PtrIntTy, 1ULL), Prefix + "enable.sel");
    // (#1) enable = 0 - sel     (-sel)
    // Value *EnableBit = B.CreateSub(ConstantInt::get(PtrIntTy, 0ULL), EnableSel, Prefix + "enable.bit");
    // (#2) enable = -enable
    Value *EnableBit = B.CreateNeg(EnableSel, Prefix + "enable.bit");

    /* Generate calculation of offset (for every idx, multiply element size by
     * element idx, and add all together). IRBuilder does proper constant
     * folding on this, meaning that if the entire offset is known at compile
     * time, no calculation will be present in IR. */
    Value *Diff;
    ConstantInt *ConstOffset = nullptr;

    APInt ConstOffsetVal(64, 0);
    if (GEP->accumulateConstantOffset(DL, ConstOffsetVal))
        ConstOffset = B.getInt(ConstOffsetVal);

    if(ConstOffset){
      Diff = ConstOffset;
    }
    else{
      Diff = emitGEPOffset(&B, DL, GEP);
    }

    // shifted = diff << 49
    Value *Shifted = B.CreateShl(Diff, 49, Prefix + "shifted");

    // addoffset = shifted & enable
    Value *AddOffset = B.CreateAnd(Shifted, EnableBit);
    Value *PtrAdd = B.CreateAdd(PtrInt, AddOffset, Prefix + "added");
    Value *NewPtr = B.CreateIntToPtr(PtrAdd, GEP->getType(), Prefix + "newptr");

    for (User *U : Users)
      U->replaceUsesOfWith(GEP, NewPtr);
  }
}

PreservedAnalyses CanonPtrPass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  for (Function &F : M){
    if (shouldInstrument(F)){
      runOnFunc(F, FAM);
    }
  }
  return PreservedAnalyses::none();
}
