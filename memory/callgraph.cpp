
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

#include "callgraph.h"
#include "serial.h"
#include "storage.h"
#include "baked.h"
#include <imlang/storage.h>

NAMESPACE_XGILL_BEGIN

ConfigOption print_indirect_calls(CK_Flag, "print-indirect", NULL,
                                  "print generated indirect calls");

/////////////////////////////////////////////////////////////////////
// CallEdgeSet static
/////////////////////////////////////////////////////////////////////

HashCons<CallEdgeSet> CallEdgeSet::g_table;

int CallEdgeSet::Compare(const CallEdgeSet *cset0, const CallEdgeSet *cset1)
{
  TryCompareObjects(cset0->GetFunction(), cset1->GetFunction(), Variable);
  TryCompareValues((int)cset0->IsCallers(), (int)cset1->IsCallers());
  return 0;
}

CallEdgeSet* CallEdgeSet::Copy(const CallEdgeSet *cset)
{
  return new CallEdgeSet(*cset);
}

void CallEdgeSet::Write(Buffer *buf, const CallEdgeSet *cset)
{
  Assert(cset->m_edges);
  WriteMerge(buf, cset->m_function, cset->m_callers, *(cset->m_edges));
}

CallEdgeSet* CallEdgeSet::Read(Buffer *buf)
{
  Variable *function = NULL;
  bool callers = false;
  Vector<CallEdge> edges;

  ReadMerge(buf, &function, &callers, &edges);

  CallEdgeSet *res = Make(function, callers);
  Assert(res->GetEdgeCount() == 0);

  for (size_t eind = 0; eind < edges.Size(); eind++)
    res->AddEdge(edges[eind]);
  return res;
}

void CallEdgeSet::WriteMerge(Buffer *buf,
                             Variable *function, bool callers,
                             const Vector<CallEdge> &edges)
{
  WriteOpenTag(buf, TAG_CallEdgeSet);
  Variable::Write(buf, function);
  WriteTagEmpty(buf, callers ? TAG_True : TAG_False);

  for (size_t eind = 0; eind < edges.Size(); eind++) {
    WriteOpenTag(buf, TAG_CallEdge);
    BlockPPoint::Write(buf, edges[eind].where);
    Variable::Write(buf, edges[eind].callee);
    WriteCloseTag(buf, TAG_CallEdge);
  }

  WriteCloseTag(buf, TAG_CallEdgeSet);
}

void CallEdgeSet::ReadMerge(Buffer *buf,
                            Variable **pfunction, bool *pcallers,
                            Vector<CallEdge> *pedges)
{
  Try(ReadOpenTag(buf, TAG_CallEdgeSet));
  while (!ReadCloseTag(buf, TAG_CallEdgeSet)) {
    switch (PeekOpenTag(buf)) {
    case TAG_Variable: {
      Try(*pfunction == NULL);
      *pfunction = Variable::Read(buf);
      break;
    }
    case TAG_True: {
      Try(ReadTagEmpty(buf, TAG_True));
      *pcallers = true;
      break;
    }
    case TAG_False: {
      Try(ReadTagEmpty(buf, TAG_False));
      *pcallers = false;
      break;
    }
    case TAG_CallEdge: {
      Try(ReadOpenTag(buf, TAG_CallEdge));

      BlockPPoint where = BlockPPoint::Read(buf);
      Variable *callee = Variable::Read(buf);
      pedges->PushBack(CallEdge(where, callee));

      Try(ReadCloseTag(buf, TAG_CallEdge));
      break;
    }
    default:
      BadTag(PeekOpenTag(buf));
    }
  }

  Try(*pfunction != NULL);
  Try(!pedges->Empty());
}

/////////////////////////////////////////////////////////////////////
// CallEdgeSet
/////////////////////////////////////////////////////////////////////

CallEdgeSet::CallEdgeSet(Variable *function, bool callers)
  : m_function(function), m_callers(callers), m_edges(NULL)
{
  Assert(m_function);
  m_hash = m_function->Hash();
  m_hash = Hash32(m_hash, m_callers);
}

size_t CallEdgeSet::GetEdgeCount() const
{
  if (m_edges == NULL)
    return 0;
  return m_edges->Size();
}

const CallEdge& CallEdgeSet::GetEdge(size_t ind) const
{
  Assert(m_edges != NULL);
  return m_edges->At(ind);
}

