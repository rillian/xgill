
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

#include "clobber.h"
#include "block.h"
#include "storage.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// MemoryClobber static
/////////////////////////////////////////////////////////////////////

// list of all MemoryClobber structures, indexed by their kind.
static Vector<MemoryClobber*> *g_clobber_list = NULL;

MemoryClobber* MemoryClobber::Lookup(MemoryClobberKind kind)
{
  Assert(g_clobber_list != NULL);

  size_t ind = (size_t) kind;
  Assert(ind < g_clobber_list->Size());
  Assert(g_clobber_list->At(ind) != NULL);

  return g_clobber_list->At(ind);
}

void MemoryClobber::Register(MemoryClobber *clobber)
{
  // this is called during static initialization so make sure
  // g_clobber_list is setup properly.
  static bool initialized_clobber_list = false;
  if (!initialized_clobber_list) {
    g_clobber_list = new Vector<MemoryClobber*>();
    initialized_clobber_list = true;
  }

  size_t ind = (size_t) clobber->m_kind;
  while (ind >= g_clobber_list->Size())
    g_clobber_list->PushBack(NULL);

  Assert(g_clobber_list->At(ind) == NULL);
  g_clobber_list->At(ind) = clobber;
}

MemoryClobber mclobber_Default(MCLB_Default);

/////////////////////////////////////////////////////////////////////
// MemoryClobberModset
/////////////////////////////////////////////////////////////////////

class MemoryClobberModset : public MemoryClobber
{
 public:
  bool m_indirect;
  MemoryClobberModset(bool indirect)
    : MemoryClobber(indirect ? MCLB_Modset : MCLB_ModsetNoIndirect),
      m_indirect(indirect) {}

  void ComputeClobber(BlockMemory *mcfg, PEdge *edge,
                      Vector<GuardAssign> *assigns,
                      Vector<GuardAssign> *clobbered)
  {
    // only clobbering values written by direct calls and loops.
    PPoint point = edge->GetSource();

    // get the direct and indirect callees of the edge.
    Vector<BlockId*> callees;
    bool is_direct = false;

    if (BlockId *callee = edge->GetDirectCallee()) {
      is_direct = true;
      callees.PushBack(callee);
    }
    else if (edge->IsCall() && m_indirect) {
      Variable *function = mcfg->GetId()->BaseVar();
      CallEdgeSet *indirect_callees = CalleeCache.Lookup(function);

      if (indirect_callees) {
        for (size_t ind = 0; ind < indirect_callees->GetEdgeCount(); ind++) {
          const CallEdge &edge = indirect_callees->GetEdge(ind);

          if (edge.where == BlockPPoint(mcfg->GetId(), point)) {
            edge.callee->IncRef();
            BlockId *callee = BlockId::Make(B_Function, edge.callee);
            callees.PushBack(callee);
          }
        }
      }

      CalleeCache.Release(function);
    }

    for (size_t cind = 0; cind < callees.Size(); cind++) {
      BlockId *callee = callees[cind];
      BlockModset *modset = GetBlockModset(callee);

      // fill the assigns in the caller, but only for direct calls.
      if (is_direct) {
        for (size_t ind = 0; ind < modset->GetAssignCount(); ind++) {
          const GuardAssign &gts = modset->GetAssign(ind);
          mcfg->TranslateAssign(TRK_Callee, point, NULL,
                                gts.left, gts.right, gts.guard, assigns);
        }
      }

      // fill the modsets in the caller.
      for (size_t ind = 0; ind < modset->GetModsetCount(); ind++) {
        const PointValue &cv = modset->GetModsetLval(ind);

        GuardExpVector caller_res;
        mcfg->TranslateExp(TRK_Callee, point, cv.lval, &caller_res);

        for (size_t ind = 0; ind < caller_res.Size(); ind++) {
          const GuardExp &gt = caller_res[ind];
          if (!gt.guard->IsTrue())
            gt.guard->IncRef(clobbered);
          gt.exp->IncRef(clobbered);
          cv.lval->IncRef(clobbered);
          if (cv.kind)
            cv.kind->IncRef(clobbered);

          GuardAssign gti(gt.exp, cv.lval, gt.guard, cv.kind);
          clobbered->PushBack(gti);
        }
      }

      modset->DecRef();
      callee->DecRef();
    }
  }
};

MemoryClobberModset msimp_Modset(true);
MemoryClobberModset msimp_ModsetNoIndirect(false);

NAMESPACE_XGILL_END
