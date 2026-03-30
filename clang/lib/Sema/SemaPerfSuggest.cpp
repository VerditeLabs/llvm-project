//===--- SemaPerfSuggest.cpp - Performance suggestion analysis ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements AST-based performance suggestion analysis. It walks
// function bodies and declarations looking for code patterns where programmer
// changes could enable better compiler optimizations.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/Type.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Sema.h"

using namespace clang;

namespace {

/// Threshold in bytes above which pass-by-value triggers a suggestion.
constexpr uint64_t PassByValueThreshold = 16;

/// Check if a function body is simple enough to be constexpr.
/// This is a conservative heuristic - it checks for obvious blockers.
static bool couldBeConstexpr(const FunctionDecl *FD, ASTContext &Ctx) {
  // Already constexpr/consteval
  if (FD->isConstexpr() || FD->isConsteval())
    return false;

  // Must have a body
  const Stmt *Body = FD->getBody();
  if (!Body)
    return false;

  // Skip main
  if (FD->isMain())
    return false;

  // Skip virtual functions - they can't be constexpr in practice for
  // polymorphic dispatch
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    if (MD->isVirtual())
      return false;
  }

  // Must have a literal or acceptable return type
  QualType RetTy = FD->getReturnType();
  if (!RetTy->isVoidType() && !RetTy->isLiteralType(Ctx))
    return false;

  // Check all parameters have literal types
  for (const ParmVarDecl *P : FD->parameters()) {
    QualType PT = P->getType().getNonReferenceType();
    if (!PT->isLiteralType(Ctx))
      return false;
  }

  // Simple heuristic: walk the body looking for blockers.
  // A real implementation would be more thorough, but this catches
  // the common easy cases.
  class ConstexprBlockerFinder : public RecursiveASTVisitor<ConstexprBlockerFinder> {
  public:
    bool HasBlocker = false;

    bool VisitCallExpr(CallExpr *CE) {
      if (const FunctionDecl *Callee = CE->getDirectCallee()) {
        // If calling a non-constexpr function, this can't be constexpr
        if (!Callee->isConstexpr())
          HasBlocker = true;
      } else {
        // Indirect call
        HasBlocker = true;
      }
      return !HasBlocker;
    }

    bool VisitCXXNewExpr(CXXNewExpr *) {
      HasBlocker = true;
      return false;
    }

    bool VisitCXXDeleteExpr(CXXDeleteExpr *) {
      HasBlocker = true;
      return false;
    }

    bool VisitAsmStmt(AsmStmt *) {
      HasBlocker = true;
      return false;
    }

    bool VisitCXXTryStmt(CXXTryStmt *) {
      HasBlocker = true;
      return false;
    }

    // Static local variables block constexpr
    bool VisitVarDecl(VarDecl *VD) {
      if (VD->isStaticLocal()) {
        HasBlocker = true;
        return false;
      }
      if (!VD->getType()->isLiteralType(VD->getASTContext())) {
        HasBlocker = true;
        return false;
      }
      return true;
    }

    // goto blocks constexpr
    bool VisitGotoStmt(GotoStmt *) {
      HasBlocker = true;
      return false;
    }

    // Labels block constexpr (pre-C++23)
    bool VisitLabelStmt(LabelStmt *) {
      HasBlocker = true;
      return false;
    }
  };

  ConstexprBlockerFinder Finder;
  Finder.TraverseStmt(const_cast<Stmt *>(Body));
  return !Finder.HasBlocker;
}

/// Check for pass-by-value of large types that should be const-ref.
static void checkPassByValue(Sema &S, const FunctionDecl *FD) {
  ASTContext &Ctx = S.getASTContext();
  for (const ParmVarDecl *P : FD->parameters()) {
    QualType T = P->getType();

    // Only interested in non-reference, non-pointer pass-by-value
    if (T->isReferenceType() || T->isPointerType())
      continue;

    // Skip trivially-copyable small types (int, float, pointers, etc.)
    if (T->isBuiltinType() || T->isEnumeralType())
      continue;

    // Must be a complete type
    if (T->isIncompleteType())
      continue;

    // Check size
    const auto *RT = T->getAs<RecordType>();
    if (!RT)
      continue;

    const RecordDecl *RD = RT->getDecl();
    if (!RD->isCompleteDefinition())
      continue;

    uint64_t Size = Ctx.getTypeSize(T) / 8; // bits to bytes
    if (Size > PassByValueThreshold) {
      S.Diag(P->getLocation(), diag::warn_perf_suggest_pass_by_ref)
          << P->getName() << T << Size;
      // High impact: copies in hot paths are expensive
      S.Diag(P->getLocation(), diag::note_perf_suggest_impact) << 1; // medium
    }
  }
}