void CallEdgeSet::AddEdge(const CallEdge &edge)
{
  if (m_edges == NULL)
    m_edges = new Vector<CallEdge>();

  edge.where.id->MoveRef(NULL, this);
  edge.callee->MoveRef(NULL, this);
  m_edges->PushBack(edge);
}

void CallEdgeSet::Print(OutStream &out) const
{
  out << "Call edge set"
      << (m_callers ? " [callers]" : " [callees]")
      << ": " << m_function << endl;

  if (m_edges != NULL) {
    for (size_t eind = 0; eind < m_edges->Size(); eind++) {
      const CallEdge &edge = m_edges->At(eind);

      out << "  " << edge.where.id << ":" << edge.where.point
          << " -> " << edge.callee << endl;
    }
  }
}

void CallEdgeSet::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_function->DecMoveRef(ov, nv);

  if (m_edges != NULL) {
    Assert(ov == this && nv == NULL);

    for (size_t eind = 0; eind < m_edges->Size(); eind++) {
      const CallEdge &edge = m_edges->At(eind);

      edge.where.id->DecRef(this);
      edge.callee->DecRef(this);
    }
  }
}

void CallEdgeSet::Persist()
{
  Assert(m_edges == NULL);
}

void CallEdgeSet::UnPersist()
{
  if (m_edges != NULL)
    delete m_edges;
}

/////////////////////////////////////////////////////////////////////
// Callgraph computation
/////////////////////////////////////////////////////////////////////

// add to the append callgraph caches the call edges resulting from the
// specified call invoking the specified callee (directly or indirectly).
void CallgraphProcessCall(BlockCFG *cfg, PEdgeCall *edge, Variable *callee)
{
  Assert(callee->IsGlobal());

  BlockPPoint where(cfg->GetId(), edge->GetSource());
  Variable *caller = where.id->BaseVar();

  // add the caller edge to the cache.

  Vector<CallEdgeSet*> *caller_entries =
    g_pending_callers.Lookup(callee, true);
  if (caller_entries->Empty()) {
    callee->IncRef();
    caller_entries->PushBack(CallEdgeSet::Make(callee, true));
  }

  CallEdgeSet *caller_cset = caller_entries->At(0);

  where.id->IncRef();
  callee->IncRef();
  caller_cset->AddEdge(CallEdge(where, callee));

  // add the callee edge to the cache.

  Vector<CallEdgeSet*> *callee_entries =
    g_pending_callees.Lookup(caller, true);
  if (callee_entries->Empty()) {
    caller->IncRef();
    callee_entries->PushBack(CallEdgeSet::Make(caller, false));
  }

  CallEdgeSet *callee_cset = callee_entries->At(0);

  where.id->IncRef();
  callee->IncRef();
  callee_cset->AddEdge(CallEdge(where, callee));
}

void CallgraphProcessCFG(BlockCFG *cfg, Vector<Variable*> *callees,
                         bool *indirect)
{
  for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
    PEdge *edge = cfg->GetEdge(eind);
    if (PEdgeCall *nedge = edge->IfCall()) {

      // watch out for 'direct' calls to local variables, resulting from the
      // frontend not being able to figure out the function being referred to.
      Variable *callee = nedge->GetDirectFunction();
      if (callee && callee->IsGlobal()) {
        CallgraphProcessCall(cfg, nedge, callee);

        if (!callees->Contains(callee))
          callees->PushBack(callee);
      }

      if (!callee)
        *indirect = true;
    }
  }
}

// maximum number of locations to propagate to when finding targets
// for indirect calls. this does not include the function pointer
// targets themselves.
#define FUNPTR_ESCAPE_LIMIT  100

// if trace represents a global function then get that function.
Variable* GetTraceFunction(Trace *trace)
{
  if (trace->Kind() == TK_Glob) {
    if (ExpVar *exp = trace->GetValue()->IfVar()) {
      Variable *var = exp->GetVariable();

      if (var->Kind() == VK_Func)
        return var;
    }
  }

  return NULL;
}

class FunctionPointerEscape : public EscapeStatus
{
 public:
  BlockCFG *m_cfg;
  PEdgeCall *m_edge;
  Vector<Variable*> *m_callees;
  bool m_found;

