//=== StructureReplacement.cpp Source-level Replacement of Structures C++ *===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// The file declares a pass to perform replacement of fields of structures with
// separate variables.
//
// The replacement of function parameters are possible only.
// Type of a parameter to replace must be a pointer to some record type.
//===----------------------------------------------------------------------===//

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/IncludeTree.h"
#include "tsar/Analysis/Clang/NoMacroAssert.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/Clang/Pragma.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/Analysis/CallGraph.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tsar;
using namespace clang;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-struct-replacement"

namespace {
inline const clang::Type *getCanonicalUnqualifiedType(ValueDecl *VD) {
  return VD->getType()
      .getTypePtr()
      ->getCanonicalTypeUnqualified()
      ->getTypePtr();
}

struct Replacement {
  Replacement(ValueDecl *M) : Member(M) {}

  /// Member, this replacement belongs to, of a parameter which should be
  /// replaced.
  ValueDecl *Member;

  /// Locations in a source code which contains accesses to the member 'Member'
  /// of an original parameter.
  std::vector<SourceRange> Ranges;

  /// Identifier of a new parameter which corresponds to the member 'Member' of
  /// an original parameter which should be replaced.
  SmallString<32> Identifier;

  /// This is 'true' if a value of the member 'Member' of an original parameter
  /// can be changed in the original function call.
  bool InAssignment = false;
};

/// Map from parameter to its replacement which is list of necessary members,
/// replacement string and range that must be changed with this string.
using ReplacementCandidates = SmallDenseMap<
    NamedDecl *,
    std::tuple<SmallVector<Replacement, 8>, std::string, SourceRange>, 8,
    DenseMapInfo<NamedDecl *>,
    TaggedDenseMapTuple<bcl::tagged<NamedDecl *, NamedDecl>,
                        bcl::tagged<SmallVector<Replacement, 8>, Replacement>,
                        bcl::tagged<std::string, std::string>,
                        bcl::tagged<SourceRange, SourceRange>>>;

/// Description of a possible replacement of a source function.
struct ReplacementMetadata {
  struct ParamReplacement {
    Optional<unsigned> TargetParam;
    FieldDecl *TargetMember = nullptr;
    bool IsPointer = false;
  };

  bool valid(unsigned *ParamIdx = nullptr) const {
    if (!TargetDecl) {
      if (ParamIdx)
        *ParamIdx = Parameters.size();
      return false;
    }
    for (unsigned I = 0, EI = Parameters.size(); I < EI; ++I)
      if (!Parameters[I].TargetParam) {
        if (ParamIdx)
          *ParamIdx = I;
        return false;
      }
    return true;
  }

  /// Declaration of a function which can be replaced with a current one.
  CanonicalDeclPtr<FunctionDecl> TargetDecl = nullptr;

  /// Correspondence between parameters of this function and the target
  /// 'TargetDecl' of a call replacement.
  SmallVector<ParamReplacement, 8> Parameters;
};

/// List of original functions for a clone.
using ReplacementTargets = SmallVector<ReplacementMetadata, 1>;

/// Map from calls that should be replaced to functions which should be used
/// instead of callee.
using ReplacementRequests =
    DenseMap<clang::CallExpr *,
             std::tuple<clang::FunctionDecl *, clang::SourceLocation>,
             DenseMapInfo<clang::CallExpr *>,
             TaggedDenseMapTuple<
                 bcl::tagged<clang::CallExpr *, clang::CallExpr>,
                 bcl::tagged<clang::FunctionDecl *, clang::FunctionDecl>,
                 bcl::tagged<clang::SourceLocation, clang::SourceLocation>>>;

/// Set of calls that should be implicitly requested due to accesses to
/// replace candidates.
using ReplacementImplicitRequests = DenseSet<clang::CallExpr *>;

struct FunctionInfo {
  FunctionInfo(FunctionDecl *FD) {
    if (FD->doesThisDeclarationHaveABody()) {
      Definition = FD;
    } else {
      const FunctionDecl *D = FD->getFirstDecl();
      if (D->hasBody(D))
        Definition = const_cast<FunctionDecl *>(D);
    }
    assert(
        Definition &&
        "FunctionInfo can be created for a function with a known body only!");
  }

  /// Function redeclaration which has a body.
  FunctionDecl *Definition = nullptr;

  /// List of parameters of this function, which are specified in 'replace'
  /// clause, which should be replaced.
  ReplacementCandidates Candidates;

  /// List of calls from this function, which are marked with a 'with' clause,
  /// which should be replaced.
  ReplacementRequests Requests;

  /// List of calls that should be implicitly requested due to accesses to
  /// replace candidates.
  ReplacementImplicitRequests ImplicitRequests;

  /// Calls to functions from this list can be replaced with this function.
  ReplacementTargets Targets;

  /// Source ranges which correspond to transformation clauses and which
  /// can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveTransform;

  /// Source ranges which corresponds to metadata clauses
  /// which can be successfully removed.
  SmallVector<CharSourceRange, 8> ToRemoveMetadata;

  /// Source ranges which have to be removed from the clone only.
  SmallVector<CharSourceRange, 8> ToRemoveClone;

  /// Name of a new function which is a result of replacement.
  SmallString<32> ReplacmenetName;

  SmallPtrSet<DeclRefExpr *, 8> Meta;
  bool Strict = false;

  /// Return true if there is no replacement-related information available.
  bool empty() const {
    return Candidates.empty() && Requests.empty() && Targets.empty();
  }

  /// Return true if at least one replacement candidate has been found.
  bool hasCandidates() const { return !Candidates.empty(); }

  /// Return true if at least on function call inside a current function should
  /// be replaced.
  bool hasRequests() const { return !Requests.empty(); }

  /// Return true if a specified reference is located in a 'replace' clause.
  bool inClause(const DeclRefExpr *DRE) const { return Meta.count(DRE); }
};

using ReplacementMap =
    DenseMap<CanonicalDeclPtr<FunctionDecl>, std::unique_ptr<FunctionInfo>,
             DenseMapInfo<CanonicalDeclPtr<FunctionDecl>>,
             TaggedDenseMapPair<
                 bcl::tagged<CanonicalDeclPtr<FunctionDecl>, FunctionDecl>,
                 bcl::tagged<std::unique_ptr<FunctionInfo>, FunctionInfo>>>;


clang::DeclRefExpr *getCandidate(clang::Expr *ArgExpr) {
  if (auto *Cast = dyn_cast<ImplicitCastExpr>(ArgExpr))
    if (Cast->getCastKind() == CK_LValueToRValue)
      ArgExpr = Cast->getSubExpr();
  return dyn_cast<DeclRefExpr>(ArgExpr);
}

const clang::DeclRefExpr *getCandidate(const clang::Expr *ArgExpr) {
  if (auto *Cast = dyn_cast<ImplicitCastExpr>(ArgExpr))
    if (Cast->getCastKind() == CK_LValueToRValue)
      ArgExpr = Cast->getSubExpr();
  return dyn_cast<DeclRefExpr>(ArgExpr);
}

ReplacementCandidates::iterator isExprInCandidates(const clang::Expr *ArgExpr,
    ReplacementCandidates &Candidates) {
  if (auto *DRE = getCandidate(ArgExpr))
    return Candidates.find(DRE->getFoundDecl());
  return Candidates.end();
}

using CallList = std::vector<clang::CallExpr *>;

/// This class collects all 'replace' clauses in the code.
class ReplacementCollector : public RecursiveASTVisitor<ReplacementCollector> {
public:
  ReplacementCollector(TransformationContext &TfmCtx,
      ReplacementMap &Replacements, CallList &Calls)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(TfmCtx.getContext().getSourceManager())
     , mLangOpts(TfmCtx.getContext().getLangOpts())
     , mReplacements(Replacements)
     , mCalls(Calls)
  {}

  /// Return list of parameters to replace.
  ReplacementMap & getReplacementInfo() noexcept {
    return mReplacements;
  }

  /// Return list of parameters to replace.
  const ReplacementMap & getReplacementInfo() const noexcept {
    return mReplacements;
  }

  /// Return list of visited call expressions.
  CallList & getCalls() noexcept {
    return mCalls;
  }