/// Check if a for/while loop has a non-constant bound that prevents
/// unrolling/vectorization.
class LoopBoundChecker : public RecursiveASTVisitor<LoopBoundChecker> {
  Sema &S;

public:
  LoopBoundChecker(Sema &S) : S(S) {}

  bool VisitForStmt(ForStmt *FS) {
    checkLoopCondition(FS->getCond(), FS->getForLoc());
    return true;
  }

  bool VisitWhileStmt(WhileStmt *WS) {
    checkLoopCondition(WS->getCond(), WS->getWhileLoc());
    return true;
  }

private:
  void checkLoopCondition(const Expr *Cond, SourceLocation LoopLoc) {
    if (!Cond)
      return;

    // Look for comparisons like i < f() or i < *p or i < param
    const auto *BO = dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts());
    if (!BO)
      return;
    if (!BO->isRelationalOp() && !BO->isEqualityOp())
      return;

    // Check the RHS for problematic bound expressions
    const Expr *Bound = BO->getRHS()->IgnoreParenImpCasts();
    checkBoundExpr(Bound, LoopLoc);
  }

  void checkBoundExpr(const Expr *E, SourceLocation LoopLoc) {
    if (!E)
      return;

    // Function call as loop bound
    if (isa<CallExpr>(E)) {
      S.Diag(LoopLoc, diag::warn_perf_suggest_loop_fixed_bound) << 0;
      S.Diag(E->getExprLoc(), diag::note_perf_suggest_impact) << 2; // high
      return;
    }

    // Check for DeclRefExpr to a ParmVarDecl (variable parameter bound)
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (isa<ParmVarDecl>(DRE->getDecl())) {
        S.Diag(LoopLoc, diag::warn_perf_suggest_loop_fixed_bound) << 1;
        S.Diag(E->getExprLoc(), diag::note_perf_suggest_impact) << 1; // medium
        return;
      }
    }

    // Pointer dereference as bound
    if (isa<UnaryOperator>(E) &&
        cast<UnaryOperator>(E)->getOpcode() == UO_Deref) {
      S.Diag(LoopLoc, diag::warn_perf_suggest_loop_fixed_bound) << 3;
      S.Diag(E->getExprLoc(), diag::note_perf_suggest_impact) << 1; // medium
      return;
    }

    // Member expr that might be a method call (.size(), etc.)
    if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(E)) {
      S.Diag(LoopLoc, diag::warn_perf_suggest_loop_fixed_bound) << 0;
      S.Diag(MCE->getExprLoc(), diag::note_perf_suggest_impact) << 2; // high
      return;
    }
  }
};

/// Check for virtual function calls inside loop bodies.
class VirtualInLoopChecker : public RecursiveASTVisitor<VirtualInLoopChecker> {
  Sema &S;
  unsigned LoopDepth = 0;

public:
  VirtualInLoopChecker(Sema &S) : S(S) {}

  bool TraverseForStmt(ForStmt *FS) {
    ++LoopDepth;
    RecursiveASTVisitor::TraverseForStmt(FS);
    --LoopDepth;
    return true;
  }

  bool TraverseWhileStmt(WhileStmt *WS) {
    ++LoopDepth;
    RecursiveASTVisitor::TraverseWhileStmt(WS);
    --LoopDepth;
    return true;
  }

  bool TraverseCXXForRangeStmt(CXXForRangeStmt *RS) {
    ++LoopDepth;
    RecursiveASTVisitor::TraverseCXXForRangeStmt(RS);
    --LoopDepth;
    return true;
  }

  bool TraverseDoStmt(DoStmt *DS) {
    ++LoopDepth;
    RecursiveASTVisitor::TraverseDoStmt(DS);
    --LoopDepth;
    return true;
  }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *MCE) {
    if (LoopDepth == 0)
      return true;

    const CXXMethodDecl *MD = MCE->getMethodDecl();
    if (!MD || !MD->isVirtual())
      return true;

    S.Diag(MCE->getExprLoc(), diag::warn_perf_suggest_virtual_in_loop)
        << MD->getDeclName();
    S.Diag(MCE->getExprLoc(), diag::note_perf_suggest_impact) << 2; // high
    return true;
  }
};

