
// Sixgill: Static assertion checker for C/C++ programs.
// Copyright (C) 2009-2010  Stanford University
// Author: Brian Hackett
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "infer.h"

#include "expand.h"
#include "nullterm.h"
#include "invariant.h"

#include <imlang/storage.h>
#include <imlang/loopsplit.h>
#include <memory/baked.h>
#include <memory/escape.h>
#include <memory/storage.h>
#include <solve/solver.h>

// cutoff for arithmetic escape propagation.
#define ARITHMETIC_ESCAPE_LIMIT 50

NAMESPACE_XGILL_BEGIN

// escape structure for identifying accesses which use pointer arithmetic.
// TODO: there are two big holes in how we identify these writes currently:
// 1. only considering arithmetic within the current function. the following
//    code will slip by:
//    int* foo() { return &buf[n]; }
//    void main() { int *x = foo(); *x = 0; }
// 2. bad treatment of buffers of structures. the following code will slip by:
//    void main() { str *s = &buf[n]; s->f = 0; }
//    void main() { str *s = &buf[n]; int *x = &s->f; *x = 0; }
// these issues really need to get fixed.
class ArithmeticEscape : public EscapeStatus
{
 public:
  Variable *m_function;
  Vector<Exp*> &m_arithmetic_lvals;

  ArithmeticEscape(Variable *function,
                   Vector<Exp*> &arithmetic_lvals)
    : EscapeStatus(true, ARITHMETIC_ESCAPE_LIMIT),
      m_function(function), m_arithmetic_lvals(arithmetic_lvals)
  {}

  Trace* Visit(Trace *trace, bool *skip_cutoff)
  {
    if (trace->Kind() != TK_Func)
      return NULL;

    if (trace->GetFunction() != m_function)
      return NULL;

    // add the trace's lvalue to each of the summaries.
    Exp *exp = trace->GetValue();

    if (!m_arithmetic_lvals.Contains(exp)) {
      exp->IncRef(&m_arithmetic_lvals);
      m_arithmetic_lvals.PushBack(exp);
    }

    trace->IncRef();
    return trace;
  }
};

// if the specified assignment involves pointer arithmetic on the right side,
// propagate that arithmetic to the left side and everything it might flow to.
void ProcessArithmeticAssign(ArithmeticEscape *escape, BlockId *id,
                             Exp *left, Exp *right)
{
  // the expression has to be an ExpIndex for it to be arithmetic.
  // this is the result of operations like 'p++' if p is a pointer.
  if (right->IsIndex()) {

    // get the expression we will be running escape propagation on.
    // this is the deref of the left side of the assignment.
    left->IncRef();
    Exp *left_drf = Exp::MakeDrf(left);

    if (Trace *trace = Trace::MakeFromExp(id, left_drf)) {
      bool success = escape->FollowEscape(trace);
      trace->DecRef();

      if (!success) {
        logout << "WARNING: ProcessArithmeticAssign: "
               << "escape propagation failed" << endl;
      }
    }
  }
}

// visitor for adding assertions at each buffer access. for now buffer
// assertion generation is crude and only looks for ExpIndex and ExpDrf
// expressions whose value is ever that of the result of pointer arithmetic
// (according to any assignments in the function's CFGs).
class BufferScanVisitor : public ExpVisitor
{
 public:
  Vector<AssertInfo> &asserts;
  const Vector<Exp*> &arithmetic_list;
  PPoint point;
  bool check_writes;

  BufferScanVisitor(Vector<AssertInfo> &_asserts,
                    const Vector<Exp*> &_arithmetic_list,
                    PPoint _point, bool _check_writes)
    : ExpVisitor(VISK_Lval),
      asserts(_asserts), arithmetic_list(_arithmetic_list),
      point(_point), check_writes(_check_writes)
  {}

