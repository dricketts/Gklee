//===-- ExecutorUtil.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"

#include "Context.h"

#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/Solver.h"

#include "klee/Config/Version.h"
#include "klee/Internal/Module/KModule.h"

#include "klee/util/GetElementPtrTypeIterator.h"

#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
#include "llvm/ModuleProvider.h"
#endif
#include "llvm/Support/CallSite.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
#include "llvm/DataLayout.h"
#else
#include "llvm/Target/TargetData.h"
#endif
#include <iostream>
#include <cassert>

using namespace klee;
using namespace llvm;

namespace klee {

  klee::ref<ConstantExpr> Executor::evalConstantExpr(const llvm::ConstantExpr *ce) {
    LLVM_TYPE_Q llvm::Type *type = ce->getType();

    klee::ref<ConstantExpr> op1(0), op2(0), op3(0);
    int numOperands = ce->getNumOperands();

    if (numOperands > 0) { 
      op1 = evalConstant(ce->getOperand(0));
      //std::cout << "op1 value dump: " << std::endl;
      //ce->getOperand(0)->dump();
      op1->ctype = CUDAUtil::getCType(ce->getOperand(0), is_GPU_mode);
    }
   
    if (numOperands > 1) {
      op2 = evalConstant(ce->getOperand(1));
      //std::cout << "op2 value dump: " << std::endl;
      //ce->getOperand(1)->dump();
      op2->ctype = CUDAUtil::getCType(ce->getOperand(1), is_GPU_mode);
    }

    if (numOperands > 2) {
      op3 = evalConstant(ce->getOperand(2));
      //std::cout << "op3 value dump: " << std::endl;
      //ce->getOperand(2)->dump();
      op3->ctype = CUDAUtil::getCType(ce->getOperand(2), is_GPU_mode);
    }

    switch (ce->getOpcode()) {
    default :
      ce->dump();
      std::cerr << "error: unknown ConstantExpr type\n"
                << "opcode: " << ce->getOpcode() << "\n";
      abort();

    case Instruction::Trunc: 
      return op1->Extract(0, getWidthForLLVMType(type));
    case Instruction::ZExt:  return op1->ZExt(getWidthForLLVMType(type));
    case Instruction::SExt:  return op1->SExt(getWidthForLLVMType(type));
    case Instruction::Add:   return op1->Add(op2);
    case Instruction::Sub:   return op1->Sub(op2);
    case Instruction::Mul:   return op1->Mul(op2);
    case Instruction::SDiv:  return op1->SDiv(op2);
    case Instruction::UDiv:  return op1->UDiv(op2);
    case Instruction::SRem:  return op1->SRem(op2);
    case Instruction::URem:  return op1->URem(op2);
    case Instruction::And:   return op1->And(op2);
    case Instruction::Or:    return op1->Or(op2);
    case Instruction::Xor:   return op1->Xor(op2);
    case Instruction::Shl:   return op1->Shl(op2);
    case Instruction::LShr:  return op1->LShr(op2);
    case Instruction::AShr:  return op1->AShr(op2);
    case Instruction::BitCast: return op1;

    case Instruction::IntToPtr:
      return op1->ZExt(getWidthForLLVMType(type));

    case Instruction::PtrToInt:
      return op1->ZExt(getWidthForLLVMType(type));

    case Instruction::GetElementPtr: {

      klee::ref<ConstantExpr> base = op1->ZExt(Context::get().getPointerWidth());

      for (gep_type_iterator ii = gep_type_begin(ce), ie = gep_type_end(ce);
           ii != ie; ++ii) {
        klee::ref<ConstantExpr> addend = 
          ConstantExpr::alloc(0, Context::get().getPointerWidth());

        if (LLVM_TYPE_Q StructType *st = dyn_cast<StructType>(*ii)) {
          const StructLayout *sl = kmodule->targetData->getStructLayout(st);
          const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());

          addend = ConstantExpr::alloc(sl->getElementOffset((unsigned)
                                                            ci->getZExtValue()),
                                       Context::get().getPointerWidth());
        } else {
          const SequentialType *set = cast<SequentialType>(*ii);
          klee::ref<ConstantExpr> index = 
            evalConstant(cast<Constant>(ii.getOperand()));
          unsigned elementSize = 
            kmodule->targetData->getTypeStoreSize(set->getElementType());

          index = index->ZExt(Context::get().getPointerWidth());
          addend = index->Mul(ConstantExpr::alloc(elementSize, 
                                                  Context::get().getPointerWidth()));
        }

        base = base->Add(addend);
      }

      // by Guodong, set the ctype
      // memory type inference
      GPUConfig::CTYPE ctype = CUDAUtil::getCType(ce->getOperand(0), is_GPU_mode);
      base->ctype = ctype;

      return base;
    }
      
    case Instruction::ICmp: {
      switch(ce->getPredicate()) {
      default: assert(0 && "unhandled ICmp predicate");
      case ICmpInst::ICMP_EQ:  return op1->Eq(op2);
      case ICmpInst::ICMP_NE:  return op1->Ne(op2);
      case ICmpInst::ICMP_UGT: return op1->Ugt(op2);
      case ICmpInst::ICMP_UGE: return op1->Uge(op2);
      case ICmpInst::ICMP_ULT: return op1->Ult(op2);
      case ICmpInst::ICMP_ULE: return op1->Ule(op2);
      case ICmpInst::ICMP_SGT: return op1->Sgt(op2);
      case ICmpInst::ICMP_SGE: return op1->Sge(op2);
      case ICmpInst::ICMP_SLT: return op1->Slt(op2);
      case ICmpInst::ICMP_SLE: return op1->Sle(op2);
      }
    }

    case Instruction::Select:
      return op1->isTrue() ? op2 : op3;

    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FCmp:
      assert(0 && "floating point ConstantExprs unsupported");
      return 0;
    }
  }

}
