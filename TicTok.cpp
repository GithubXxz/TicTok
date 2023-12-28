#include "TicTokCounter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <_types/_uint64_t.h>

using namespace llvm;

Constant *CreateGlobalCounter(Module &M, StringRef GlobalVarName) {
  auto &CTX = M.getContext();

  // This will insert a declaration into M
  Constant *NewGlobalVar =
      M.getOrInsertGlobal(GlobalVarName, IntegerType::getDoubleTy(CTX));

  // This will change the declaration into definition (and initialise to 0)
  GlobalVariable *NewGV = M.getNamedGlobal(GlobalVarName);
  NewGV->setLinkage(GlobalValue::CommonLinkage);
  NewGV->setAlignment(MaybeAlign(4));
  NewGV->setInitializer(llvm::ConstantFP::get(CTX, APFloat(0.0)));

  return NewGlobalVar;
}

//-----------------------------------------------------------------------------
// TicTokCounter implementation
//-----------------------------------------------------------------------------
bool TicTokCounter::runOnModule(Module &M) {
  bool Instrumented = false;

  // Function name <--> IR variable that holds the call counter
  llvm::StringMap<Constant *> CallCounterMap;
  // Function name <--> IR variable that holds the function name
  llvm::StringMap<Constant *> FuncNameMap;

  auto &CTX = M.getContext();

  FunctionType *ClockTy =
      FunctionType::get(IntegerType::getInt64Ty(CTX), {}, false);

  FunctionCallee Clock = M.getOrInsertFunction("clock", ClockTy);

  // STEP 1: For each function in the module, inject a call-counting code
  // --------------------------------------------------------------------
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    // Get an IR builder. Sets the insertion point to the top of the function
    IRBuilder<> Builder(&*F.getEntryBlock().getFirstInsertionPt());

    // Create a global variable to count the calls to this function
    std::string CounterName = "TimeFor_" + std::string(F.getName());
    Constant *Var = CreateGlobalCounter(M, CounterName);
    CallCounterMap[F.getName()] = Var;

    // Create a global variable to hold the name of this function
    Constant *FuncName = Builder.CreateGlobalStringPtr(F.getName());
    FuncNameMap[F.getName()] = FuncName;

    AllocaInst *ticCounterPtr = Builder.CreateAlloca(Type::getInt64Ty(CTX));
    CallInst *ticCounter = Builder.CreateCall(Clock);
    Builder.CreateStore(ticCounter, ticCounterPtr);

    for (auto &BB : F) {
      if (auto terminal = BB.getTerminator();
          terminal != nullptr && terminal->getOpcode() == Instruction::Ret) {
        Builder.SetInsertPoint(terminal);
        LoadInst *Begin =
            Builder.CreateLoad(IntegerType::getInt64Ty(CTX), ticCounterPtr);
        CallInst *End = Builder.CreateCall(Clock);
        Value *TimeUsed = Builder.CreateSub(End, Begin);
        Value *TimeUsedShift =
            Builder.CreateUIToFP(TimeUsed, Type::getDoubleTy(CTX));
        Value *TimeSeconds = Builder.CreateFDiv(
            TimeUsedShift, ConstantFP::get(CTX, APFloat(1000000.0)));

        LoadInst *CurFuncTotal =
            Builder.CreateLoad(Type::getDoubleTy(CTX), Var);
        Value *Added = Builder.CreateFAdd(CurFuncTotal, TimeSeconds);
        Builder.CreateStore(Added, Var);
      }
    }
    Instrumented = true;
  }

  // Stop here if there are no function definitions in this module
  if (false == Instrumented)
    return Instrumented;

  // STEP 2: Inject the declaration of printf
  // ----------------------------------------
  // Create (or _get_ in cases where it's already available) the following
  // declaration in the IR module:
  //    declare i32 @printf(i8*, ...)
  // It corresponds to the following C declaration:
  //    int printf(char *, ...)
  PointerType *PrintfArgTy = PointerType::getUnqual(Type::getInt8Ty(CTX));
  FunctionType *PrintfTy =
      FunctionType::get(IntegerType::getInt32Ty(CTX), PrintfArgTy,
                        /*IsVarArgs=*/true);

  FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfTy);

  // Set attributes as per inferLibFuncAttributes in BuildLibCalls.cpp
  Function *PrintfF = dyn_cast<Function>(Printf.getCallee());
  PrintfF->setDoesNotThrow();
  PrintfF->addParamAttr(0, Attribute::NoCapture);
  PrintfF->addParamAttr(0, Attribute::ReadOnly);

  // STEP 3: Inject a global variable that will hold the printf format string
  // ------------------------------------------------------------------------
  llvm::Constant *ResultFormatStr =
      llvm::ConstantDataArray::getString(CTX, "%-20s %-10f\n");

  Constant *ResultFormatStrVar =
      M.getOrInsertGlobal("ResultFormatStrIR", ResultFormatStr->getType());
  dyn_cast<GlobalVariable>(ResultFormatStrVar)->setInitializer(ResultFormatStr);

  std::string out = "";
  out += "=================================================\n";
  out += "TIC-TOK: get function waste time\n";
  out += "=================================================\n";
  out += "NAME                 #FUNC WASTE SECONDS\n";
  out += "-------------------------------------------------\n";

  llvm::Constant *ResultHeaderStr =
      llvm::ConstantDataArray::getString(CTX, out.c_str());

  Constant *ResultHeaderStrVar =
      M.getOrInsertGlobal("ResultHeaderStrIR", ResultHeaderStr->getType());
  dyn_cast<GlobalVariable>(ResultHeaderStrVar)->setInitializer(ResultHeaderStr);

  // STEP 4: Define a printf wrapper that will print the results
  // -----------------------------------------------------------
  // Define `printf_wrapper` that will print the results stored in FuncNameMap
  // and CallCounterMap.  It is equivalent to the following C++ function:
  // (item.name comes from FuncNameMap, item.count comes from
  // CallCounterMap)
  FunctionType *PrintfWrapperTy =
      FunctionType::get(llvm::Type::getVoidTy(CTX), {},
                        /*IsVarArgs=*/false);
  Function *PrintfWrapperF = dyn_cast<Function>(
      M.getOrInsertFunction("printf_wrapper", PrintfWrapperTy).getCallee());

  // Create the entry basic block for printf_wrapper ...
  llvm::BasicBlock *RetBlock =
      llvm::BasicBlock::Create(CTX, "enter", PrintfWrapperF);
  IRBuilder<> Builder(RetBlock);

  // ... and start inserting calls to printf
  // (printf requires i8*, so cast the input strings accordingly)
  llvm::Value *ResultHeaderStrPtr =
      Builder.CreatePointerCast(ResultHeaderStrVar, PrintfArgTy);
  llvm::Value *ResultFormatStrPtr =
      Builder.CreatePointerCast(ResultFormatStrVar, PrintfArgTy);

  Builder.CreateCall(Printf, {ResultHeaderStrPtr});

  LoadInst *LoadCounter;
  for (auto &item : CallCounterMap) {
    LoadCounter = Builder.CreateLoad(Type::getDoubleTy(CTX), item.second);
    // LoadCounter = Builder.CreateLoad(item.second);
    Builder.CreateCall(
        Printf, {ResultFormatStrPtr, FuncNameMap[item.first()], LoadCounter});
  }

  // Finally, insert return instruction
  Builder.CreateRetVoid();

  Function *MainFunction = M.getFunction("main");
  for (auto &BB : *MainFunction) {
    if (auto terminal = BB.getTerminator();
        terminal != nullptr && terminal->getOpcode() == Instruction::Ret) {
      // IRBuilder<> Builder(&*BB.getFirstInsertionPt());
      IRBuilder<> Builder(terminal);
      Builder.CreateCall(PrintfWrapperF);
    }
  }

  return true;
}

PreservedAnalyses TicTokCounter::run(llvm::Module &M,
                                     llvm::ModuleAnalysisManager &) {
  bool Changed = runOnModule(M);

  return (Changed ? llvm::PreservedAnalyses::none()
                  : llvm::PreservedAnalyses::all());
}

//-----------------------------------------------------------------------------
// New PM Registration
//-----------------------------------------------------------------------------
llvm::PassPluginLibraryInfo getTicTokCounterPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "tic-tok", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "tic-tok") {
                    MPM.addPass(TicTokCounter());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getTicTokCounterPluginInfo();
}