  void Visit(Exp *lval)
  {
    if (!lval->IsLvalue())
      return;

    // buffer which is being accessed.
    Exp *base = NULL;

    // element type of the buffer.
    Type *elem_type = NULL;

    // index of the buffer being accessed.
    Exp *index = NULL;

    // peel off any leading fields.
    while (ExpFld *nlval = lval->IfFld())
      lval = nlval->GetTarget();

    if (ExpIndex *nlval = lval->IfIndex()) {
      base = nlval->GetTarget();
      base->IncRef();

      elem_type = nlval->GetElementType();
      elem_type->IncRef();

      index = nlval->GetIndex();
      index->IncRef();

      if (base->IsIndex()) {
        // multidimensional array access, the base needs to be checked
        // if we are looking for reads.
        if (!check_writes)
          Visit(base);
      }
    }

    if (ExpDrf *nlval = lval->IfDrf()) {
      // see if this expression is in the values we know might be the result
      // of pointer arithmetic.
      bool is_arithmetic = false;

      if (Exp *new_lval = Trace::SanitizeExp(lval)) {
        is_arithmetic = arithmetic_list.Contains(new_lval);
        new_lval->DecRef();
      }

      if (is_arithmetic) {
        base = lval;
        base->IncRef();

        elem_type = nlval->GetType();
        if (elem_type) {
          elem_type->IncRef();
        }
        else {
          // TODO: need better handling for this case. *((int*)n)
          elem_type = Type::MakeVoid();
        }

        index = Exp::MakeInt(0);
      }
    }

    if (base || elem_type || index) {
      Assert(base && elem_type && index);

      // need two sets of references, for lower and upper bound tests.
      base->IncRef();
      elem_type->IncRef();
      index->IncRef();

      AssertInfo lower_info;
      AssertInfo upper_info;

      lower_info.kind = check_writes ? ASK_WriteUnderflow : ASK_ReadUnderflow;
      upper_info.kind = check_writes ? ASK_WriteOverflow : ASK_ReadOverflow;

      lower_info.cls = ASC_Check;
      upper_info.cls = ASC_Check;

      lower_info.point = point;
      upper_info.point = point;

      Exp *lbound = Exp::MakeBound(BND_Lower, base, elem_type);
      lower_info.bit = Exp::MakeCompareBit(B_GreaterEqual, index, lbound);

      Exp *ubound = Exp::MakeBound(BND_Upper, base, elem_type);
      upper_info.bit = Exp::MakeCompareBit(B_LessThan, index, ubound);

      // if we are looking for reads, only add if there is not already
      // an equivalent write (we add writes to the assert list first).
      // this will obscure the reads in statements like '(*p)++', oh well.

      bool skip_lower = false;
      bool skip_upper = false;

      // don't restrict just to reads here, so that this works as a general
      // purpose dupe-remover.
      for (size_t ind = 0; ind < asserts.Size(); ind++) {
        const AssertInfo &info = asserts[ind];
        if (info.point == point && info.bit == lower_info.bit)
          skip_lower = true;
        if (info.point == point && info.bit == upper_info.bit)
          skip_upper = true;
      }

      if (skip_lower) lower_info.bit->DecRef();
      else asserts.PushBack(lower_info);

      if (skip_upper) upper_info.bit->DecRef();
      else asserts.PushBack(upper_info);
    }
  }
};

// visitor for adding assertions at each integer operation which
// might overflow.
class IntegerScanVisitor : public ExpVisitor
{
 public:
  Vector<AssertInfo> &asserts;
  PPoint point;

  IntegerScanVisitor(Vector<AssertInfo> &_asserts, PPoint _point)
    : ExpVisitor(VISK_All), asserts(_asserts), point(_point)
  {}

  void Visit(Exp *exp)
  {
    size_t bits = exp->Bits();
    bool sign = exp->Sign();

    if (!bits)
      return;

    Assert(exp->IsUnop() || exp->IsBinop());

    AssertInfo lower_info;
    AssertInfo upper_info;

    lower_info.kind = ASK_IntegerUnderflow;
    upper_info.kind = ASK_IntegerOverflow;

    lower_info.cls = ASC_Check;
    upper_info.cls = ASC_Check;

    lower_info.point = point;
    upper_info.point = point;

    exp->IncRef();
    exp->IncRef();

    const char *min_value = GetMinimumInteger(bits, sign);
    const char *max_value = GetMaximumInteger(bits, sign);

    Exp *min_exp = Exp::MakeIntStr(min_value);
    Exp *max_exp = Exp::MakeIntStr(max_value);

    lower_info.bit = Exp::MakeCompareBit(B_GreaterEqual, exp, min_exp);
    upper_info.bit = Exp::MakeCompareBit(B_LessEqual, exp, max_exp);

    asserts.PushBack(lower_info);
    asserts.PushBack(upper_info);
  }
};