  /// Return list of visited call expressions.
  const CallList & getCalls() const noexcept {
    return mCalls;
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    Pragma P(*S);
    SmallVector<Stmt *, 2> Clauses;
    if (findClause(P, ClauseId::Replace, Clauses)) {
      auto ReplaceSize = Clauses.size();
      findClause(P, ClauseId::With, Clauses);
      auto StashSize = Clauses.size();
      mCurrFunc->Strict |= !findClause(P, ClauseId::NoStrict, Clauses);
      // Do not remove 'nostrict' clause if the directive contains other
      // clauses except 'replace'.
      if (P.clause_size() > Clauses.size())
        Clauses.resize(StashSize);
      auto IsPossible = pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
        mCurrFunc->ToRemoveTransform, PragmaFlags::IsInHeader);
      if (!IsPossible.first)
        if (IsPossible.second & PragmaFlags::IsInMacro)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive_in_macro);
        else if (IsPossible.second & PragmaFlags::IsInHeader)
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive_in_include);
        else
          toDiag(mSrcMgr.getDiagnostics(), Clauses.front()->getLocStart(),
            diag::warn_remove_directive);
      mInClause = ClauseId::Replace;
      Clauses.resize(StashSize);
      auto I = Clauses.begin(), EI = Clauses.end();
      for (auto ReplaceEI = I + ReplaceSize; I < ReplaceEI; ++I) {
        mCurrClauseBeginLoc = (**I).getBeginLoc();
        if (!RecursiveASTVisitor::TraverseStmt(*I))
          break;
      }
      mInClause = ClauseId::With;
      for (; I < EI; ++I) {
        mCurrClauseBeginLoc = (**I).getBeginLoc();
        if (!RecursiveASTVisitor::TraverseStmt(*I))
          break;
      }
      mInClause = ClauseId::NotClause;
      return true;
    }
    if (findClause(P, ClauseId::ReplaceMetadata, Clauses)) {
      assert(mCurrFunc && "Replacement-related data must not be null!");
      pragmaRangeToRemove(P, Clauses, mSrcMgr, mLangOpts,
        mCurrFunc->ToRemoveMetadata, PragmaFlags::IsInHeader);
      mInClause = ClauseId::ReplaceMetadata;
      for (auto *C : Clauses) {
        mCurrClauseBeginLoc = C->getBeginLoc();
        for (auto *S : Pragma::clause(&C))
          if (!RecursiveASTVisitor::TraverseStmt(S))
            break;
          checkMetadataClauseEnd(mCurrMetaBeginLoc, C->getLocEnd());
      }
      mInClause = ClauseId::NotClause;
      return true;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool VisitStringLiteral(clang::StringLiteral *SL) {
    if (mInClause != ClauseId::ReplaceMetadata)
      return true;
    assert(!mCurrFunc->Targets.empty() &&
           "At least on target must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    assert(CurrMD.TargetDecl && "Error in pragma, expected source function!");
    assert(mCurrMetaTargetParam < CurrMD.TargetDecl->getNumParams() &&
           "Parameter index is out of range!");
    assert(!SL->getString().empty() && "Member must be specified!");
    auto TargetParam = CurrMD.TargetDecl->getParamDecl(mCurrMetaTargetParam);
    auto Ty = getCanonicalUnqualifiedType(TargetParam);
    auto PtrTy = cast<clang::PointerType>(Ty);
    auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
    auto StructTy = cast<clang::RecordType>(PointeeTy);
    auto StructDecl = StructTy->getDecl();
    auto MemberItr =
        find_if(StructDecl->fields(), [SL](const FieldDecl *FieldD) {
          return FieldD->getDeclName().isIdentifier() &&
                 FieldD->getName() == SL->getString();
        });
    if (MemberItr == StructDecl->field_end()) {
      toDiag(mSrcMgr.getDiagnostics(), SL->getBeginLoc(),
             diag::error_replace_md);
      toDiag(mSrcMgr.getDiagnostics(), StructDecl->getLocation(),
             diag::note_record_member_unknown)
          << SL->getString();
      return false;
    }
    mCurrMetaMember = *MemberItr;
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    if (mInClause == ClauseId::ReplaceMetadata)
      return VisitReplaceMetadataClauseExpr(Expr);
    if (mInClause == ClauseId::Replace)
      return VisitReplaceClauseExpr(Expr);
    if (mInClause == ClauseId::With)
      return VisitReplaceWithClauseExpr(Expr);
    return true;
  }

  bool VisitCallExpr(CallExpr *Expr) {
    mCalls.push_back(Expr);
    if (mInClause == ClauseId::NotClause && mCurrWithTarget)
      mCurrFunc->Requests.try_emplace(Expr, mCurrWithTarget,
                                      mCurrClauseBeginLoc);
    mCurrWithTarget = nullptr;
    return true;
  }

  bool TraverseCompoundStmt(CompoundStmt *CS) {
    if (mInClause != ClauseId::ReplaceMetadata)
      return RecursiveASTVisitor::TraverseCompoundStmt(CS);
    assert(!mCurrFunc->Targets.empty() &&
           "At least on target must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    if (mCurrMetaTargetParam >= CurrMD.TargetDecl->getNumParams()) {
      toDiag(mSrcMgr.getDiagnostics(), mCurrMetaBeginLoc,
        diag::error_function_args_number) << mCurrMetaTargetParam + 1;
      toDiag(mSrcMgr.getDiagnostics(), CurrMD.TargetDecl->getLocation(),
        diag::note_declared_at);
      return false;
    }
    auto Res = RecursiveASTVisitor::TraverseCompoundStmt(CS);
    ++mCurrMetaTargetParam;
    mCurrMetaMember = nullptr;
    return Res;
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    if (!FD->doesThisDeclarationHaveABody())
      return true;
    mCurrFunc = mReplacements.try_emplace(FD, make_unique<FunctionInfo>(FD))
                    .first->get<FunctionInfo>()
                    .get();
    auto Res =
        RecursiveASTVisitor::TraverseFunctionDecl(FD);
    if (mCurrFunc->empty())
      mReplacements.erase(FD);
    return Res;
  }

private:
  bool VisitReplaceWithClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    if (mCurrWithTarget) {
      SmallString<32> Out;
      toDiag(mSrcMgr.getDiagnostics(), mCurrClauseBeginLoc,
             diag::error_directive_clause_twice)
          << getPragmaText(ClauseId::Replace, Out).trim('\n')
          << getName(ClauseId::With);
      return false;
    }
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
        mCurrWithTarget = FD;
        return true;
    }
    toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
      diag::error_clause_expect_function) << getName(ClauseId::With);
    toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(), diag::note_declared_at);
    return false;
  }

  bool VisitReplaceMetadataClauseExpr(DeclRefExpr *Expr) {
    assert(mCurrFunc && "Replacement description must not be null!");
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
      checkMetadataClauseEnd(mCurrMetaBeginLoc, Expr->getBeginLoc());
      mCurrFunc->Targets.emplace_back();
      auto &CurrMD = mCurrFunc->Targets.back();
      CurrMD.TargetDecl = FD;
      CurrMD.Parameters.resize(mCurrFunc->Definition->getNumParams());
      mCurrMetaTargetParam = 0;
      mCurrMetaBeginLoc = Expr->getBeginLoc();
      return true;
    }
    assert(!mCurrFunc->Targets.empty() &&
           "Storage for metadata must be initialized!");
    auto &CurrMD = mCurrFunc->Targets.back();
    assert(mCurrMetaTargetParam < CurrMD.TargetDecl->getNumParams() &&
           "Parameter index is out of range!");
    if (auto PD = dyn_cast<ParmVarDecl>(ND)) {
      auto TargetParam =
          CurrMD.TargetDecl->getParamDecl(mCurrMetaTargetParam);
      auto LHSTy = PD->getType();
      auto RHSTy =
          mCurrMetaMember ? mCurrMetaMember->getType() : TargetParam->getType();
      auto &Sema = mTfmCtx.getCompilerInstance().getSema();
      auto ConvertTy =
          Sema.CheckAssignmentConstraints(Expr->getBeginLoc(), LHSTy, RHSTy);
      bool IsPointer = false;
      if (ConvertTy != Sema::Compatible) {
        if (auto DecayedTy = dyn_cast<clang::DecayedType>(LHSTy)) {
          // Type of parameter (LHS) in replacement candidate is an array type.
          auto LHSPointeeTy = DecayedTy->getPointeeType();
          // Discard outermost array type of RHS value because it is implicitly
          // compatible with a pointer type.
          if (auto ArrayTy = dyn_cast<clang::ArrayType>(RHSTy)) {
            auto RHSElementTy = ArrayTy->getElementType();
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                Expr->getBeginLoc(), LHSPointeeTy, RHSElementTy);
            if (ConvertPointeeTy == Sema::Compatible)
              ConvertTy = ConvertPointeeTy;
          }
        } else if (auto PtrTy = dyn_cast<clang::PointerType>(LHSTy)) {
          auto LHSPointeeTy = PtrTy->getPointeeType();
          // Discard outermost array type of RHS value because it is implicitly
          // compatible with a pointer type.
          if (auto ArrayTy = dyn_cast<clang::ArrayType>(RHSTy)) {
            auto RHSElementTy = ArrayTy->getElementType();
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                Expr->getBeginLoc(), LHSPointeeTy, RHSElementTy);
            if (ConvertPointeeTy == Sema::Compatible) {
              ConvertTy = ConvertPointeeTy;
            } else if (auto NestedPtrTy =
                           dyn_cast<clang::PointerType>(LHSPointeeTy)) {
              auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
                  Expr->getBeginLoc(), NestedPtrTy->getPointeeType(),
                  RHSElementTy);
              if (ConvertPointeeTy == Sema::Compatible) {
                ConvertTy = ConvertPointeeTy;
                LHSTy = LHSPointeeTy;
                IsPointer = true;
              }
            }
          } else {
            auto ConvertPointeeTy = Sema.CheckAssignmentConstraints(
              Expr->getBeginLoc(), LHSPointeeTy, RHSTy);
            if (ConvertPointeeTy == Sema::Compatible) {
              LHSTy = LHSPointeeTy;
              ConvertTy = ConvertPointeeTy;
              IsPointer = true;
            }
          }
        }
      }
      if (ConvertTy != Sema::Compatible)
        Sema.DiagnoseAssignmentResult(ConvertTy, Expr->getBeginLoc(), LHSTy,
                                      RHSTy, Expr, Sema::AA_Passing);
      if (ConvertTy == Sema::Incompatible) {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
               diag::error_replace_md_type_incompatible)
                << (mCurrMetaMember ? 0 : 1);
        toDiag(mSrcMgr.getDiagnostics(),
               mCurrMetaMember ? mCurrMetaMember->getLocation()
                               : TargetParam->getLocation(),
               diag::note_declared_at);
        toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
          diag::note_declared_at);
        return false;
      }
      unsigned ParamIdx = 0;
      for (unsigned EI = mCurrFunc->Definition->getNumParams(); ParamIdx < EI;
           ++ParamIdx)
        if (PD == mCurrFunc->Definition->getParamDecl(ParamIdx))
          break;
      assert(ParamIdx < mCurrFunc->Definition->getNumParams() &&
             "Unknown parameter!");
      CurrMD.Parameters[ParamIdx].IsPointer = IsPointer;
      CurrMD.Parameters[ParamIdx].TargetMember = mCurrMetaMember;
      if (CurrMD.Parameters[ParamIdx].TargetParam) {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
          diag::error_replace_md_param_twice);
        return false;
      }
      CurrMD.Parameters[ParamIdx].TargetParam = mCurrMetaTargetParam;
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getLocation(),
        diag::error_expect_function_param);
      toDiag(mSrcMgr.getDiagnostics(), ND->getLocation(),
        diag::note_declared_at);
      return false;
    }
    return true;
  }

  bool VisitReplaceClauseExpr(DeclRefExpr *Expr) {
    mCurrFunc->Meta.insert(Expr);
    auto ND = Expr->getFoundDecl();
    if (auto PD = dyn_cast<ParmVarDecl>(ND)) {
      auto Ty = getCanonicalUnqualifiedType(PD);
      if (auto PtrTy = dyn_cast<clang::PointerType>(Ty)) {
        auto PointeeTy = PtrTy->getPointeeType().getTypePtr();
        if (auto StructTy = dyn_cast<clang::RecordType>(PointeeTy)) {
          mCurrFunc->Candidates.try_emplace(PD);
        } else {
          toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
                 diag::warn_disable_replace_struct_no_struct);
        }
      } else {
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
          diag::warn_disable_replace_struct_no_pointer);
      }
    } else {
      toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
        diag::warn_disable_replace_struct_no_param);
    }
    return true;
  }

  /// Check that the last metadata clause is correct.
  bool checkMetadataClauseEnd(SourceLocation BeginLoc, SourceLocation EndLoc) {
    if (mCurrFunc->Targets.empty())
      return true;
    auto TargetFD = mCurrFunc->Targets.back().TargetDecl;
    unsigned ParamIdx = mCurrFunc->Targets.back().Parameters.size();
    if (!mCurrFunc->Targets.back().valid(&ParamIdx)) {
      toDiag(mSrcMgr.getDiagnostics(), BeginLoc,
             diag::error_replace_md_missing);
      toDiag(mSrcMgr.getDiagnostics(),
             mCurrFunc->Definition->getParamDecl(ParamIdx)->getLocation(),
             diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    } else if (TargetFD->getNumParams() != mCurrMetaTargetParam) {
      toDiag(mSrcMgr.getDiagnostics(), EndLoc,
             diag::error_replace_md_target_param_expected);
      toDiag(mSrcMgr.getDiagnostics(),
             TargetFD->getParamDecl(mCurrMetaTargetParam)->getLocation(),
             diag::note_replace_md_no_param);
      mCurrFunc->Targets.pop_back();
      return false;
    }
    return true;
  }

  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  const LangOptions &mLangOpts;
  ReplacementMap &mReplacements;
  CallList &mCalls;

  FunctionInfo *mCurrFunc = nullptr;
  ClauseId mInClause = ClauseId::NotClause;
  SourceLocation mCurrClauseBeginLoc;

  FunctionDecl *mCurrWithTarget = nullptr;

  unsigned mCurrMetaTargetParam = 0;
  FieldDecl *mCurrMetaMember = nullptr;
  SourceLocation mCurrMetaBeginLoc;
};

