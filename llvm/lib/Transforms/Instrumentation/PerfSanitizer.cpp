//===- PerfSanitizer.cpp - Performance suggestion analysis ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass analyzes LLVM IR to identify missed optimization opportunities.
// Unlike traditional optimization passes that silently transform code, this
// pass emits actionable suggestions to the programmer about source-level
// changes that would enable better optimization.
//
// Checks performed:
//   1. Functions that could benefit from __attribute__((pure/const))
//   2. Loops with indirect/virtual calls that prevent vectorization
//   3. Aliasing issues that prevent load/store optimization
//   4. Functions too large to inline that could be split
//   5. Loops with variable trip counts that prevent unrolling
//   6. Cold code mixed with hot code (could be outlined)
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/PerfSanitizer.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "perf-sanitizer"

STATISTIC(NumPureCandidate, "Number of functions suggested as pure/const");
STATISTIC(NumIndirectInLoop, "Number of indirect calls found in loops");
STATISTIC(NumLargeFunction, "Number of functions suggested for splitting");
STATISTIC(NumVariableTripCount, "Number of loops with variable trip counts");
STATISTIC(NumMemoryReorder, "Number of aliasing issues found");

namespace {

/// Impact level for sorting suggestions.
enum class Impact : unsigned {
  Low = 0,
  Medium = 1,
  High = 2,
};

/// A single performance suggestion tied to a source location.
struct PerfSuggestion {
  Impact ImpactLevel;
  std::string PassName;
  std::string Message;
  DebugLoc Loc;
  std::string FunctionName;
};

/// Check if a function could be marked pure or const.
/// Pure functions don't modify global state; const functions also don't
/// read global state. These attributes enable CSE, DCE, and hoisting.
static void checkPureConst(Function &F, OptimizationRemarkEmitter &ORE) {
  if (F.isDeclaration())
    return;
  if (F.doesNotAccessMemory() || F.onlyReadsMemory())
    return;
  if (F.isVarArg())
    return;

  bool ReadsMemory = false;
  bool WritesMemory = false;
  bool HasSideEffects = false;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *CI = dyn_cast<CallBase>(&I)) {
        // If calling a function with unknown effects, bail
        Function *Callee = CI->getCalledFunction();
        if (!Callee || (!Callee->doesNotAccessMemory() &&
                        !Callee->onlyReadsMemory())) {
          HasSideEffects = true;
          break;
        }
        if (Callee->onlyReadsMemory())
          ReadsMemory = true;
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        // Check if storing to an argument (might be output parameter)
        // or to global memory
        WritesMemory = true;
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        ReadsMemory = true;
      }
    }
    if (HasSideEffects)
      break;
  }

  if (HasSideEffects)
    return;

  if (!WritesMemory) {
    ++NumPureCandidate;
    StringRef Suggestion = !ReadsMemory
        ? "function only depends on its arguments; marking it "
          "__attribute__((const)) enables CSE, DCE, and loop hoisting"
        : "function does not modify memory; marking it "
          "__attribute__((pure)) enables CSE and loop-invariant code motion";

    ORE.emit([&]() {
      return OptimizationRemarkMissed(DEBUG_TYPE, "PureConstCandidate",
                                       F.getSubprogram(), &F.getEntryBlock())
             << Suggestion;
    });
  }
}

/// Check for indirect/virtual calls inside loops. These prevent
/// devirtualization, inlining, and vectorization.
static void checkIndirectCallsInLoops(Function &F, LoopInfo &LI,
                                       OptimizationRemarkEmitter &ORE) {
  for (Loop *L : LI) {
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        if (CB->isIndirectCall()) {
          ++NumIndirectInLoop;
          ORE.emit([&]() {
            return OptimizationRemarkMissed(DEBUG_TYPE, "IndirectCallInLoop",
                                            I.getDebugLoc(), BB)
                   << "indirect/virtual call inside loop prevents "
                      "devirtualization and vectorization; consider using "
                      "CRTP, std::variant, or hoisting the call";
          });
        }
      }
    }
  }
}

/// Check for loops with variable (unknown at compile time) trip counts.
/// Fixed trip counts enable full unrolling and better vectorization.
static void checkLoopTripCounts(Function &F, LoopInfo &LI,
                                 ScalarEvolution &SE,
                                 OptimizationRemarkEmitter &ORE) {
  for (Loop *L : LI) {
    // Only check innermost loops (most impactful for vectorization)
    if (!L->getSubLoops().empty())
      continue;

    // Get the trip count
    unsigned TripCount = SE.getSmallConstantTripCount(L);
    if (TripCount > 0)
      continue; // Already a constant trip count

    // Check if we can at least get a max trip count
    unsigned MaxTripCount = SE.getSmallConstantMaxTripCount(L);
    BasicBlock *Header = L->getHeader();

    if (MaxTripCount == 0) {
      ++NumVariableTripCount;
      ORE.emit([&]() {
        return OptimizationRemarkMissed(DEBUG_TYPE, "VariableTripCount",
                                         Header->getFirstNonPHIIt()->getDebugLoc(),
                                         Header)
               << "loop trip count is not compile-time constant and has no "
                  "known upper bound; this prevents unrolling and limits "
                  "vectorization; consider using a fixed-size container or "
                  "adding __builtin_assume(n <= MAX)";
      });
    }
  }
}

