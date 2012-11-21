//===-- ExternalDispatcher.cpp --------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/ExternalDispatcher.h"
#include "klee/Config/config.h"

// Ugh.
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#include "llvm/Module.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
#include "llvm/ModuleProvider.h"
#endif
#if !(LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
#include "llvm/LLVMContext.h"
#endif
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include <setjmp.h>
#include <signal.h>
#include <iostream>

using namespace llvm;
using namespace klee;

/***/

static jmp_buf escapeCallJmpBuf;

extern "C" {

#ifdef _WIN32
static void sigsegv_handler(int signal)
{
}
#else
static void sigsegv_handler(int signal, siginfo_t *info, void *context) {
  longjmp(escapeCallJmpBuf, 1);
}
#endif

}

void *ExternalDispatcher::resolveSymbol(const std::string &name) {
  assert(executionEngine);

  const char *str = name.c_str();

  // We use this to validate that function names can be resolved so we
  // need to match how the JIT does it. Unfortunately we can't
  // directly access the JIT resolution function
  // JIT::getPointerToNamedFunction so we emulate the important points.

  if (str[0] == 1) // asm specifier, skipped
    ++str;

  void *addr = sys::DynamicLibrary::SearchForAddressOfSymbol(str);
  if (addr)
    return addr;

  // If it has an asm specifier and starts with an underscore we retry
  // without the underscore. I (DWD) don't know why.
  if (name[0] == 1 && str[0]=='_') {
    ++str;
    addr = sys::DynamicLibrary::SearchForAddressOfSymbol(str);
  }

  return addr;
}

ExternalDispatcher::ExternalDispatcher(ExecutionEngine* engine) {
  dispatchModule = new Module("ExternalDispatcher", getGlobalContext());

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
  ExistingModuleProvider* MP = new ExistingModuleProvider(dispatchModule);
#endif

  originalEngine = engine;
  if(engine) {
    executionEngine = engine;
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
    executionEngine->addModuleProvider(MP);
#else
    executionEngine->addModule(dispatchModule);
#endif
  } else {
    std::string error;
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
    executionEngine = ExecutionEngine::createJIT(MP, &error);
#else
    executionEngine = ExecutionEngine::createJIT(dispatchModule, &error);
#endif
    if (!executionEngine) {
      std::cerr << "unable to make jit: " << error << "\n";
      abort();
    }

    // If we have a native target, initialize it to ensure it is linked in and
    // usable by the JIT.
    llvm::InitializeNativeTarget();
  }

  // from ExecutionEngine::create
  if (executionEngine) {
    // Make sure we can resolve symbols in the program as well. The zero arg
    // to the function tells DynamicLibrary to load the program, not a library.
    sys::DynamicLibrary::LoadLibraryPermanently(0);
  }

#ifdef WINDOWS
  preboundFunctions["getpid"] = (void*) (uintptr_t) getpid;
  preboundFunctions["putchar"] = (void*) (uintptr_t) putchar;
  preboundFunctions["printf"] = (void*) (uintptr_t) printf;
  preboundFunctions["fprintf"] = (void*) (uintptr_t) fprintf;
  preboundFunctions["sprintf"] = (void*) (uintptr_t) sprintf;
#endif
}

ExternalDispatcher::~ExternalDispatcher() {
  if(!originalEngine)
    delete executionEngine;
}

bool ExternalDispatcher::executeCall(Function *f, Instruction *i, uint64_t *args) {
  dispatchers_ty::iterator it = dispatchers.find(i);
  Function *dispatcher;

  if (it == dispatchers.end()) {
#ifdef WINDOWS
    std::map<std::string, void*>::iterator it2 =
      preboundFunctions.find(f->getName()));

    if (it2 != preboundFunctions.end()) {
      // only bind once
      if (it2->second) {
        executionEngine->addGlobalMapping(f, it2->second);
        it2->second = 0;
      }
    }
#endif

    dispatcher = createDispatcher(f,i);

    dispatchers.insert(std::make_pair(i, dispatcher));

    if (dispatcher) {
      // Force the JIT execution engine to go ahead and build the function. This
      // ensures that any errors or assertions in the compilation process will
      // trigger crashes instead of being caught as aborts in the external
      // function.
      executionEngine->recompileAndRelinkFunction(dispatcher);
    }
  } else {
    dispatcher = it->second;
  }

  return runProtectedCall(dispatcher, args);
}

// FIXME: This is not reentrant.
uint64_t *ExternalDispatcher::gTheArgsP;

bool ExternalDispatcher::runProtectedCall(Function *f, uint64_t *args) {
#ifndef _WIN32
  struct sigaction segvAction, segvActionOld;
#endif
  bool res;

  if (!f)
    return false;

  std::vector<GenericValue> gvArgs;
  gTheArgsP = args;

#ifdef _WIN32
  signal(SIGSEGV, ::sigsegv_handler);
#else
  segvAction.sa_handler = 0;
  memset(&segvAction.sa_mask, 0, sizeof(segvAction.sa_mask));
  segvAction.sa_flags = SA_SIGINFO;
  segvAction.sa_sigaction = ::sigsegv_handler;
  sigaction(SIGSEGV, &segvAction, &segvActionOld);
#endif

  if (setjmp(escapeCallJmpBuf)) {
    res = false;
  } else {

    executionEngine->runFunction(f, gvArgs);
    res = true;
  }

#ifdef _WIN32
#warning Implement more robust signal handling on windows
  signal(SIGSEGV, SIG_IGN);
#else
  sigaction(SIGSEGV, &segvActionOld, 0);
#endif
  return res;
}