// mark the trivial/redundant assertions in the specified list.
void MarkRedundantAssertions(BlockMemory *mcfg, Vector<AssertInfo> &asserts)
{
  BlockCFG *cfg = mcfg->GetCFG();

  // assertions are marked redundant in two passes:
  // 1. for each path reaching the assertion, the validity of the assertion is
  //    implied by one or more prior or future assertions.
  //    this pass also picks up assertions which trivially hold, where the
  //    assertion is valid due to the conditions along the paths themselves.
  // 2. there is an isomorphic assertion within an inner loop. it is
  //    sufficient to check just the inner assertion.

  Solver *solver = new Solver("redundant");

  for (size_t ind = 0; ind < asserts.Size(); ind++) {
    AssertInfo &info = asserts[ind];
    solver->PushContext();

    Assert(info.cls == ASC_Check);

    // assert the guard at point.
    Bit *guard = mcfg->GetGuard(info.point);
    solver->AddAssert(0, guard);

    // assert the negation of the bit at point.

    info.bit->IncRef();
    Bit *not_bit = Bit::MakeNot(info.bit);

    Bit *result_not_bit;
    mcfg->TranslateBit(TRK_Point, info.point, not_bit, &result_not_bit);
    solver->AddAssert(0, result_not_bit);
    not_bit->DecRef();
    result_not_bit->DecRef(&result_not_bit);

    if (!solver->IsSatisfiable()) {
      // the assert is tautological or is proved by the guard, thus trivial.
      info.cls = ASC_Trivial;
      solver->PopContext();
      continue;
    }

    // assert the remaining assertions in the summary hold.
    for (size_t aind = 0; aind < asserts.Size(); aind++) {
      const AssertInfo &oinfo = asserts[aind];

      // skip this assertion itself.
      if (info.point == oinfo.point && info.bit == oinfo.bit)
        continue;

      // skip assertions already marked as trivial or redundant.
      if (oinfo.cls != ASC_Check)
        continue;

      // skip assertions for a different kind than the original.
      // this avoids interference between the different kinds of assertions,
      // though it is unlikely to affect whether we actually mark an
      // assert as redundant.
      if (oinfo.kind != info.kind)
        continue;

      // make an implication: guard at opoint => obit.

      Bit *other_guard = mcfg->GetGuard(oinfo.point);
      other_guard->IncRef();

      Bit *result_other_bit;
      mcfg->TranslateBit(TRK_Point, oinfo.point, oinfo.bit, &result_other_bit);
      result_other_bit->MoveRef(&result_other_bit, NULL);

      Bit *imply_bit = Bit::MakeImply(other_guard, result_other_bit);
      solver->AddAssert(0, imply_bit);
      imply_bit->DecRef();
    }

    if (!solver->IsSatisfiable()) {
      // the assert is implied by the remaining assertions, thus redundant.
      info.cls = ASC_Redundant;
    }

    solver->PopContext();
  }

  solver->Clear();
  delete solver;

  for (size_t ind = 0; ind < cfg->GetEdgeCount(); ind++) {
    PEdgeLoop *loop_edge = cfg->GetEdge(ind)->IfLoop();
    if (!loop_edge)
      continue;

    BlockCFG *loop_cfg = GetBlockCFG(loop_edge->GetLoopId());

    for (size_t aind = 0; aind < asserts.Size(); aind++) {
      AssertInfo &info = asserts[aind];

      if (info.cls != ASC_Check)
        continue;

      if (cfg->IsLoopIsomorphic(info.point)) {
        // this assertion's point is isomorphic to a point within the
        // loop body, so there will be an equivalent loop assertion.
        info.cls = ASC_Redundant;
      }
    }

    loop_cfg->DecRef();
  }
}