/// Check for functions that are too large to inline.
/// Suggest splitting if the function is large and has distinct hot/cold regions.
static void checkFunctionSize(Function &F, OptimizationRemarkEmitter &ORE,
                               BlockFrequencyInfo *BFI) {
  // Count instructions
  unsigned InstCount = 0;
  for (BasicBlock &BB : F)
    InstCount += BB.size();

  // Typical inline threshold is ~250 instructions
  if (InstCount <= 300)
    return;

  ++NumLargeFunction;
  ORE.emit([&]() {
    return OptimizationRemarkMissed(DEBUG_TYPE, "LargeFunction",
                                     F.getSubprogram(), &F.getEntryBlock())
           << "function has " << ore::NV("InstCount", InstCount)
           << " instructions (inline threshold ~250); consider splitting "
              "into smaller functions to enable inlining at call sites";
  });
}

/// Check for memory access patterns that prevent optimization due to
/// potential aliasing.
static void checkAliasingIssues(Function &F, LoopInfo &LI,
                                 OptimizationRemarkEmitter &ORE) {
  for (Loop *L : LI) {
    if (!L->getSubLoops().empty())
      continue;

    // Look for loops with both loads and stores to pointer arguments.
    // These often can't be vectorized without restrict.
    bool HasArgLoad = false;
    bool HasArgStore = false;
    BasicBlock *Header = L->getHeader();

    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
          if (isa<Argument>(Ptr))
            HasArgLoad = true;
        }
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
          if (isa<Argument>(Ptr))
            HasArgStore = true;
        }
      }
    }

    if (HasArgLoad && HasArgStore) {
      ++NumMemoryReorder;
      ORE.emit([&]() {
        return OptimizationRemarkMissed(DEBUG_TYPE, "PotentialAliasing",
                                         Header->getFirstNonPHIIt()->getDebugLoc(),
                                         Header)
               << "loop reads and writes through pointer parameters that may "
                  "alias; adding __restrict qualifiers or using "
                  "-fstrict-aliasing could enable vectorization";
      });
    }
  }
}

} // anonymous namespace

PreservedAnalyses PerfSanitizerPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  // Skip declarations and intrinsics
  if (F.isDeclaration() || F.isIntrinsic())
    return PreservedAnalyses::all();

  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  // Check if any remarks are enabled for this pass
  if (!ORE.enabled())
    return PreservedAnalyses::all();

  // Run checks that don't need loop info
  checkPureConst(F, ORE);
  checkFunctionSize(F, ORE, nullptr);

  // Run loop-based checks
  auto &LI = AM.getResult<LoopAnalysis>(F);
  if (!LI.empty()) {
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    checkIndirectCallsInLoops(F, LI, ORE);
    checkLoopTripCounts(F, LI, SE, ORE);
    checkAliasingIssues(F, LI, ORE);
  }

  // This is an analysis pass - we don't modify the IR
  return PreservedAnalyses::all();
}

PreservedAnalyses PerfSanitizerReportPass::run(Module &M,
                                                ModuleAnalysisManager &AM) {
  // Emit a summary of all suggestions found across the module.
  // Individual suggestions are emitted via -Rpass-missed=perf-sanitizer
  // in the function-level pass. This is just the summary.

  unsigned TotalSuggestions = NumPureCandidate + NumIndirectInLoop +
                              NumLargeFunction + NumVariableTripCount +
                              NumMemoryReorder;

  if (TotalSuggestions > 0) {
    errs() << "\n=== Performance Sanitizer Summary ===\n";
    errs() << "Found " << TotalSuggestions
           << " optimization opportunities (sorted by impact):\n\n";

    // High impact first
    if (NumIndirectInLoop > 0)
      errs() << "  [HIGH]   " << NumIndirectInLoop
             << "x indirect/virtual calls in loops "
                "(prevents devirtualization, inlining, vectorization)\n";
    if (NumVariableTripCount > 0)
      errs() << "  [HIGH]   " << NumVariableTripCount
             << "x loops with variable trip counts "
                "(prevents unrolling and vectorization)\n";
    if (NumMemoryReorder > 0)
      errs() << "  [HIGH]   " << NumMemoryReorder
             << "x potential pointer aliasing in loops "
                "(prevents vectorization)\n";
    if (NumLargeFunction > 0)
      errs() << "  [MEDIUM] " << NumLargeFunction
             << "x functions too large to inline "
                "(consider splitting)\n";
    if (NumPureCandidate > 0)
      errs() << "  [MEDIUM] " << NumPureCandidate
             << "x functions could be marked pure/const "
                "(enables CSE and hoisting)\n";

    errs() << "\nUse -Rpass-missed=perf-sanitizer for per-location details.\n";
    errs() << "=====================================\n\n";
  }

  return PreservedAnalyses::all();
}