/// Return metadata which are necessary to process request or nullptr.
///
/// Emit diagnostics if request is not valid.
ReplacementMetadata * findRequestMetadata(
    const ReplacementRequests::value_type &Request,
    const ReplacementMap &ReplacementInfo, const SourceManager &SrcMgr) {
  auto TargetItr = ReplacementInfo.find(Request.get<FunctionDecl>());
  auto toDiagNoMetadata = [&Request, &SrcMgr]() {
    toDiag(SrcMgr.getDiagnostics(), Request.get<CallExpr>()->getLocStart(),
           diag::warn_replace_call_unable);
    toDiag(SrcMgr.getDiagnostics(), Request.get<SourceLocation>(),
           diag::note_replace_call_no_md)
        << Request.get<FunctionDecl>();
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<FunctionDecl>()->getLocation(),
           diag::note_declared_at);
  };
  if (TargetItr == ReplacementInfo.end()) {
    toDiagNoMetadata();
    return nullptr;
  }
  auto CalleeFD = Request.get<clang::CallExpr>()->getDirectCallee();
  if (!CalleeFD) {
    toDiag(SrcMgr.getDiagnostics(),
           Request.get<clang::CallExpr>()->getLocStart(),
      diag::warn_replace_call_indirect_unable);
    return nullptr;
  }
  CalleeFD = CalleeFD->getCanonicalDecl();
  auto &TargetInfo = *TargetItr->get<FunctionInfo>();
  auto MetaItr = llvm::find_if(
      TargetInfo.Targets, [CalleeFD](const ReplacementMetadata &RM) {
        return RM.TargetDecl == CalleeFD;
      });
  if (MetaItr == TargetInfo.Targets.end()) {
    toDiagNoMetadata();
    return nullptr;
  }
  return &*MetaItr;
}

class ReplacementSanitizer : public RecursiveASTVisitor<ReplacementSanitizer> {
public:
  using ReplacementCandidates =
    SmallDenseMap<NamedDecl *, SmallVector<Replacement, 8>, 8>;

  ReplacementSanitizer(TransformationContext &TfmCtx, FunctionInfo &RC,
      ReplacementMap &ReplacementInfo)
     : mTfmCtx(TfmCtx)
     , mSrcMgr(mTfmCtx.getContext().getSourceManager())
     , mReplacements(RC)
     , mReplacementInfo(ReplacementInfo)
  {}

