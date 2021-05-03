//===- MemoryLocationRange.cpp ---- Memory Location Range -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
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
// This file provides utility analysis objects describing memory locations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include <bcl/Equation.h>
#include <llvm/Support/Debug.h>

using namespace tsar;

typedef MemoryLocationRange::Dimension Dimension;

namespace {
/// Finds difference between dimensions D and I where I is a subset of D
/// and adds results to Res. Return `false` if Threshold is exceeded, `true`
/// otherwise.
bool difference(const Dimension &D, const Dimension &I,
                llvm::SmallVectorImpl<Dimension> &Res, std::size_t Threshold) {
  if (D.Start < I.Start) {
    auto &Left = Res.emplace_back();
    Left.Step = D.Step;
    Left.Start = D.Start;
    Left.TripCount = (I.Start - D.Start) / D.Step;
    Left.DimSize = D.DimSize;
  }
  auto DEnd = D.Start + D.Step * (D.TripCount - 1);
  auto IEnd = I.Start + I.Step * (I.TripCount - 1);
  if (DEnd > IEnd) {
    auto &Right = Res.emplace_back();
    Right.Step = D.Step;
    Right.Start = IEnd + D.Step;
    Right.TripCount = (DEnd - IEnd) / D.Step;
    Right.DimSize = D.DimSize;
  }
  if (I.TripCount > 1) {
    // I.Step % D.Step is always 0 because I is a subset of D.
    auto RepeatNumber = I.Step / D.Step - 1;
    if (RepeatNumber > Threshold)
      return false;
    for (auto J = 0; J < RepeatNumber; ++J) {
      auto &Center = Res.emplace_back();
      Center.Start = I.Start + D.Step * (J + 1);
      Center.Step = D.Step;
      Center.TripCount = I.TripCount - 1;
      Center.DimSize = D.DimSize;
    }
  }
  return true;
}

void printSolutionInfo(llvm::raw_ostream &OS,
    const MemoryLocationRange &Int,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  auto PrintRange = [&OS](const MemoryLocationRange &R) {
    auto &Dim = R.DimList[0];
    OS << "{" << (R.Ptr == nullptr ? "Empty" : "Full") << " | ";
    OS << Dim.Start << " + " << Dim.Step << " * T, T in [" << 0 <<
        ", " << Dim.TripCount << "), DimSize: " << Dim.DimSize << "} ";
  };
  OS << "\n[EQUATION] Solution:\n";
  OS << "Left: ";
  if (LC)
    for (auto &R : *LC) {
      PrintRange(R);
    }
  OS << "\nIntersection: ";
  PrintRange(Int);
  OS << "\nRight: ";
  if (RC)
    for (auto &R : *RC) {
      PrintRange(R);
    }
  OS << "\n[EQUATION] Solution has been printed.\n";
}

void delinearize(const MemoryLocationRange &From, MemoryLocationRange &What) {
  typedef MemoryLocationRange::LocKind LocKind;
  if (What.Kind != LocKind::DEFAULT || From.Kind != LocKind::COLLAPSED)
    return;
  if (!What.LowerBound.hasValue() || !What.UpperBound.hasValue())
    return;
  auto Lower = What.LowerBound.getValue();
  auto Upper = What.UpperBound.getValue();
  if (Lower >= Upper)
    return;
  const auto DimN = From.DimList.size();
  if (DimN == 0)
    return;
  assert(From.UpperBound.hasValue() &&
      "UpperBound of a collapsed array location must have a value!");
  auto ElemSize = From.UpperBound.getValue();
  std::vector<uint64_t> SizesInBytes(DimN + 1, 0);
  if (Lower % ElemSize != 0 || Upper % ElemSize != 0)
    return;
  SizesInBytes.back() = ElemSize;
  for (int64_t DimIdx = DimN - 1; DimIdx >= 0; DimIdx--) {
    SizesInBytes[DimIdx] = From.DimList[DimIdx].DimSize *
                            SizesInBytes[DimIdx + 1];
    if (SizesInBytes[DimIdx] == 0)
      assert(DimIdx == 0 && "Collapsed memory location should not contain "
          "dimensions of size 0, except for the 0th dimension.");
  }
  std::vector<uint64_t> LowerIdx(DimN, 0), UpperIdx(DimN, 0);
  for (std::size_t I = 0; I < DimN; ++I) {
    auto CurrSize = SizesInBytes[I], NextSize = SizesInBytes[I + 1];
    LowerIdx[I] = CurrSize > 0 ? (Lower % CurrSize) / NextSize :
                                  Lower / NextSize;
    UpperIdx[I] = CurrSize > 0 ?  ((Upper - 1) % CurrSize) / NextSize :
                                  (Upper - 1) / NextSize;
  }
  if (LowerIdx[DimN - 1] == 0 &&
      UpperIdx[DimN - 1] + 1 == From.DimList[DimN - 1].DimSize) {
    for (std::size_t I = 1; I < DimN - 1; ++I)
      if (LowerIdx[I] != 0 || UpperIdx[I] + 1 != From.DimList[I].DimSize)
        return;
    What.DimList.resize(DimN);
    for (std::size_t I = 0; I < DimN; ++I) {
      auto &Dim = What.DimList[I];
      Dim.Start = LowerIdx[I];
      Dim.Step = 1;
      Dim.TripCount = UpperIdx[I] - LowerIdx[I] + 1;
      Dim.DimSize = From.DimList[I].DimSize;
    }
  } else {
    for (std::size_t I = 0; I < DimN - 1; ++I)
      if (LowerIdx[I] != UpperIdx[I])
        return;
    What.DimList.resize(DimN);
    for (std::size_t I = 0; I < DimN; ++I) {
      auto &Dim = What.DimList[I];
      Dim.Start = LowerIdx[I];
      Dim.Step = 1;
      Dim.TripCount = UpperIdx[I] - LowerIdx[I] + 1;
      Dim.DimSize = From.DimList[I].DimSize;
    }
  }
  What.Kind = LocKind::COLLAPSED;
}

llvm::Optional<MemoryLocationRange> intersectScalar(
    MemoryLocationRange LHS,
    MemoryLocationRange RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC) {
  typedef MemoryLocationRange::LocKind LocKind;
  if (LHS.Ptr != RHS.Ptr)
    return llvm::None;
  assert(LHS.Kind != LocKind::COLLAPSED && RHS.Kind != LocKind::COLLAPSED &&
      "It is forbidden to calculate an intersection between non-scalar "
      "variables!");
  if (!LHS.LowerBound.hasValue() || !LHS.UpperBound.hasValue() ||
      !RHS.LowerBound.hasValue() || !RHS.UpperBound.hasValue()) {
    if ((LHS.UpperBound.hasValue() && RHS.LowerBound.hasValue() &&
         LHS.UpperBound.getValue() <= RHS.LowerBound.getValue()) ||
        (LHS.LowerBound.hasValue() && RHS.UpperBound.hasValue() &&
         LHS.LowerBound.getValue() >= RHS.UpperBound.getValue())) 
      return llvm::None;
    return MemoryLocationRange();
  }
  if (LHS.UpperBound.getValue() > RHS.LowerBound.getValue() &&
      LHS.LowerBound.getValue() < RHS.UpperBound.getValue()) {
    MemoryLocationRange Int(LHS);
    Int.LowerBound = std::max(LHS.LowerBound.getValue(),
                              RHS.LowerBound.getValue());
    Int.UpperBound = std::min(LHS.UpperBound.getValue(),
                              RHS.UpperBound.getValue());
    if (LC) {
      if (LHS.LowerBound.getValue() < Int.LowerBound.getValue())
        LC->emplace_back(LHS).UpperBound = Int.LowerBound.getValue();
      if (LHS.UpperBound.getValue() > Int.UpperBound.getValue())
        LC->emplace_back(LHS).LowerBound = Int.UpperBound.getValue();
    }
    if (RC) {
      if (RHS.LowerBound.getValue() < Int.LowerBound.getValue())
        RC->emplace_back(RHS).UpperBound = Int.LowerBound.getValue();
      if (RHS.UpperBound.getValue() > Int.UpperBound.getValue())
        RC->emplace_back(RHS).LowerBound = Int.UpperBound.getValue();
    }
    return Int;
  }
  return llvm::None;
}
};

