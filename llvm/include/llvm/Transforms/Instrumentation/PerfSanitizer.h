//===- PerfSanitizer.h - Performance suggestion pass ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass analyzes LLVM IR to identify missed optimization opportunities
// that the programmer could address through source-level changes. It emits
// optimization remarks sorted by estimated performance impact.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PERFSANITIZER_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PERFSANITIZER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;
class Module;

/// Function-level pass that analyzes IR for performance improvement
/// opportunities and emits optimization remarks.
struct PerfSanitizerPass : PassInfoMixin<PerfSanitizerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

/// Module-level pass that collects all PerfSanitizer remarks and emits
/// a sorted summary report (highest impact first).
struct PerfSanitizerReportPass : PassInfoMixin<PerfSanitizerReportPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_PERFSANITIZER_H