  bool TraverseStmt(Stmt *S) {
    if (S && std::distance(S->child_begin(), S->child_end()) > 1) {
      LLVM_DEBUG(if (mInAssignment) dbgs()
                 << "[REPLACE]: disable assignment check\n");
      mInAssignment = false;
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool TraverseCallExpr(clang::CallExpr *Expr) {
    auto RequestItr = mReplacements.Requests.find(Expr);
    if (RequestItr == mReplacements.Requests.end()) {
      auto ImplicitItr = mReplacements.ImplicitRequests.find(Expr);
      if (ImplicitItr == mReplacements.ImplicitRequests.end())
        return RecursiveASTVisitor::TraverseCallExpr(Expr);
      bool Res = true;
      for (unsigned ArgIdx = 0, EI = Expr->getNumArgs(); ArgIdx < EI; ++ArgIdx) {
        auto ArgExpr = Expr->getArg(ArgIdx);
        auto ReplacementItr =
          isExprInCandidates(ArgExpr, mReplacements.Candidates);
        // Do not process replacement candidates if a corresponding callee
        // may be cloned further.
        if (ReplacementItr == mReplacements.Candidates.end())
          Res &= TraverseStmt(ArgExpr);
      }
      return Res;
    }
    assert(RequestItr->get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(*RequestItr, mReplacementInfo, mSrcMgr);
    if (!Meta) {
      mReplacements.Requests.erase(RequestItr);
      return RecursiveASTVisitor::TraverseCallExpr(Expr);
    }
    bool Res = true;
    for (unsigned ArgIdx = 0, EI= Expr->getNumArgs(); ArgIdx < EI; ++ArgIdx) {
      auto ArgExpr = Expr->getArg(ArgIdx);
      auto ReplacementItr =
          isExprInCandidates(ArgExpr, mReplacements.Candidates);
      if (ReplacementItr != mReplacements.Candidates.end()) {
        for (auto &ParamMeta : Meta->Parameters) {
          if (*ParamMeta.TargetParam != ArgIdx)
            continue;
          if (!ParamMeta.TargetMember) {
            toDiag(mSrcMgr.getDiagnostics(),
                   ReplacementItr->get<NamedDecl>()->getLocStart(),
                   diag::warn_disable_replace_struct);
            toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
                   diag::note_replace_struct_arrow);
            mReplacements.Candidates.erase(ReplacementItr->get<NamedDecl>());
            break;
          }
          auto Itr = addToReplacement(ParamMeta.TargetMember,
                                      ReplacementItr->get<Replacement>());
        }
      } else {
        Res &= !TraverseStmt(ArgExpr);
      }
    }
    return Res;
  }

  bool VisitDeclRefExpr(DeclRefExpr *Expr) {
    mLastDeclRef = nullptr;
    if (!mIsInnermostMember && !mReplacements.inClause(Expr)) {
      auto ND = Expr->getFoundDecl();
      if (mReplacements.Candidates.count(ND)) {
        toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
          diag::warn_disable_replace_struct);
        toDiag(mSrcMgr.getDiagnostics(), Expr->getLocStart(),
          diag::note_replace_struct_arrow);
        mReplacements.Candidates.erase(ND);
      }
    } else {
      mLastDeclRef = Expr;
    }
    return true;
  }

  bool TraverseMemberExpr(MemberExpr *Expr) {
    mIsInnermostMember = true;
    auto Res = RecursiveASTVisitor::TraverseMemberExpr(Expr);
    if (mIsInnermostMember && mLastDeclRef) {
      auto ND = mLastDeclRef->getFoundDecl();
      auto ReplacementItr = mReplacements.Candidates.find(ND);
      if (ReplacementItr != mReplacements.Candidates.end()) {
        if (!Expr->isArrow()) {
          toDiag(mSrcMgr.getDiagnostics(), ND->getLocStart(),
            diag::warn_disable_replace_struct);
          toDiag(mSrcMgr.getDiagnostics(), Expr->getOperatorLoc(),
            diag::note_replace_struct_arrow);
          mReplacements.Candidates.erase(ReplacementItr);
        } else {
          auto Itr = addToReplacement(Expr->getMemberDecl(),
            ReplacementItr->get<Replacement>());
          Itr->Ranges.emplace_back(Expr->getSourceRange());
          Itr->InAssignment |= mInAssignment;
        }
      }
    }
    mIsInnermostMember = false;
    return Res;
  }

  bool TraverseBinAssign(BinaryOperator *BO) {
    mInAssignment = true;
    LLVM_DEBUG(dbgs() << "[REPLACE]: check assignment at ";
               BO->getOperatorLoc().print(dbgs(), mSrcMgr); dbgs() << "\n");
    auto Res = TraverseStmt(BO->getLHS());
    LLVM_DEBUG(dbgs() << "[REPLACE]: disable assignment check\n");
    mInAssignment = false;
    return Res && TraverseStmt(BO->getRHS());
  }

private:
  auto addToReplacement(ValueDecl *Member, SmallVectorImpl<Replacement> &List)
      -> SmallVectorImpl<Replacement>::iterator {
    auto Itr = find_if(
        List, [Member](const Replacement &R) { return R.Member == Member; });
    if (Itr == List.end()) {
      List.emplace_back(Member);
      return List.end() - 1;
    }
    return Itr;
  }

  TransformationContext &mTfmCtx;
  SourceManager &mSrcMgr;
  FunctionInfo &mReplacements;
  ReplacementMap &mReplacementInfo;

  bool mIsInnermostMember = false;
  DeclRefExpr *mLastDeclRef;
  bool mInAssignment = false;
};

/// Check that types which are necessary to build checked declaration are
/// available outside the root declaration.
class TypeSearch : public RecursiveASTVisitor<TypeSearch> {
public:
  TypeSearch(NamedDecl *Root, NamedDecl *Check, SourceManager &SrcMgr,
      const GlobalInfoExtractor &GlobalInfo)
    : mRootDecl(Root), mCheckDecl(Check)
    , mSrcMgr(SrcMgr), mGlobalInfo(GlobalInfo) {
    assert(Root && "Declaration must not be null!");
    assert(Check && "Declaration must not be null!");
  }

  bool VisitTagType(TagType *TT) {
    if (!mGlobalInfo.findOutermostDecl(TT->getDecl())) {
      toDiag(mSrcMgr.getDiagnostics(), mRootDecl->getLocation(),
             diag::warn_disable_replace_struct);
      toDiag(mSrcMgr.getDiagnostics(), mCheckDecl->getLocStart(),
             diag::note_replace_struct_decl);
      mIsOk = false;
      return false;
    }
    return true;
  }

  bool isOk() const noexcept { return mIsOk; }

private:
  NamedDecl *mRootDecl;
  NamedDecl *mCheckDecl;
  SourceManager &mSrcMgr;
  const GlobalInfoExtractor &mGlobalInfo;
  bool mIsOk = true;
};

/// Insert #pragma inside the body of a new function to describe its relation
/// with the original function.
void addPragmaMetadata(FunctionInfo &FuncInfo,
    SourceManager &SrcMgr, const LangOptions &LangOpts,
    ExternalRewriter &Canvas) {
  SmallString<256> MDPragma;
  MDPragma.push_back('\n');
  getPragmaText(ClauseId::ReplaceMetadata, MDPragma);
  if (MDPragma.back() == '\n')
    MDPragma.pop_back();
  MDPragma.push_back('(');
  MDPragma += FuncInfo.Definition->getName();
  MDPragma.push_back('(');
  for (unsigned I = 0, EI = FuncInfo.Definition->getNumParams(); I < EI; ++I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I);
    if (I > 0)
      MDPragma.push_back(',');
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      MDPragma += PD->getName();
      continue;
    }
    MDPragma += "{";
    auto Itr = ReplacementItr->get<Replacement>().begin();
    auto EndItr = ReplacementItr->get<Replacement>().end();
    if (Itr != EndItr) {
      MDPragma += ".";
      MDPragma += Itr->Member->getName();
      MDPragma += "=";
      MDPragma += Itr->Identifier;
      ++Itr;
    }
    for (auto &R : make_range(Itr, EndItr)) {
      MDPragma += ',';
      MDPragma += ".";
      MDPragma += R.Member->getName();
      MDPragma += "=";
      MDPragma += R.Identifier;
    }
    MDPragma.push_back('}');
  }
  MDPragma.push_back(')');
  MDPragma.push_back(')');
  auto FuncBody = FuncInfo.Definition->getBody();
  assert(FuncBody && "Body of a transformed function must be available!");
  auto NextToBraceLoc = SrcMgr.getExpansionLoc(FuncBody->getLocStart());
  Token Tok;
  if (getRawTokenAfter(NextToBraceLoc, SrcMgr, LangOpts, Tok) ||
      SrcMgr.getPresumedLineNumber(Tok.getLocation()) ==
        SrcMgr.getPresumedLineNumber(NextToBraceLoc)) {
    MDPragma.push_back('\n');
  }
  NextToBraceLoc = NextToBraceLoc.getLocWithOffset(1);
  Canvas.InsertTextAfter(NextToBraceLoc, MDPragma);
}