llvm::Optional<MemoryLocationRange> MemoryLocationRangeEquation::intersect(
    MemoryLocationRange LHS,
    MemoryLocationRange RHS,
    llvm::SmallVectorImpl<MemoryLocationRange> *LC,
    llvm::SmallVectorImpl<MemoryLocationRange> *RC,
    std::size_t Threshold) {
  typedef milp::BAEquation<ColumnT, ValueT> BAEquation;
  typedef BAEquation::Monom Monom;
  typedef milp::BinomialSystem<ColumnT, ValueT, 0, 0, 1> LinearSystem;
  typedef std::pair<ValueT, ValueT> VarRange;
  typedef MemoryLocationRange::LocKind LocKind;
  assert(LHS.Ptr && RHS.Ptr &&
      "Pointers of intersected memory locations must not be null!");
  // Return a location that may be an intersection, but cannot be calculated 
  // exactly.
  if (LHS.Ptr != RHS.Ptr)
    return llvm::None;
  if (LHS.Kind == LocKind::DEFAULT && RHS.Kind == LocKind::COLLAPSED)
    delinearize(RHS, LHS);
  else if (RHS.Kind == LocKind::DEFAULT && LHS.Kind == LocKind::COLLAPSED)
    delinearize(LHS, RHS);
  if (LHS.Kind != LocKind::COLLAPSED && RHS.Kind != LocKind::COLLAPSED)
    return intersectScalar(LHS, RHS, LC, RC);
  if (LHS.Kind != LocKind::COLLAPSED || RHS.Kind != LocKind::COLLAPSED)
    return MemoryLocationRange();
  if (LHS.DimList.size() != RHS.DimList.size())
    return MemoryLocationRange();
  if (LHS.LowerBound == RHS.LowerBound && LHS.UpperBound == RHS.UpperBound &&
      LHS.DimList == RHS.DimList)
    return LHS;
  bool Intersected = true;
  MemoryLocationRange Int(LHS);
  for (std::size_t I = 0; I < LHS.DimList.size(); ++I) {
    auto &Left = LHS.DimList[I];
    auto &Right = RHS.DimList[I];
    if (Left.DimSize != Right.DimSize)
      return MemoryLocationRange();
    auto LeftEnd = Left.Start + Left.Step * (Left.TripCount - 1);
    auto RightEnd = Right.Start + Right.Step * (Right.TripCount - 1);
    if (LeftEnd < Right.Start || RightEnd < Left.Start) {
      Intersected = false;
      break;
    }
    ColumnInfo Info;
    assert(Left.Start >= 0 && Right.Start >= 0 && "Start must be non-negative!");
    // We guarantee that K1 and K2 will not be equal to 0.
    assert(Left.Step > 0 && Right.Step > 0 && "Steps must be positive!");
    assert(Left.TripCount > 0 && Right.TripCount > 0 &&
        "Trip count must be positive!");
    ValueT L1 = Left.Start, K1 = Left.Step;
    ValueT L2 = Right.Start, K2 = Right.Step;
    VarRange XRange(0, Left.TripCount - 1), YRange(0, Right.TripCount - 1);
    LinearSystem System;
    System.push_back(Monom(0, K1), Monom(1, -K2), L2 - L1);
    System.instantiate(Info);
    auto SolutionNumber = System.solve<ColumnInfo, false>(Info);
    if (SolutionNumber == 0) {
      Intersected = false;
      break;
    }
    auto &Solution = System.getSolution();
    auto &LineX = Solution[0], &LineY = Solution[1];
    // B will be equal to 0 only if K1 is equal to 0 but K1 is always positive.
    ValueT A = LineX.Constant, B = -LineX.RHS.Value;
    // D will be equal to 0 only if K2 is equal to 0 but K2 is always positive.
    ValueT C = LineY.Constant, D = -LineY.RHS.Value;
    assert(B > 0 && "B must be positive!");
    ValueT TXmin = std::ceil((XRange.first - A) / double(B));
    ValueT TXmax = std::floor((XRange.second - A) / double(B));
    assert(D > 0 && "D must be positive!");
    ValueT TYmin = std::ceil((YRange.first - C) / double(D));
    ValueT TYmax = std::floor((YRange.second - C) / double(D));
    ValueT Tmin = std::max(TXmin, TYmin);
    ValueT Tmax = std::min(TXmax, TYmax);
    if (Tmax < Tmin) {
      Intersected = false;
      break;
    }
    ValueT Shift = Tmin;
    Tmin = 0;
    Tmax -= Shift;
    ValueT Step = K1 * B;
    ValueT Start = (K1 * A + L1) + Step * Shift;
    auto &Intersection = Int.DimList[I];
    Intersection.Start = Start;
    Intersection.Step = Step;
    Intersection.TripCount = Tmax + 1;
    Intersection.DimSize = Left.DimSize;
    assert(Start >= 0 && "Start must be non-negative!");
    assert(Step > 0 && "Step must be positive!");
    assert(Intersection.TripCount > 0 && "Trip count must be non-negative!");
    if (LC) {
      llvm::SmallVector<Dimension, 3> ComplLeft;
      if (!difference(Left, Intersection, ComplLeft, Threshold)) {
        auto &IncLoc = LC->emplace_back(LHS);
        IncLoc.DimList.clear();
        IncLoc.Kind = LocKind::NON_COLLAPSABLE;
      } else {
        for (auto &Comp : ComplLeft) {
          auto &NewLoc = LC->emplace_back(LHS);
          NewLoc.DimList[I] = Comp;
        }
      }
    }
    if (RC) {
      llvm::SmallVector<Dimension, 3> ComplRight;
      if (!difference(Right, Intersection, ComplRight, Threshold)) {
        auto &IncLoc = RC->emplace_back(RHS);
        IncLoc.DimList.clear();
        IncLoc.Kind = LocKind::NON_COLLAPSABLE;
      } else {
        for (auto &Comp : ComplRight) {
          auto &NewLoc = RC->emplace_back(RHS);
          NewLoc.DimList[I] = Comp;
        }
      }
    }
  }
  if (!Intersected)
    return llvm::None;
  //printSolutionInfo(llvm::dbgs(), Int, LC, RC);
  return Int;
}