  FunctionPointerEscape(BlockCFG *cfg, PEdgeCall *edge,
                        Vector<Variable*> *callees)
    : EscapeStatus(false, FUNPTR_ESCAPE_LIMIT),
      m_cfg(cfg), m_edge(edge), m_callees(callees), m_found(false)
  {}

  Trace* Visit(Trace *trace, bool *skip_cutoff)
  {
    // handle discovery of a specific function as the call target.
    if (Variable *function = GetTraceFunction(trace)) {
      if (print_indirect_calls.IsSpecified())
        logout << "Indirect: " << m_cfg->GetId() << ": " << m_edge->GetSource()
               << ": " << function << endl;

      // check to see if there is a mismatch in the number of arguments
      // between the call edge and target function. we don't use a more
      // aggressive notion of mismatch, which can run into trouble with casts.

      function->IncRef();
      BlockId *callee_id = BlockId::Make(B_Function, function);
      BlockCFG *callee = GetBlockCFG(callee_id);

      bool mismatch = false;
      if (callee) {
        // count the arguments by finding the argument local with the
        // highest index.
        size_t arg_count = 0;

        const Vector<DefineVariable> *vars = callee->GetVariables();
        for (size_t ind = 0; vars && ind < vars->Size(); ind++) {
          Variable *var = vars->At(ind).var;
          if (var->Kind() == VK_Arg && var->GetIndex() >= arg_count)
            arg_count = var->GetIndex() + 1;
        }

        if (arg_count != m_edge->GetArgumentCount())
          mismatch = true;
        callee->DecRef();
      }
      callee_id->DecRef();

      // discard the call edge in case of a mismatch.
      if (mismatch) {
        logout << "WARNING: Discarded mismatched indirect call: "
               << m_cfg->GetId() << ": " << m_edge->GetSource()
               << ": " << function << endl;
      }
      else {
        if (!m_callees->Contains(function)) {
          function->IncRef();
          m_callees->PushBack(function);
        }

        CallgraphProcessCall(m_cfg, m_edge, function);
        m_found = true;
      }
    }

    Vector<Trace*> matches;
    trace->GetMatches(&matches);

    // we just want the first one, the least specific.
    Assert(matches.Size() > 0);
    Trace *res = matches[0];
    res->IncRef();

    for (size_t ind = 0; ind < matches.Size(); ind++)
      matches[ind]->DecRef(&matches);

    if (GetTraceFunction(res) != NULL) {
      // don't count functions against the escape propagation
      // cutoff, so that we can find any number of function pointers
      // as long as the paths to them are short.
      *skip_cutoff = true;
    }

    return res;
  }
};

void CallgraphProcessCFGIndirect(BlockCFG *cfg, Vector<Variable*> *callees)
{
  static BaseTimer indirect_timer("cfg_indirect");
  Timer _timer(&indirect_timer);

  for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
    PEdgeCall *edge = cfg->GetEdge(eind)->IfCall();
    if (!edge) continue;

    Variable *callee = edge->GetDirectFunction();
    if (callee != NULL) {
      // this is a direct call and we generated the edge for it already.
      continue;
    }

    FunctionPointerEscape escape(cfg, edge, callees);
    Exp *function = edge->GetFunction();

    // source we will propagate backwards from to get indirect targets.
    Trace *source = NULL;

    if (edge->GetInstanceObject()) {
      // virtual call through an object. we've encoded a class hierarchy
      // in the escape edges so walk this to get the possible targets.

      // get the supertype to use from the callsite's signature.
      TypeCSU *csu_type = edge->GetType()->GetCSUType();

      if (csu_type) {
        String *csu_name = csu_type->GetCSUName();

        if (IgnoreType(csu_name)) {
          logout << "WARNING: Ignoring indirect call: " << edge << endl;
          continue;
        }

        csu_name->IncRef();

        function->IncRef();
        source = Trace::MakeComp(function, csu_name);
      }
    }
    else {
      // indirect call through a function pointer.
      function->IncRef();
      source = Trace::MakeFromExp(cfg->GetId(), function);
    }

    bool success = false;

    if (source) {
      success = escape.FollowEscape(source);
      source->DecRef();
    }

    if (!success) {
      logout << "WARNING: Incomplete function pointer propagation: "
             << edge << endl;
    }

    if (!escape.m_found) {
      logout << "WARNING: No indirect targets found: "
             << cfg->GetId() << ": " << edge << endl;
    }
  }
}

NAMESPACE_XGILL_END