/// Check if a function could benefit from noexcept.
static void checkNoexcept(Sema &S, const FunctionDecl *FD) {
  // Skip if already noexcept
  const auto *FPT = FD->getType()->getAs<FunctionProtoType>();
  if (!FPT || FPT->isNothrow())
    return;

  // Skip destructors (they're implicitly noexcept)
  if (isa<CXXDestructorDecl>(FD))
    return;

  // Only suggest for move constructors and move assignment operators
  // where noexcept has the biggest impact (enables move in containers)
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    bool IsMoveSpecial = false;
    if (const auto *CD = dyn_cast<CXXConstructorDecl>(MD))
      IsMoveSpecial = CD->isMoveConstructor();
    else
      IsMoveSpecial = MD->isMoveAssignmentOperator();

    if (IsMoveSpecial) {
      // Check if body contains throw statements
      class ThrowFinder : public RecursiveASTVisitor<ThrowFinder> {
      public:
        bool FoundThrow = false;
        bool VisitCXXThrowExpr(CXXThrowExpr *) {
          FoundThrow = true;
          return false;
        }
      };

      if (const Stmt *Body = FD->getBody()) {
        ThrowFinder TF;
        TF.TraverseStmt(const_cast<Stmt *>(Body));
        if (!TF.FoundThrow) {
          S.Diag(FD->getLocation(), diag::warn_perf_suggest_noexcept)
              << FD->getDeclName();
          S.Diag(FD->getLocation(), diag::note_perf_suggest_impact)
              << 2; // high
        }
      }
    }
  }
}

} // anonymous namespace

namespace clang {

void emitPerfSuggestions(Sema &S, const FunctionDecl *FD) {
  if (!FD || !FD->hasBody())
    return;

  DiagnosticsEngine &Diags = S.getDiagnostics();
  ASTContext &Ctx = S.getASTContext();

  // Skip system headers
  if (S.getSourceManager().isInSystemHeader(FD->getLocation()))
    return;

  // Skip template patterns (we'll check instantiations)
  if (FD->isDependentContext())
    return;

  // Check if any of our diagnostics are enabled
  bool AnyEnabled = false;
  AnyEnabled |= !Diags.isIgnored(diag::warn_perf_suggest_constexpr_function,
                                  FD->getLocation());
  AnyEnabled |= !Diags.isIgnored(diag::warn_perf_suggest_pass_by_ref,
                                  FD->getLocation());
  AnyEnabled |= !Diags.isIgnored(diag::warn_perf_suggest_loop_fixed_bound,
                                  FD->getLocation());
  AnyEnabled |= !Diags.isIgnored(diag::warn_perf_suggest_virtual_in_loop,
                                  FD->getLocation());
  AnyEnabled |= !Diags.isIgnored(diag::warn_perf_suggest_noexcept,
                                  FD->getLocation());
  if (!AnyEnabled)
    return;

  // 1. Check if function could be constexpr
  if (!Diags.isIgnored(diag::warn_perf_suggest_constexpr_function,
                        FD->getLocation())) {
    if (couldBeConstexpr(FD, Ctx)) {
      S.Diag(FD->getLocation(), diag::warn_perf_suggest_constexpr_function)
          << FD->getDeclName();
      S.Diag(FD->getLocation(), diag::note_perf_suggest_impact) << 1; // medium
    }
  }

  // 2. Check pass-by-value of large types
  if (!Diags.isIgnored(diag::warn_perf_suggest_pass_by_ref,
                        FD->getLocation())) {
    checkPassByValue(S, FD);
  }

  // 3. Check loop bounds
  if (!Diags.isIgnored(diag::warn_perf_suggest_loop_fixed_bound,
                        FD->getLocation())) {
    LoopBoundChecker LBC(S);
    LBC.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  }

  // 4. Check virtual calls in loops
  if (!Diags.isIgnored(diag::warn_perf_suggest_virtual_in_loop,
                        FD->getLocation())) {
    VirtualInLoopChecker VLC(S);
    VLC.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  }

  // 5. Check noexcept on move operations
  if (!Diags.isIgnored(diag::warn_perf_suggest_noexcept,
                        FD->getLocation())) {
    checkNoexcept(S, FD);
  }
}

} // namespace clang