void InferSummaries(const Vector<BlockSummary*> &summary_list)
{
  static BaseTimer infer_timer("infer_summaries");
  Timer _timer(&infer_timer);

  if (summary_list.Empty())
    return;

  Variable *function = summary_list[0]->GetId()->BaseVar();
  Vector<BlockCFG*> *annot_list = BodyAnnotCache.Lookup(function->GetName());

  // all traces which might refer to the result of pointer arithmetic.
  Vector<Exp*> arithmetic_list;
  ArithmeticEscape escape(function, arithmetic_list);

  // initial pass over the CFGs to get traces used in pointer arithmetic.
  for (size_t ind = 0; ind < summary_list.Size(); ind++) {
    BlockSummary *sum = summary_list[ind];

    BlockCFG *cfg = sum->GetMemory()->GetCFG();
    for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
      PEdge *edge = cfg->GetEdge(eind);

      if (PEdgeAssign *assign_edge = edge->IfAssign()) {
        Exp *left = assign_edge->GetLeftSide();
        Exp *right = assign_edge->GetRightSide();
        ProcessArithmeticAssign(&escape, cfg->GetId(), left, right);
      }
    }
  }

  for (size_t ind = 0; ind < summary_list.Size(); ind++) {
    BlockSummary *sum = summary_list[ind];
    BlockMemory *mcfg = sum->GetMemory();
    BlockCFG *cfg = mcfg->GetCFG();

    // accumulate all the assertions at points in the CFG.
    Vector<AssertInfo> asserts;

    // add assertions at function exit for any postconditions.
    if (cfg->GetId()->Kind() == B_Function) {
      for (size_t aind = 0; annot_list && aind < annot_list->Size(); aind++) {
        BlockCFG *annot_cfg = annot_list->At(aind);

        if (annot_cfg->GetAnnotationKind() != AK_Postcondition)
          continue;
        if (Bit *bit = BlockMemory::GetAnnotationBit(annot_cfg)) {
          bit->IncRef();

          AssertInfo info;
          info.kind = ASK_Annotation;
          info.cls = ASC_Check;
          info.point = cfg->GetExitPoint();
          info.bit = bit;
          asserts.PushBack(info);
        }
      }
    }

    // add any intermediate annotated assertions.
    for (size_t pind = 0; pind < cfg->GetPointAnnotationCount(); pind++) {
      BlockPPoint point_annot = cfg->GetPointAnnotation(pind);

      BlockCFG *annot_cfg = NULL;
      for (size_t aind = 0; annot_list && aind < annot_list->Size(); aind++) {
        BlockCFG *test_cfg = annot_list->At(aind);
        if (test_cfg->GetId() == point_annot.id) {
          annot_cfg = test_cfg;
          break;
        }
      }
      if (!annot_cfg) continue;

      if (annot_cfg->GetAnnotationKind() != AK_Assert &&
          annot_cfg->GetAnnotationKind() != AK_AssertRuntime)
        continue;
      if (Bit *bit = BlockMemory::GetAnnotationBit(annot_cfg)) {
        bit->IncRef();

        AssertInfo info;
        info.kind = (annot_cfg->GetAnnotationKind() == AK_Assert)
          ? ASK_Annotation : ASK_AnnotationRuntime;
        info.cls = ASC_Check;
        info.point = point_annot.point;
        info.bit = bit;
        asserts.PushBack(info);
      }
    }

    for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
      PEdge *edge = cfg->GetEdge(eind);
      PPoint point = edge->GetSource();

      if (PEdgeCall *nedge = edge->IfCall()) {
        // add assertions for any callee preconditions.

        // pull preconditions from both direct and indirect calls.
        Vector<Variable*> callee_names;

        if (Variable *callee = nedge->GetDirectFunction()) {
          callee_names.PushBack(callee);
        }
        else {
          CallEdgeSet *callees = CalleeCache.Lookup(function);

          if (callees) {
            for (size_t eind = 0; eind < callees->GetEdgeCount(); eind++) {
              const CallEdge &edge = callees->GetEdge(eind);
              if (edge.where.id == cfg->GetId() && edge.where.point == point)
                callee_names.PushBack(edge.callee);
            }
          }
        }

        for (size_t cind = 0; cind < callee_names.Size(); cind++) {
          String *callee = callee_names[cind]->GetName();
          Vector<BlockCFG*> *call_annot_list = BodyAnnotCache.Lookup(callee);

          for (size_t aind = 0;
               call_annot_list && aind < call_annot_list->Size(); aind++) {
            BlockCFG *annot_cfg = call_annot_list->At(aind);

            if (annot_cfg->GetAnnotationKind() != AK_Precondition)
              continue;
            if (Bit *bit = BlockMemory::GetAnnotationBit(annot_cfg)) {
              ConvertCallsiteMapper mapper(cfg, point, false);
              Bit *caller_bit = bit->DoMap(&mapper);
              if (!caller_bit)
                continue;

              AssertInfo info;
              info.kind = ASK_Annotation;
              info.cls = ASC_Check;
              info.point = point;
              info.bit = caller_bit;
              asserts.PushBack(info);
            }
          }

          BodyAnnotCache.Release(callee);
        }

        if (!nedge->GetDirectFunction())
          CalleeCache.Release(function);
      }

      BufferScanVisitor write_visitor(asserts, arithmetic_list, point, true);
      BufferScanVisitor read_visitor(asserts, arithmetic_list, point, false);
      IntegerScanVisitor integer_visitor(asserts, point);

      // only look at the written lvalues for the write visitor.
      if (PEdgeAssign *assign = edge->IfAssign())
        write_visitor.Visit(assign->GetLeftSide());
      if (PEdgeCall *call = edge->IfCall()) {
        if (Exp *returned = call->GetReturnValue())
          write_visitor.Visit(returned);
      }

      edge->DoVisit(&read_visitor);

      // disable integer overflow visitor for now.
      // edge->DoVisit(&integer_visitor);
    }

    MarkRedundantAssertions(mcfg, asserts);

    // move the finished assertion list into the summary.
    for (size_t ind = 0; ind < asserts.Size(); ind++) {
      const AssertInfo &info = asserts[ind];
      sum->AddAssert(info.kind, info.cls, info.point, info.bit);
    }
  }

  // infer delta and termination invariants for all summaries.
  for (size_t ind = 0; ind < summary_list.Size(); ind++)
    InferInvariants(summary_list[ind], arithmetic_list);

  DecRefVector<Exp>(arithmetic_list, &arithmetic_list);
  BodyAnnotCache.Release(function->GetName());
}

NAMESPACE_XGILL_END