template <class RewriterT>
void replaceCall(FunctionInfo &FI, const CallExpr &Expr,
    StringRef ReplacementName, const ReplacementMetadata &Meta,
    RewriterT &Rewriter) {
  auto &SrcMgr = Rewriter.getSourceMgr();
  SmallString<256> NewCallExpr;
  NewCallExpr += ReplacementName;
  NewCallExpr += '(';
  for (unsigned I = 0, EI = Meta.Parameters.size(); I < EI; ++I) {
    if (I > 0)
      NewCallExpr += ", ";
    auto &ParamInfo = Meta.Parameters[I];
    auto ArgExpr = Expr.getArg(*ParamInfo.TargetParam);
    auto ReplacementItr = isExprInCandidates(ArgExpr, FI.Candidates);
    if (ReplacementItr == FI.Candidates.end()) {
      if (ParamInfo.IsPointer)
        NewCallExpr += '&';
      if (ParamInfo.TargetMember) {
        NewCallExpr += '(';
        NewCallExpr += Rewriter.getRewrittenText(
          SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
        NewCallExpr += ')';
        NewCallExpr += "->";
        NewCallExpr += ParamInfo.TargetMember->getName();
      } else {
        if (ParamInfo.IsPointer)
          NewCallExpr += '(';
        NewCallExpr += Rewriter.getRewrittenText(
          SrcMgr.getExpansionRange(ArgExpr->getSourceRange()).getAsRange());
        if (ParamInfo.IsPointer)
          NewCallExpr += ')';
      }
    } else {
      auto Itr = find_if(ReplacementItr->template get<Replacement>(),
                      [&ParamInfo](const Replacement &R) {
                        return R.Member == ParamInfo.TargetMember;
                      });
      assert(Itr != ReplacementItr->get<Replacement>().end() &&
             "Description of the replacement must be found!");
      if (Itr->InAssignment || FI.Strict) {
        if (ParamInfo.IsPointer) {
          NewCallExpr += Itr->Identifier;
        } else {
          NewCallExpr += "*";
          NewCallExpr += Itr->Identifier;
        }
      } else if (ParamInfo.IsPointer) {
        NewCallExpr += "&";
        NewCallExpr += Itr->Identifier;
      } else {
        NewCallExpr += Itr->Identifier;
      }
    }
  }
  NewCallExpr += ')';
  Rewriter.ReplaceText(
      getExpansionRange(SrcMgr, Expr.getSourceRange()).getAsRange(),
      NewCallExpr);
}

#ifdef LLVM_DEBUG
void printMetadataLog(const FunctionInfo &FuncInfo) {
  auto *FD = FuncInfo.Definition;
  auto &Sources = FuncInfo.Targets;
  if (Sources.empty())
    return;
  dbgs() << "[REPLACE]: replacement is '" << FD->getName() << "' function\n";
  for (auto &SI : Sources) {
    dbgs() << "[REPLACE]: target '" << SI.TargetDecl->getName()
           << "' for replacement";
    if (!SI.valid()) {
      dbgs() << " is not valid\n";
      continue;
    }
    if (FD->getCanonicalDecl() == SI.TargetDecl)
      dbgs() << " is implicit";
    dbgs() << "\n";
    const FunctionDecl *TargetDefinition = SI.TargetDecl;
    SI.TargetDecl->hasBody(TargetDefinition);
    for (unsigned I = 0, EI = SI.Parameters.size(); I < EI; ++I) {
      auto &PI = SI.Parameters[I];
      auto TargetParam = TargetDefinition->getParamDecl(*PI.TargetParam);
      dbgs() << "[REPLACE]: target parameter ";
      if (!TargetParam->getIdentifier())
        dbgs() << TargetParam->getName();
      else
        dbgs() << "<" << *PI.TargetParam << ">";
      if (PI.TargetMember)
        dbgs() << "." << PI.TargetMember->getName();
      dbgs() << "->" << I << " (";
      if (FD->getCanonicalDecl() != SI.TargetDecl)
        dbgs() << FD->getParamDecl(I)->getName() << ",";
      dbgs() << (PI.IsPointer ? "pointer" : "value");
      dbgs() << ")\n";
    }
  }
}

void printCandidateLog(const ReplacementCandidates &Candidates, bool IsStrict) {
  dbgs() << "[REPLACE]: " << (IsStrict ? "strict" : "nostrict")
    << " replacement\n";
  dbgs() << "[REPLACE]: replacement candidates found";
  for (auto &Candidate : Candidates)
    dbgs() << " " << Candidate.get<NamedDecl>()->getName();
  dbgs() << "\n";
}

void printRequestLog(FunctionInfo &FuncInfo, const SourceManager &SrcMgr) {
  if (FuncInfo.Requests.empty())
    return;
  dbgs() << "[REPLACE]: callee replacement requests inside '"
         << FuncInfo.Definition->getName()
         << "' found\n";
  for (auto &Request : FuncInfo.Requests) {
    dbgs() << "[REPALCE]: with " << FuncInfo.Definition->getName() << " at ";
    Request.get<clang::CallExpr>()->getLocStart().print(dbgs(), SrcMgr);
    dbgs() << "\n";
  }
}

void printImplicitRequestLog(const FunctionInfo &FuncInfo,
    const SourceManager &SrcMgr) {
  if (FuncInfo.ImplicitRequests.empty())
    return;
  dbgs() << "[REPLACE]: callee replacement implicit requests inside '"
         << FuncInfo.Definition->getName() << "' found\n";
  for (auto &Request : FuncInfo.ImplicitRequests) {
    dbgs() << "[REPALCE]: at ";
    Request->getLocStart().print(dbgs(), SrcMgr);
    dbgs() << "\n";
  }
}
#endif

template<class RewriterT > bool replaceCalls(FunctionInfo &FI,
    ReplacementMap &ReplacementInfo, RewriterT &Rewriter) {
  auto &SrcMgr = Rewriter.getSourceMgr();
  LLVM_DEBUG(printRequestLog(FI, SrcMgr));
  bool IsChanged = false;
  for (auto &Request : FI.Requests) {
    assert(Request.get<clang::CallExpr>() && "Call must not be null!");
    assert(Request.get<clang::FunctionDecl>() &&
           "Target function must not be null!");
    auto Meta = findRequestMetadata(Request, ReplacementInfo, SrcMgr);
    if (!Meta)
      continue;
    replaceCall(FI, *Request.get<clang::CallExpr>(),
                Request.get<FunctionDecl>()->getName(), *Meta, Rewriter);
    IsChanged = true;
  }
  for (auto *Request : FI.ImplicitRequests) {
    auto Callee = Request->getDirectCallee();
    if (!Callee)
      continue;
    auto Itr = ReplacementInfo.find(Callee);
    if (Itr == ReplacementInfo.end())
      continue;
    auto MetaItr = find_if(
        Itr->get<FunctionInfo>()->Targets, [Callee](ReplacementMetadata &Meta) {
          return Meta.TargetDecl == Callee->getCanonicalDecl();
        });
    if (MetaItr == Itr->get<FunctionInfo>()->Targets.end())
      continue;
    assert(!Itr->get<FunctionInfo>()->ReplacmenetName.empty() &&
      "Name of the function clone must not be null!");
    replaceCall(FI, *Request, Itr->get<FunctionInfo>()->ReplacmenetName,
      *MetaItr, Rewriter);
    IsChanged = true;
  }
  return IsChanged;
}

class ClangStructureReplacementPass :
    public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  ClangStructureReplacementPass() : ModulePass(ID) {
      initializeClangStructureReplacementPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TransformationEnginePass>();
    AU.addRequired<ClangGlobalInfoPass>();
    AU.addRequired<ClangIncludeTreePass>();
    AU.getPreservesAll();
  }

  void releaseMemory() override {
    mReplacementInfo.clear();
    mTfmCtx = nullptr;
    mGlobalInfo = nullptr;
    mRawInfo = nullptr;
  }

private:
  void addSuffix(const Twine &Prefix, SmallVectorImpl<char> &Out) {
    for (unsigned Count = 0;
      mRawInfo->Identifiers.count((Prefix + Twine(Count)).toStringRef(Out));
      ++Count, Out.clear());
    mRawInfo->Identifiers.insert(StringRef(Out.data(), Out.size()));
  }

  FunctionInfo * tieCallGraphNode(CallGraphNode *CGN) {
    if (!CGN->getDecl() || !isa<FunctionDecl>(CGN->getDecl()))
      return nullptr;
    auto InfoItr = mReplacementInfo.find(CGN->getDecl()->getAsFunction());
    if (InfoItr == mReplacementInfo.end())
      return nullptr;
    return InfoItr->get<FunctionInfo>().get();
  }

  /// Collect replacement candidates for functions in a specified strongly
  /// connected component in a call graph.
  void collectCandidatesIn(scc_iterator<CallGraph *> &SCC);

  /// Check accesses to replacement candidates inside a specified function.
  ///
  /// Remove replacement candidate if it cannot be replaced.
  void sanitizeCandidates(FunctionInfo &FuncInfo);

  /// Update list of members which should become parameters in a new function
  /// according to accesses in callees.
  ///
  /// If parameter has a structure type and it should be replaced, some its
  /// members can be implicitly accessed in callees while these members are not
  /// accessed in a function explicitly. This function collects these members
  /// for further replacement.
  void fillImplicitReplacementMembers(scc_iterator<CallGraph *> &SCC);

  /// Check replacement candidates which are passed to calls.
  /// Remove replacement candidates if it cannot be replaced in callee.
  void sanitizeCandidatesInCalls(scc_iterator<CallGraph *> &SCC);

  void buildParameters(FunctionInfo &FuncInfo);
  void buildParameters(scc_iterator<CallGraph *> &SCC) {
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      buildParameters(*FuncInfo);
    }
  }

  void insertNewFunction(FunctionInfo &FuncInfo );
  void insertNewFunctions(scc_iterator<CallGraph *> &SCC) {
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      insertNewFunction(*FuncInfo);
    }
  }

  TransformationContext *mTfmCtx = nullptr;
  ClangGlobalInfoPass::RawInfo *mRawInfo = nullptr;
  const GlobalInfoExtractor *mGlobalInfo = nullptr;
  ReplacementMap mReplacementInfo;
};
} // namespace

char ClangStructureReplacementPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(ClangStructureReplacementPass,
  "clang-struct-replacement", "Source-level Structure Replacement (Clang)",
  false, false,
  tsar::TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangIncludeTreePass)
INITIALIZE_PASS_IN_GROUP_END(ClangStructureReplacementPass,
  "clang-struct-replacement", "Source-level Structure Replacement (Clang)",
  false, false,
  tsar::TransformationQueryManager::getPassRegistry())

ModulePass * llvm::createClangStructureReplacementPass() {
  return new ClangStructureReplacementPass;
}

void ClangStructureReplacementPass::collectCandidatesIn(
    scc_iterator<CallGraph *> &SCC) {
  std::vector<std::pair<FunctionDecl *, CallList>> Calls;
  LLVM_DEBUG(dbgs() << "[REPLACE]: process functions in SCC\n");
  for (auto *CGN : *SCC) {
    if (!CGN->getDecl() || !isa<FunctionDecl>(CGN->getDecl()))
      continue;
    const auto *Definition =
        cast<FunctionDecl>(CGN->getDecl()->getCanonicalDecl());
    if (!Definition->hasBody(Definition))
      continue;
    LLVM_DEBUG(dbgs() << "[REPLACE]: process '" << Definition->getName()
                      << "'\n");
    Calls.emplace_back(const_cast<FunctionDecl *>(Definition), CallList());
    ReplacementCollector Collector(*mTfmCtx, mReplacementInfo,
                                   Calls.back().second);
    Collector.TraverseDecl(const_cast<FunctionDecl *>(Definition));
  }
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto &CallerToCalls : Calls) {
      auto *Caller = CallerToCalls.first;
      auto CallerItr = mReplacementInfo.find(Caller);
      if (CallerItr == mReplacementInfo.end() ||
          !CallerItr->get<FunctionInfo>()->hasCandidates())
        continue;
      for (auto *Call : CallerToCalls.second) {
        if (CallerItr->get<FunctionInfo>()->Requests.count(Call))
          continue;
        const auto *CalleeDefinition = Call->getDirectCallee();
        if (!CalleeDefinition)
          continue;
        CalleeDefinition = CalleeDefinition->getFirstDecl();
        if (!CalleeDefinition->hasBody(CalleeDefinition))
          continue;
        auto *Callee = const_cast<FunctionDecl *>(CalleeDefinition);
        auto CalleeItr = mReplacementInfo.find(Callee);
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr = (isExprInCandidates(
              Call->getArg(I), CallerItr->get<FunctionInfo>()->Candidates));
          if (Itr == CallerItr->get<FunctionInfo>()->Candidates.end())
            continue;
          CallerItr->get<FunctionInfo>()->ImplicitRequests.insert(Call);
          if (CalleeItr == mReplacementInfo.end()) {
            CalleeItr =
                mReplacementInfo
                    .try_emplace(Callee, make_unique<FunctionInfo>(Callee))
                    .first;
            CalleeItr->get<FunctionInfo>()->Strict =
                CallerItr->get<FunctionInfo>()->Strict;
            LLVM_DEBUG(dbgs()
                       << "[REPLACE]: add implicit "
                       << (CalleeItr->get<FunctionInfo>()->Strict ? "strict"
                                                                  : "nostrict")
                       << " replacement for '"
                       << CalleeItr->get<FunctionDecl>()->getName() << "'\n");
          }
          if (CalleeItr->get<FunctionInfo>()
                  ->Candidates.try_emplace(Callee->getParamDecl(I))
                  .second) {
            IsChanged = true;
            CalleeItr->get<FunctionInfo>()->Strict |=
                CallerItr->get<FunctionInfo>()->Strict;
          }
        }
      }
    }
  } while (IsChanged && SCC.hasLoop());
}

void ClangStructureReplacementPass::sanitizeCandidates(FunctionInfo &FuncInfo) {
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  auto &LangOpts = mTfmCtx->getContext().getLangOpts();
  // Check general preconditions.
  auto FuncRange =
      SrcMgr.getExpansionRange(FuncInfo.Definition->getSourceRange());
  if (!SrcMgr.isWrittenInSameFile(FuncRange.getBegin(), FuncRange.getEnd())) {
    FuncInfo.Candidates.clear();
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
      diag::warn_disable_replace_struct);
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocStart(),
      diag::note_source_range_not_single_file);
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocEnd(),
      diag::note_end_location);
    return;
  }
  if (SrcMgr.getFileCharacteristic(FuncInfo.Definition->getLocStart()) !=
    SrcMgr::C_User) {
    FuncInfo.Candidates.clear();
    toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
      diag::warn_disable_replace_struct_system);
    return;
  }
  if (FuncInfo.Strict) {
    bool HasMacro = false;
    for_each_macro(FuncInfo.Definition, SrcMgr, LangOpts, mRawInfo->Macros,
      [&FuncInfo, &SrcMgr, &HasMacro](SourceLocation Loc) {
        if (!HasMacro) {
          HasMacro = true;
          toDiag(SrcMgr.getDiagnostics(), FuncInfo.Definition->getLocation(),
            diag::warn_disable_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), Loc,
            diag::note_replace_struct_macro_prevent);
        }
      });
    if (HasMacro) {
      FuncInfo.Candidates.clear();
      return;
    }
  }
  ReplacementSanitizer Verifier(*mTfmCtx, FuncInfo, mReplacementInfo);
  Verifier.TraverseDecl(FuncInfo.Definition);
}