// For performance purposes we construct the stub in such a way that the
// arguments pointer is passed through the static global variable gTheArgsP in
// this file. This is done so that the stub function prototype trivially matches
// the special cases that the JIT knows how to directly call. If this is not
// done, then the jit will end up generating a nullary stub just to call our
// stub, for every single function call.
Function *ExternalDispatcher::createDispatcher(Function *target, Instruction *inst) {
  if (!resolveSymbol(target->getName()))
    return 0;

  CallSite cs;
  if (inst->getOpcode()==Instruction::Call) {
    cs = CallSite(cast<CallInst>(inst));
  } else {
    cs = CallSite(cast<InvokeInst>(inst));
  }

  Value **args = new Value*[cs.arg_size()];

  std::vector<Type*> nullary;

  Function *dispatcher = Function::Create(FunctionType::get(Type::getVoidTy(getGlobalContext()),
                                ArrayRef<Type*>(nullary), false),
                      GlobalVariable::ExternalLinkage,
                      "",
                      dispatchModule);


  BasicBlock *dBB = BasicBlock::Create(getGlobalContext(), "entry", dispatcher);

  // Get a Value* for &gTheArgsP, as an i64**.
  Instruction *argI64sp =
    new IntToPtrInst(ConstantInt::get(Type::getInt64Ty(getGlobalContext()),
                                      (uintptr_t) (void*) &gTheArgsP),
                     PointerType::getUnqual(PointerType::getUnqual(Type::getInt64Ty(getGlobalContext()))),
                     "argsp", dBB);
  Instruction *argI64s = new LoadInst(argI64sp, "args", dBB);

  // Get the target function type.
  FunctionType *FTy =
    cast<FunctionType>(cast<PointerType>(target->getType())->getElementType());

  // Each argument will be passed by writing it into gTheArgsP[i].
  unsigned i = 0;
  for (CallSite::arg_iterator ai = cs.arg_begin(), ae = cs.arg_end();
       ai!=ae; ++ai, ++i) {
    // Determine the type the argument will be passed as. This accomodates for
    // the corresponding code in Executor.cpp for handling calls to bitcasted
    // functions.
    Type *argTy = (i < FTy->getNumParams() ? FTy->getParamType(i) :
                         (*ai)->getType());
    Instruction *argI64p =
      GetElementPtrInst::Create(argI64s,
                                ConstantInt::get(Type::getInt32Ty(getGlobalContext()),
                                                 i+1),
                                "", dBB);

    Instruction *argp = new BitCastInst(argI64p, PointerType::getUnqual(argTy),
                                        "", dBB);
    args[i] = new LoadInst(argp, "", dBB);
  }

  /////////////////////
  //S2E modification
  //The original KLEE code issued a call instruction to the external function
  //represented by a plain llvm::Function. The LLVM JIT would create a stub
  //for such a call. The stub and the JITed function (the one returned by this method)
  //must be close enough in memory because the JIT generates a machine call instruction
  //that uses a relative 32-bits displacement. Unfortunately, the default JIT memory
  //manager allocates blocks of code too far apart for a 32-bit value.
  //To solve this, we create an absolute call by casting the native pointer to
  //the helper to the type of that helper.

  uintptr_t targetFunctionAddress =
          (uintptr_t) llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(target->getName());

  assert(targetFunctionAddress && "External function not registered");


  Instruction *toPtr = new IntToPtrInst(
              ConstantInt::get(Type::getInt64Ty(dispatchModule->getContext()),
                               APInt(sizeof(targetFunctionAddress) * 8, targetFunctionAddress)),
              PointerType::get(Type::getInt64Ty(dispatchModule->getContext()), 0), "", dBB);

  Instruction *dispatchTarget = new BitCastInst(
        toPtr,
        cs.getCalledValue()->getType(), "", dBB);

  /////////////////////

#if 0
  //Original KLEE code for external function calling
  Instruction *dispatchTarget = new BitCastInst(
        dispatchModule->getOrInsertFunction(target->getName(), FTy,
                                        target->getAttributes()),
        cs.getCalledValue()->getType(), "", dBB);
#endif

  Instruction *result = CallInst::Create(dispatchTarget, ArrayRef<Value*>(args, cs.arg_size()), "", dBB);
  if (result->getType() != Type::getVoidTy(getGlobalContext())) {
    Instruction *resp =
      new BitCastInst(argI64s, PointerType::getUnqual(result->getType()),
                      "", dBB);
    new StoreInst(result, resp, dBB);
  }

  ReturnInst::Create(getGlobalContext(), dBB);

  delete[] args;

  return dispatcher;
}