void ClangStructureReplacementPass::fillImplicitReplacementMembers(
    scc_iterator<CallGraph *> &SCC) {
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      for (auto *Call : FuncInfo->ImplicitRequests) {
        auto *Callee = Call->getDirectCallee();
        assert(Callee &&
          "Called must be known for a valid implicit request!");
        auto CalleeItr = mReplacementInfo.find(Callee);
        if (CalleeItr == mReplacementInfo.end())
          continue;
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr =
            isExprInCandidates(Call->getArg(I), FuncInfo->Candidates);
          if (Itr == FuncInfo->Candidates.end())
            continue;
          auto CalleeCandidateItr =
            CalleeItr->get<FunctionInfo>()->Candidates.find(
              CalleeItr->get<FunctionInfo>()->Definition->getParamDecl(I));
          if (CalleeCandidateItr ==
            CalleeItr->get<FunctionInfo>()->Candidates.end())
            continue;
          for (auto &CalleeR : CalleeCandidateItr->get<Replacement>()) {
            auto CallerRItr = find_if(Itr->get<Replacement>(),
              [&CalleeR](const Replacement &R) {
                return R.Member == CalleeR.Member;
              });
            if (CallerRItr != Itr->get<Replacement>().end()) {
              if (!CallerRItr->InAssignment && CalleeR.InAssignment)
                CallerRItr->InAssignment = IsChanged = true;
            } else {
              Itr->get<Replacement>().emplace_back(CalleeR.Member);
              Itr->get<Replacement>().back().InAssignment =
                CalleeR.InAssignment;
              IsChanged = true;
            }
          }
        }
      }
    }
  } while (IsChanged && SCC.hasLoop());
}

void ClangStructureReplacementPass::sanitizeCandidatesInCalls(
    scc_iterator<CallGraph *> &SCC) {
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  bool IsChanged = false;
  do {
    IsChanged = false;
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo || !FuncInfo->hasCandidates())
        continue;
      SmallVector<CallExpr *, 8> ImplicitRequestToRemove;
      for (auto *Call : FuncInfo->ImplicitRequests) {
        auto *Callee = Call->getDirectCallee();
        assert(Callee &&
          "Called must be known for a valid implicit request!");
        auto CalleeItr = mReplacementInfo.find(Callee);
        bool HasCandidatesInArgs = false;
        for (unsigned I = 0, EI = Call->getNumArgs(); I < EI; ++I) {
          auto Itr =
            (isExprInCandidates(Call->getArg(I), FuncInfo->Candidates));
          if (Itr == FuncInfo->Candidates.end())
            continue;
          if (CalleeItr == mReplacementInfo.end()) {
            toDiag(SrcMgr.getDiagnostics(),
                   Itr->get<NamedDecl>()->getLocation(),
                   diag::warn_disable_replace_struct);
            toDiag(SrcMgr.getDiagnostics(), Call->getArg(I)->getLocStart(),
              diag::note_replace_struct_arrow);
            FuncInfo->Candidates.erase(Itr);
            IsChanged = true;
          } else {
            if (!CalleeItr->get<FunctionInfo>()->Candidates.count(
                    CalleeItr->get<FunctionInfo>()->Definition->getParamDecl(
                        I))) {
              toDiag(SrcMgr.getDiagnostics(),
                     Itr->get<NamedDecl>()->getLocation(),
                     diag::warn_disable_replace_struct);
              toDiag(SrcMgr.getDiagnostics(), Call->getArg(I)->getLocStart(),
                     diag::note_replace_struct_arrow);
              FuncInfo->Candidates.erase(Itr);
              IsChanged = true;
            } else {
              HasCandidatesInArgs = true;
            }
          }
        }
        if (!HasCandidatesInArgs)
          ImplicitRequestToRemove.push_back(Call);
      }
      for (auto *Call : ImplicitRequestToRemove)
        FuncInfo->ImplicitRequests.erase(Call);
    }
  } while (IsChanged && SCC.hasLoop());
}


void ClangStructureReplacementPass::buildParameters(FunctionInfo &FuncInfo) {
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  auto &LangOpts = mTfmCtx->getContext().getLangOpts();
  // Look up for declaration of types of parameters.
  auto &FT = getAnalysis<ClangIncludeTreePass>().getFileTree();
  std::string Context;
  auto *OFD = mGlobalInfo->findOutermostDecl(FuncInfo.Definition);
  assert(OFD && "Outermost declaration for the current function must be known!");
  auto Root = FileNode::ChildT(FT.findRoot(OFD));
  assert(Root && "File which contains declaration must be known!");
  for (auto &Internal : FT.internals()) {
    if (auto *TD = dyn_cast<TypeDecl>(Internal.getDescendant())) {
      Context += Lexer::getSourceText(
        SrcMgr.getExpansionRange(TD->getSourceRange()), SrcMgr, LangOpts);
      Context += ";";
    }
  }
  for (auto *N : depth_first(&Root)) {
    if (N->is<FileNode *>())
      continue;
    auto *OD = N->get<const FileNode::OutermostDecl *>();
    if (OD == OFD)
      break;
    if (auto *TD = dyn_cast<TypeDecl>(OD->getDescendant())) {
      Context += Lexer::getSourceText(
        SrcMgr.getExpansionRange(TD->getSourceRange()), SrcMgr, LangOpts);
      Context += ";";
    }
  }
  // Replace aggregate parameters with separate variables.
  StringMap<std::string> Replacements;
  bool TheLastParam = true;
  for (unsigned I = FuncInfo.Definition->getNumParams(); I > 0; --I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I - 1);
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      // Remove comma after the current parameter if it becomes the last one.
      if (TheLastParam) {
        SourceLocation EndLoc = PD->getLocEnd();
        Token CommaTok;
        if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr,
          LangOpts, CommaTok)) {
          toDiag(SrcMgr.getDiagnostics(), PD->getEndLoc(),
            diag::warn_transform_internal);
          FuncInfo.Candidates.clear();
          break;
        }
        if (CommaTok.is(tok::comma))
          FuncInfo.ToRemoveClone.emplace_back(
            CharSourceRange::getTokenRange(
              SrcMgr.getExpansionLoc(CommaTok.getLocation())));
      }
      TheLastParam = false;
      continue;
    }
    SmallString<128> NewParams;
    // We also remove an unused parameter if it is mentioned in replace clause.
    if (ReplacementItr->get<Replacement>().empty()) {
      SourceLocation EndLoc = PD->getLocEnd();
      Token CommaTok;
      if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
        CommaTok)) {
        toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
          diag::warn_disable_replace_struct);
        toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
          diag::note_replace_struct_de_decl);
        FuncInfo.Candidates.erase(ReplacementItr);
        TheLastParam = false;
        continue;
      }
      if (CommaTok.is(tok::comma))
        EndLoc = CommaTok.getLocation();
      FuncInfo.ToRemoveClone.emplace_back(CharSourceRange::getTokenRange(
        SrcMgr.getExpansionLoc(PD->getLocStart()),
        SrcMgr.getExpansionLoc(EndLoc)));
      toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
        diag::remark_replace_struct);
      toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
        diag::remark_remove_de_decl);
      // Do not update TheLastParam variable. If the current parameter is the
      // last in the list and if it is removed than the previous parameter
      // in the list become the last one.
      continue;
    }
    auto StashContextSize = Context.size();
    for (auto &R : ReplacementItr->get<Replacement>()) {
      TypeSearch TS(PD, R.Member, SrcMgr, *mGlobalInfo);
      TS.TraverseDecl(R.Member);
      if (!TS.isOk()) {
        Context.resize(StashContextSize);
        NewParams.clear();
        break;
      }
      addSuffix(PD->getName() + "_" + R.Member->getName(), R.Identifier);
      auto ParamType = (R.InAssignment || FuncInfo.Strict)
        ? R.Member->getType().getAsString() + "*"
        : R.Member->getType().getAsString();
      auto Tokens =
        buildDeclStringRef(ParamType, R.Identifier, Context, Replacements);
      if (Tokens.empty()) {
        Context.resize(StashContextSize);
        NewParams.clear();
        toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
          diag::warn_disable_replace_struct);
        toDiag(SrcMgr.getDiagnostics(), R.Member->getLocStart(),
          diag::note_replace_struct_decl);
        break;
      }
      if (!NewParams.empty())
        NewParams.push_back(',');
      auto Size = NewParams.size();
      join(Tokens.begin(), Tokens.end(), " ", NewParams);
      Context += StringRef(NewParams.data() + Size, NewParams.size() - Size);
      Context += ";";
      LLVM_DEBUG(dbgs() << "[REPLACE]: replacement for " << I
        << " parameter: "
        << StringRef(NewParams.data() + Size,
          NewParams.size() - Size)
        << "\n");
    }
    if (!NewParams.empty()) {
      SourceLocation EndLoc = PD->getLocEnd();
      // If the next parameter in the parameter list is unused and it has been
      // successfully remove, we have to remove comma after the current
      // parameter.
      if (TheLastParam) {
        Token CommaTok;
        if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
          CommaTok)) {
          toDiag(SrcMgr.getDiagnostics(), PD->getLocation(),
            diag::warn_disable_replace_struct);
          toDiag(SrcMgr.getDiagnostics(), PD->getLocStart(),
            diag::note_replace_struct_decl_internal);
          FuncInfo.Candidates.erase(ReplacementItr);
          continue;
        }
        if (CommaTok.is(tok::comma))
          EndLoc = CommaTok.getLocation();
      }
      auto Range = SrcMgr
        .getExpansionRange(CharSourceRange::getTokenRange(
          PD->getLocStart(), EndLoc))
        .getAsRange();
      ReplacementItr->get<std::string>() = NewParams.str();
      ReplacementItr->get<SourceRange>() = Range;
    } else {
      // Remove comma after the current parameter if it becomes the last one.
      if (TheLastParam) {
        SourceLocation EndLoc = PD->getLocEnd();
        Token CommaTok;
        if (getRawTokenAfter(SrcMgr.getExpansionLoc(EndLoc), SrcMgr, LangOpts,
          CommaTok)) {
          toDiag(SrcMgr.getDiagnostics(), PD->getEndLoc(),
            diag::warn_transform_internal);
          FuncInfo.Candidates.clear();
          break;
        }
        if (CommaTok.is(tok::comma))
          FuncInfo.ToRemoveClone.emplace_back(
            CharSourceRange::getTokenRange(
              SrcMgr.getExpansionLoc(CommaTok.getLocation())));
      }
      FuncInfo.Candidates.erase(ReplacementItr);
    }
    TheLastParam = false;
  }
}

void ClangStructureReplacementPass::insertNewFunction(FunctionInfo &FuncInfo) {
  auto &Rewriter = mTfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  LLVM_DEBUG(printCandidateLog(FuncInfo.Candidates, FuncInfo.Strict));
  LLVM_DEBUG(printRequestLog(FuncInfo, SrcMgr));
  LLVM_DEBUG(printImplicitRequestLog(FuncInfo, SrcMgr));
  ExternalRewriter Canvas(
      tsar::getExpansionRange(SrcMgr, FuncInfo.Definition->getSourceRange())
          .getAsRange(),
    SrcMgr, LangOpts);
  // Build unique name for a new function.
  addSuffix(FuncInfo.Definition->getName() + "_spf",
            FuncInfo.ReplacmenetName);
  SourceRange NameRange(
      SrcMgr.getExpansionLoc(FuncInfo.Definition->getLocation()));
  NameRange.setEnd(NameRange.getBegin().getLocWithOffset(
      FuncInfo.Definition->getName().size() - 1));
  Canvas.ReplaceText(NameRange, FuncInfo.ReplacmenetName);
  // Replace accesses to parameters.
  for (auto &ParamInfo : FuncInfo.Candidates) {
    Canvas.ReplaceText(ParamInfo.get<SourceRange>(),
      ParamInfo.get<std::string>());
    for (auto &R : ParamInfo.get<Replacement>()) {
      for (auto Range : R.Ranges) {
        SmallString<64> Tmp;
        auto AccessString = R.InAssignment || FuncInfo.Strict
          ? ("(*" + R.Identifier + ")").toStringRef(Tmp)
          : StringRef(R.Identifier);
        Canvas.ReplaceText(Range, AccessString);
      }
    }
  }
  // Build implicit metadata.
  FuncInfo.Targets.emplace_back();
  auto &FuncMeta = FuncInfo.Targets.back();
  FuncMeta.TargetDecl = FuncInfo.Definition;
  for (unsigned I = 0, EI = FuncInfo.Definition->getNumParams(); I < EI; ++I) {
    auto *PD = FuncInfo.Definition->getParamDecl(I);
    auto ReplacementItr = FuncInfo.Candidates.find(PD);
    if (ReplacementItr == FuncInfo.Candidates.end()) {
      FuncMeta.Parameters.emplace_back();
      FuncMeta.Parameters.back().TargetParam = I;
      FuncMeta.Parameters.back().IsPointer = false;
    } else {
      for (auto &R : ReplacementItr->get<Replacement>()) {
        FuncMeta.Parameters.emplace_back();
        FuncMeta.Parameters.back().TargetParam = I;
        FuncMeta.Parameters.back().TargetMember = cast<FieldDecl>(R.Member);
        FuncMeta.Parameters.back().IsPointer =
          FuncInfo.Strict || R.InAssignment;
      }
    }
  }
  LLVM_DEBUG(printMetadataLog(FuncInfo));
  replaceCalls(FuncInfo, mReplacementInfo, Canvas);
  // Remove pragmas from the original function and its clone if replacement
  // is still possible.
  Rewriter::RewriteOptions RemoveEmptyLine;
  /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
  /// set to true then removing (in RewriterBuffer) works incorrect.
  RemoveEmptyLine.RemoveLineIfEmpty = false;
  for (auto SR : FuncInfo.ToRemoveTransform) {
    Rewriter.RemoveText(SR, RemoveEmptyLine);
    Canvas.RemoveText(SR, true);
  }
  for (auto SR : FuncInfo.ToRemoveMetadata)
    Canvas.RemoveText(SR, true);
  for (auto SR : FuncInfo.ToRemoveClone)
    Canvas.RemoveText(SR, true);
  addPragmaMetadata(FuncInfo, SrcMgr, LangOpts, Canvas);
  // Update sources.
  auto OriginDefString = Lexer::getSourceText(
    CharSourceRange::getTokenRange(
      FuncInfo.Definition->getBeginLoc(),
          FuncInfo.Definition
              ->getParamDecl(FuncInfo.Definition->getNumParams() - 1)
              ->getEndLoc()),
    SrcMgr, LangOpts);
  auto LocToInsert = SrcMgr.getExpansionLoc(FuncInfo.Definition->getLocEnd());
  Rewriter.InsertTextAfterToken(
    LocToInsert,
    ("\n\n/* Replacement for " + OriginDefString + ") */\n").str());
  Rewriter.InsertTextAfterToken(LocToInsert, Canvas.getBuffer());
}

bool ClangStructureReplacementPass::runOnModule(llvm::Module &M) {
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
        ": transformation context is not available");
    return false;
  }
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  mRawInfo = &GIP.getRawInfo();
  mGlobalInfo = &GIP.getGlobalInfo();
  clang::CallGraph CG;
  CG.TraverseDecl(mTfmCtx->getContext().getTranslationUnitDecl());
  std::vector<scc_iterator<CallGraph *>> Postorder;
  for (auto I = scc_iterator<CallGraph *>::begin(&CG); !I.isAtEnd(); ++I)
    Postorder.push_back(I);
  LLVM_DEBUG(dbgs() << "[REPLACE]: number of SCCs " << Postorder.size() << "\n");
  LLVM_DEBUG(dbgs() << "[REPLACE]: traverse call graph in reverse postorder\n");
  for (auto &SCC: llvm::reverse(Postorder))
    collectCandidatesIn(SCC);
  auto &Rewriter = mTfmCtx->getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  if (SrcMgr.getDiagnostics().hasErrorOccurred())
    return false;
  LLVM_DEBUG(dbgs() << "[REPLACE]: traverse call graph in postorder\n");
  for (auto &SCC : Postorder) {
    LLVM_DEBUG(dbgs() << "[REPLACE]: process functions in SCC\n");
    for (auto *CGN : *SCC) {
      auto FuncInfo = tieCallGraphNode(CGN);
      if (!FuncInfo)
        continue;
      LLVM_DEBUG(printMetadataLog(*FuncInfo));
      LLVM_DEBUG(printCandidateLog(FuncInfo->Candidates, FuncInfo->Strict));
      LLVM_DEBUG(printRequestLog(*FuncInfo, SrcMgr));
      LLVM_DEBUG(printImplicitRequestLog(*FuncInfo, SrcMgr));
      if (!FuncInfo->hasCandidates()) {
        if (replaceCalls(*FuncInfo, mReplacementInfo, Rewriter)) {
          Rewriter::RewriteOptions RemoveEmptyLine;
          /// TODO (kaniandr@gmail.com): it seems that RemoveLineIfEmpty is
          /// set to true then removing (in RewriterBuffer) works incorrect.
          RemoveEmptyLine.RemoveLineIfEmpty = false;
          for (auto SR : FuncInfo->ToRemoveTransform)
            Rewriter.RemoveText(SR, RemoveEmptyLine);
        }
      } else {
        sanitizeCandidates(*FuncInfo);
      }
    }
    fillImplicitReplacementMembers(SCC);
    buildParameters(SCC);
    sanitizeCandidatesInCalls(SCC);
    insertNewFunctions(SCC);
  }
  return false;
}
