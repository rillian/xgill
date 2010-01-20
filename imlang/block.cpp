
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

#include "block.h"
#include "storage.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// BlockId static
/////////////////////////////////////////////////////////////////////

HashCons<BlockId> BlockId::g_table;

int BlockId::Compare(const BlockId *b0, const BlockId *b1)
{
  TryCompareValues(b0->Kind(), b1->Kind());
  TryCompareObjects(b0->BaseVar(), b1->BaseVar(), Variable);
  TryCompareObjects(b0->Loop(), b1->Loop(), String);
  return 0;
}

BlockId* BlockId::Copy(const BlockId *b)
{
  return new BlockId(*b);
}

void BlockId::Write(Buffer *buf, const BlockId *b)
{
  WriteOpenTag(buf, TAG_BlockId);
  WriteTagUInt32(buf, TAG_Kind, b->Kind());
  Variable::Write(buf, b->BaseVar());
  if (b->Loop() != NULL)
    String::Write(buf, b->Loop());
  WriteCloseTag(buf, TAG_BlockId);
}

BlockId* BlockId::Read(Buffer *buf)
{
  uint32_t kind = 0;
  Variable *var = NULL;
  String *loop = NULL;

  Try(ReadOpenTag(buf, TAG_BlockId));
  Try(ReadTagUInt32(buf, TAG_Kind, &kind));
  var = Variable::Read(buf);
  if (!ReadCloseTag(buf, TAG_BlockId)) {
    loop = String::Read(buf);
    Try(ReadCloseTag(buf, TAG_BlockId));
  }

  return Make((BlockKind)kind, var, loop);
}

/////////////////////////////////////////////////////////////////////
// BlockId
/////////////////////////////////////////////////////////////////////

BlockId::BlockId(BlockKind kind, Variable *var, String *loop)
  : m_kind(kind), m_var(var), m_loop(loop)
{
  Assert(m_var);
  switch (m_kind) {
  case B_FunctionWhole:
  case B_Function:
  case B_Initializer:
    Assert(!m_loop);
    break;
  case B_Loop:
  case B_AnnotationFunc:
  case B_AnnotationInit:
  case B_AnnotationComp:
    Assert(m_loop);
    break;
  case B_Scratch:
    break;
  default:
    Assert(false);
  }

  m_hash = Hash32(m_kind, m_var->Hash());
  if (m_loop)
    m_hash = Hash32(m_hash, m_loop->Hash());
}

const char* BlockId::LoopName() const
{
  static char loop_buf[100];
  Assert(m_kind == B_Loop);

  // parse the line number from the 'loop:point:line' format.
  const char *line = m_loop->Value();

  line = strchr(line, ':');
  Assert(line);

  line = strchr(line + 1, ':');
  Assert(line);

  snprintf(loop_buf, sizeof(loop_buf), "loop:%s", line + 1);
  return loop_buf;
}

void BlockId::Print(OutStream &out) const
{
  out << m_var->GetName()->Value();
  switch (m_kind) {
  case B_FunctionWhole:
    out << ":whole"; break;
  case B_Function: break;
  case B_Loop:
    out << ":" << m_loop->Value(); break;
  case B_Initializer:
    out << ":init"; break;
  case B_AnnotationFunc:
    out << ":annot_func:" << m_loop->Value(); break;
  case B_AnnotationInit:
    out << ":annot_init:" << m_loop->Value(); break;
  case B_AnnotationComp:
    out << ":annot_comp:" << m_loop->Value(); break;
  case B_Scratch:
    out << ":scratch"; break;
  default:
    Assert(false);
    break;
  }
}

void BlockId::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_var->DecMoveRef(ov, nv);
  if (m_loop)
    m_loop->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// BlockPPoint static
/////////////////////////////////////////////////////////////////////

void BlockPPoint::Write(Buffer *buf, BlockPPoint bp)
{
  WriteOpenTag(buf, TAG_BlockPPoint);
  BlockId::Write(buf, bp.id);
  WriteTagUInt32(buf, TAG_Index, bp.point);
  WriteCloseTag(buf, TAG_BlockPPoint);
}

BlockPPoint BlockPPoint::Read(Buffer *buf)
{
  BlockId *id = NULL;
  uint32_t point = 0;

  Try(ReadOpenTag(buf, TAG_BlockPPoint));
  while (!ReadCloseTag(buf, TAG_BlockPPoint)) {
    switch (PeekOpenTag(buf)) {
    case TAG_BlockId:
      Try(!id);
      id = BlockId::Read(buf);
      break;
    case TAG_Index:
      Try(!point);
      Try(ReadTagUInt32(buf, TAG_Index, &point));
      break;
    default:
      BadTag(PeekOpenTag(buf));
    }
  }

  Try(id);
  return BlockPPoint(id, point);
}

/////////////////////////////////////////////////////////////////////
// BlockCFG static
/////////////////////////////////////////////////////////////////////

HashCons<BlockCFG> BlockCFG::g_table;

int BlockCFG::Compare(const BlockCFG *cfg0, const BlockCFG *cfg1)
{
  BlockId *id0 = cfg0->GetId();
  BlockId *id1 = cfg1->GetId();

  if (id0->Kind() != B_Scratch || id1->Kind() != B_Scratch) {
    TryCompareObjects(id0, id1, BlockId);
    return 0;
  }

  // don't do any hash-consing for scratch CFGs, just compare based
  // on the bits.
  TryCompareValues((size_t)cfg0, (size_t)cfg1);

  // we shouldn't be getting two identical CFG pointers here
  Assert(false);
  return 0;
}

BlockCFG* BlockCFG::Copy(const BlockCFG *cfg)
{
  return new BlockCFG(*cfg);
}

void BlockCFG::Write(Buffer *buf, const BlockCFG *cfg)
{
  Assert(cfg->m_begin_location);
  Assert(cfg->m_end_location);

  WriteOpenTag(buf, TAG_BlockCFG);
  BlockId::Write(buf, cfg->m_id);
  Location::Write(buf, cfg->m_begin_location);
  Location::Write(buf, cfg->m_end_location);

  if (cfg->m_annotation_kind)
    WriteTagUInt32(buf, TAG_Kind, cfg->m_annotation_kind);

  const Vector<DefineVariable> *vars = cfg->GetVariables();
  if (vars) {
    for (size_t ind = 0; ind < vars->Size(); ind++) {
      WriteOpenTag(buf, TAG_DefineVariable);
      Variable::Write(buf, vars->At(ind).var);
      Type::Write(buf, vars->At(ind).type);
      WriteCloseTag(buf, TAG_DefineVariable);
    }
  }

  for (size_t ind = 0; ind < cfg->GetLoopParentCount(); ind++)
    BlockPPoint::Write(buf, cfg->GetLoopParent(ind));

  for (PPoint point = 1; point <= cfg->GetPointCount(); point++) {
    WriteOpenTag(buf, TAG_PPoint);
    Location::Write(buf, cfg->GetPointLocation(point));
    WriteCloseTag(buf, TAG_PPoint);
  }

  WriteTagUInt32(buf, TAG_Index, cfg->m_entry_point);
  WriteTagUInt32(buf, TAG_Index, cfg->m_exit_point);

  for (size_t ind = 0; ind < cfg->GetEdgeCount(); ind++)
    PEdge::Write(buf, cfg->GetEdge(ind));

  for (size_t ind = 0; ind < cfg->GetLoopHeadCount(); ind++) {
    LoopHead head = cfg->GetLoopHead(ind);
    WriteOpenTag(buf, TAG_LoopHead);
    WriteTagUInt32(buf, TAG_Index, head.point);
    if (head.end_location)
      Location::Write(buf, head.end_location);
    WriteCloseTag(buf, TAG_LoopHead);
  }

  if (cfg->m_loop_isomorphic) {
    for (size_t ind = 0; ind < cfg->m_loop_isomorphic->Size(); ind++) {
      PPoint point = cfg->m_loop_isomorphic->At(ind);
      WriteOpenTag(buf, TAG_LoopIsomorphic);
      WriteTagUInt32(buf, TAG_Index, point);
      WriteCloseTag(buf, TAG_LoopIsomorphic);
    }
  }

  WriteCloseTag(buf, TAG_BlockCFG);
}

BlockCFG* BlockCFG::Read(Buffer *buf)
{
  BlockCFG *res = NULL;
  bool drop_info = false;

  Try(ReadOpenTag(buf, TAG_BlockCFG));
  while (!ReadCloseTag(buf, TAG_BlockCFG)) {
    switch (PeekOpenTag(buf)) {
    case TAG_BlockId: {
      Assert(!res);

      BlockId *id = BlockId::Read(buf);
      res = Make(id);

      // throw away all the data we read if the CFG is already filled in.
      if (res->m_points)
        drop_info = true;
      break;
    }
    case TAG_Location: {
      Try(res);
      Location *loc = Location::Read(buf);

      if (drop_info)
        loc->DecRef();
      else if (!res->m_begin_location)
        res->SetBeginLocation(loc);
      else
        res->SetEndLocation(loc);
      break;
    }
    case TAG_Kind: {
      Try(res);
      uint32_t kind;
      Try(ReadTagUInt32(buf, TAG_Kind, &kind));

      res->SetAnnotationKind((AnnotationKind) kind);
      break;
    }
    case TAG_DefineVariable: {
      Try(ReadOpenTag(buf, TAG_DefineVariable));
      Variable *var = Variable::Read(buf);
      Type *type = Type::Read(buf);

      if (drop_info) {
        var->DecRef();
        type->DecRef();
      }
      else {
        res->AddVariable(var, type);
      }

      Try(ReadCloseTag(buf, TAG_DefineVariable));
      break;
    }
    case TAG_BlockPPoint: {
      BlockPPoint parent = BlockPPoint::Read(buf, TAG_BlockPPoint);

      if (drop_info)
        parent.id->DecRef();
      else
        res->AddLoopParent(parent);
      break;
    }
    case TAG_PPoint: {
      Try(ReadOpenTag(buf, TAG_PPoint));
      Location *loc = Location::Read(buf);

      if (drop_info)
        loc->DecRef();
      else
        res->AddPoint(loc);

      Try(ReadCloseTag(buf, TAG_PPoint));
      break;
    }
    case TAG_Index: {
      uint32_t point_index;
      Try(ReadTagUInt32(buf, TAG_Index, &point_index));

      if (!drop_info) {
        if (!res->m_entry_point)
          res->SetEntryPoint((PPoint) point_index);
        else
          res->SetExitPoint((PPoint) point_index);
      }

      break;
    }
    case TAG_LoopHead: {
      uint32_t point;
      Try(ReadOpenTag(buf, TAG_LoopHead));
      Try(ReadTagUInt32(buf, TAG_Index, &point));

      Location *end_loc = NULL;
      if (PeekOpenTag(buf) == TAG_Location)
        end_loc = Location::Read(buf);

      Try(ReadCloseTag(buf, TAG_LoopHead));

      if (drop_info) {
        if (end_loc)
          end_loc->DecRef();
      }
      else {
        res->AddLoopHead(point, end_loc);
      }
      break;
    }
    case TAG_LoopIsomorphic: {
      uint32_t point;
      Try(ReadOpenTag(buf, TAG_LoopIsomorphic));
      Try(ReadTagUInt32(buf, TAG_Index, &point));
      Try(ReadCloseTag(buf, TAG_LoopIsomorphic));

      if (!drop_info)
        res->AddLoopIsomorphic(point);
      break;
    }
    case TAG_PEdge: {
      PEdge *edge = PEdge::Read(buf);

      if (drop_info)
        edge->DecRef();
      else
        res->AddEdge(edge);

      break;
    }
    default:
      Try(false);
    }
  }

  Try(res);
  return res;
}

void BlockCFG::WriteList(Buffer *buf, const Vector<BlockCFG*> &cfgs)
{
  Assert(buf->pos == buf->base);

  for (size_t ind = 0; ind < cfgs.Size(); ind++)
    Write(buf, cfgs[ind]);
}

void BlockCFG::ReadList(Buffer *buf, Vector<BlockCFG*> *cfgs)
{
  Assert(buf->pos == buf->base);

  while (buf->pos != buf->base + buf->size) {
    BlockCFG *cfg;
    Try(cfg = Read(buf));
    cfgs->PushBack(cfg);
  }
}

/////////////////////////////////////////////////////////////////////
// BlockCFG
/////////////////////////////////////////////////////////////////////

BlockCFG::BlockCFG(BlockId *id)
  : m_id(id), m_begin_location(NULL), m_end_location(NULL),
    m_vars(NULL), m_loop_parents(NULL),
    m_loop_heads(NULL), m_loop_isomorphic(NULL),
    m_points(NULL), m_entry_point(0), m_exit_point(0), m_edges(NULL),
    m_annotation_kind(AK_Invalid),
    m_annotation_computed(false), m_annotation_bit(NULL),
    m_outgoing_edges(NULL), m_incoming_edges(NULL)
{
  Assert(m_id);
  m_hash = m_id->Hash();
}

Variable* BlockCFG::FindMatchingVariable(Variable *var) const
{
  if (var->IsGlobal())
    return NULL;

  if (m_vars) {
    for (size_t ind = 0; ind < m_vars->Size(); ind++) {
      Variable *local_var = m_vars->At(ind).var;
      if (local_var->Matches(var))
        return local_var;
    }
  }

  return NULL;
}

void BlockCFG::AddVariable(Variable *var, Type *type)
{
  if (m_vars == NULL)
    m_vars = new Vector<DefineVariable>();

  // remember the type on the variable itself.
  var->SetType(type);

  // check for a duplicate entry on this variable.
  for (size_t ind = 0; ind < m_vars->Size(); ind++) {
    if (var == m_vars->At(ind).var) {
      var->DecRef();

      type->MoveRef(NULL, this);
      m_vars->At(ind).type->DecRef(this);
      m_vars->At(ind).type = type;
      return;
    }
  }

  var->MoveRef(NULL, this);
  type->MoveRef(NULL, this);
  m_vars->PushBack(DefineVariable(var, type));
}

void BlockCFG::AddLoopParent(BlockPPoint where)
{
  Assert(m_id->Kind() == B_Loop);
  if (m_loop_parents == NULL)
    m_loop_parents = new Vector<BlockPPoint>();

  where.id->MoveRef(NULL, this);
  m_loop_parents->PushBack(where);
}

void BlockCFG::ClearBody()
{
  ClearEdgeInfo();

  if (m_points) {
    for (size_t ind = 0; ind < m_points->Size(); ind++)
      m_points->At(ind)->DecRef(this);
    delete m_points;
    m_points = NULL;
  }

  ClearLoopHeads();

  if (m_loop_isomorphic) {
    delete m_loop_isomorphic;
    m_loop_isomorphic = NULL;
  }

  if (m_edges) {
    for (size_t ind = 0; ind < m_edges->Size(); ind++)
      m_edges->At(ind)->DecRef(this);
    delete m_edges;
    m_edges = NULL;
  }

  m_entry_point = 0;
  m_exit_point = 0;
}

void BlockCFG::ClearLoopHeads()
{
  if (m_loop_heads) {
    for (size_t ind = 0; ind < m_loop_heads->Size(); ind++) {
      LoopHead head = m_loop_heads->At(ind);
      if (head.end_location)
        head.end_location->DecRef(this);
    }
    delete m_loop_heads;
    m_loop_heads = NULL;
  }
}

void BlockCFG::SetAnnotationKind(AnnotationKind kind)
{
  // this had better be an annotation CFG of some kind.
  switch (m_id->Kind()) {
  case B_AnnotationFunc:
  case B_AnnotationInit:
  case B_AnnotationComp:
    break;
  default:
    Assert(false);
  }

  Assert(!m_annotation_kind || m_annotation_kind == kind);
  m_annotation_kind = kind;
}

void BlockCFG::SetAnnotationBit(Bit *bit)
{
  Assert(!m_annotation_computed);
  m_annotation_computed = true;

  // workaround: frontend currently marks error annotations with invalid kind.
  if (!m_annotation_kind) {
    Assert(!bit);
    return;
  }

  if (bit) {
    bit->MoveRef(NULL, this);
    m_annotation_bit = bit;
  }
}

void BlockCFG::SetBeginLocation(Location *loc)
{
  if (m_begin_location != NULL)
    m_begin_location->DecRef(this);
  loc->MoveRef(NULL, this);
  m_begin_location = loc;
}

void BlockCFG::SetEndLocation(Location *loc)
{
  if (m_end_location != NULL)
    m_end_location->DecRef(this);
  loc->MoveRef(NULL, this);
  m_end_location = loc;
}

PPoint BlockCFG::AddPoint(Location *loc)
{
  Assert(loc);
  loc->MoveRef(NULL, this);

  if (m_points == NULL)
    m_points = new Vector<Location*>();
  m_points->PushBack(loc);

  return m_points->Size();
}

void BlockCFG::SetPointLocation(PPoint point, Location *loc)
{
  Assert(loc);
  loc->MoveRef(NULL, this);

  Assert(m_points);
  Assert(point > 0 && point <= m_points->Size());
  m_points->At(point - 1)->DecRef(this);
  m_points->At(point - 1) = loc;
}

void BlockCFG::SetEntryPoint(PPoint point)
{
  Assert(point > 0 && point <= GetPointCount());
  m_entry_point = point;
}

void BlockCFG::SetExitPoint(PPoint point)
{
  Assert(point <= GetPointCount());
  m_exit_point = point;
}

void BlockCFG::AddLoopHead(PPoint point, Location *end_location)
{
  Assert(point > 0 && point <= GetPointCount());

  if (m_loop_heads == NULL)
    m_loop_heads = new Vector<LoopHead>();

  // check for duplicate loop heads. if we find one then use the
  // later end location if both are specified.

  for (size_t ind = 0; ind < m_loop_heads->Size(); ind++) {
    LoopHead &head = m_loop_heads->At(ind);
    if (point == head.point) {
      if (end_location) {
        if (head.end_location) {
          // e.g. do { do { ... } while (...); ... } while (...);
          // the inner loop probably has no backedges.
          if (head.end_location->Line() < end_location->Line()) {
            head.end_location->DecRef(this);
            head.end_location = end_location;
            end_location->MoveRef(NULL, this);
          }
        }
        else {
          // e.g. Label: while (...) {}
          head.end_location = end_location;
          end_location->MoveRef(NULL, this);
        }
      }
      return;
    }
  }

  LoopHead head(point, end_location);
  if (end_location)
    end_location->MoveRef(NULL, this);

  m_loop_heads->PushBack(head);
}

void BlockCFG::AddLoopIsomorphic(PPoint point)
{
  Assert(point > 0 && point <= GetPointCount());

  if (m_loop_isomorphic == NULL)
    m_loop_isomorphic = new Vector<PPoint>();

  if (m_loop_isomorphic->Contains(point))
    return;
  m_loop_isomorphic->PushBack(point);
}

void BlockCFG::AddEdge(PEdge *edge)
{
  ClearEdgeInfo();

  Assert(edge);
  Assert(edge->GetSource() <= GetPointCount());
  Assert(edge->GetTarget() <= GetPointCount());
  edge->MoveRef(NULL, this);

  if (m_edges == NULL)
    m_edges = new Vector<PEdge*>();
  m_edges->PushBack(edge);
}

void BlockCFG::SetEdge(size_t ind, PEdge *edge)
{
  ClearEdgeInfo();

  Assert(edge);
  Assert(edge->GetSource() <= GetPointCount());
  Assert(edge->GetTarget() <= GetPointCount());
  edge->MoveRef(NULL, this);

  Assert(m_edges && ind < m_edges->Size());
  m_edges->At(ind)->DecRef(this);
  m_edges->At(ind) = edge;
}

const Vector<PEdge*>& BlockCFG::GetOutgoingEdges(PPoint point)
{
  ComputeEdgeInfo();

  Vector<PEdge*> *edges = m_outgoing_edges->Lookup(point, true);
  return *edges;
}

const Vector<PEdge*>& BlockCFG::GetIncomingEdges(PPoint point)
{
  ComputeEdgeInfo();

  Vector<PEdge*> *edges = m_incoming_edges->Lookup(point, true);
  return *edges;
}

PEdge* BlockCFG::GetSingleOutgoingEdge(PPoint point, bool required)
{
  const Vector<PEdge*> &edges = GetOutgoingEdges(point);

  if (edges.Size() == 1)
    return edges[0];

  if (required)
    Assert(false);

  return NULL;
}

bool BlockCFG::PointEdgeIsCall(PPoint point)
{
  PEdge *edge = GetSingleOutgoingEdge(point);

  if (edge->IsCall())
    return true;
  if (edge->IsLoop())
    return false;

  Assert(false);
  return false;
}

void BlockCFG::ComputeEdgeInfo()
{
  if (m_incoming_edges != NULL && m_outgoing_edges != NULL)
    return;
  Assert(m_incoming_edges == NULL && m_outgoing_edges == NULL);

  m_outgoing_edges = new EdgeTable(GetPointCount());
  m_incoming_edges = new EdgeTable(GetPointCount());

  for (size_t ind = 0; ind < GetEdgeCount(); ind++) {
    PEdge *edge = GetEdge(ind);
    m_outgoing_edges->Insert(edge->GetSource(), edge);
    if (edge->GetTarget() != 0)
      m_incoming_edges->Insert(edge->GetTarget(), edge);
  }
}

void BlockCFG::ClearEdgeInfo()
{
  if (m_outgoing_edges != NULL) {
    delete m_outgoing_edges;
    m_outgoing_edges = NULL;
  }

  if (m_incoming_edges != NULL) {
    delete m_incoming_edges;
    m_incoming_edges = NULL;
  }
}

void BlockCFG::Print(OutStream &out) const
{
  out << "block: " << m_id << endl;
  out << "begin: " << m_begin_location << endl;
  out << "end:   " << m_end_location << endl;

  switch (m_annotation_kind) {

#define PRINT_ANNOT(NAME, STR, _)               \
    case AK_ ## NAME: out << "annotation_kind: " << STR << endl; break;
  XIL_ITERATE_ANNOT(PRINT_ANNOT)
#undef PRINT_ANNOT

  default: break;
  }

  for (size_t ind = 0; ind < GetLoopParentCount(); ind++) {
    BlockPPoint where = GetLoopParent(ind);
    out << "parent: " << where.id << ":" << where.point << endl;
  }

  if (m_vars) {
    for (size_t ind = 0; ind < m_vars->Size(); ind++) {
      out << "define: " << m_vars->At(ind).var
          << " : " << m_vars->At(ind).type << endl;
    }
  }

  out << "pentry: " << (long)m_entry_point << endl;
  out << "pexit:  " << (long)m_exit_point << endl;

  for (PPoint point = 1; point <= GetPointCount(); point++) {
    out << "point " << point << ": ";
    GetPointLocation(point)->Print(out);
    if (IsLoopIsomorphic(point))
      out << " [isomorphic]";
    out << endl;
  }

  if (m_loop_heads) {
    for (size_t ind = 0; ind < m_loop_heads->Size(); ind++) {
      LoopHead head = m_loop_heads->At(ind);
      out << "loophead: " << head.point;
      if (head.end_location)
        out << " [" << head.end_location << "]";
      out << endl;
    }
  }

  for (size_t ind = 0; ind < GetEdgeCount(); ind++)
    out << GetEdge(ind) << endl;
}

void BlockCFG::DecMoveChildRefs(ORef ov, ORef nv)
{
  // just drop the reference on the block ID. everything else either hasn't
  // been setup (initial ::Make found an old value), or was cleared by the
  // unpersist (last reference went away).
  m_id->DecMoveRef(ov, nv);

  // try another unpersist anyways (this is idempotent) in case we are
  // being called at exit to find leaking references.
  if (ov == this) {
    Assert(nv == NULL);
    UnPersist();
  }
}

void BlockCFG::Persist()
{
  // only the ID has been initialized at this point.
}

void BlockCFG::UnPersist()
{
  // in addition to deleting allocated data, we also need to drop all the
  // references other than the block ID and reset the data to NULL.

  if (m_begin_location) {
    m_begin_location->DecRef(this);
    m_begin_location = NULL;
  }

  if (m_end_location) {
    m_end_location->DecRef(this);
    m_end_location = NULL;
  }

  if (m_vars) {
    for (size_t ind = 0; ind < m_vars->Size(); ind++) {
      m_vars->At(ind).var->DecRef(this);
      m_vars->At(ind).type->DecRef(this);
    }
    delete m_vars;
    m_vars = NULL;
  }

  if (m_loop_parents) {
    for (size_t ind = 0; ind < m_loop_parents->Size(); ind++)
      m_loop_parents->At(ind).id->DecRef(this);
    delete m_loop_parents;
    m_loop_parents = NULL;
  }

  // takes care of point and edge info.
  ClearBody();

  m_annotation_computed = false;
  if (m_annotation_bit) {
    m_annotation_bit->DecRef(this);
    m_annotation_bit = NULL;
  }
}

/////////////////////////////////////////////////////////////////////
// PEdge static
/////////////////////////////////////////////////////////////////////

HashCons<PEdge> PEdge::g_table;

int PEdge::Compare(const PEdge *e0, const PEdge *e1)
{
  TryCompareValues(e0->GetSource(), e1->GetSource());
  TryCompareValues(e0->GetTarget(), e1->GetTarget());

  return CompareInner(e0, e1);
}

int PEdge::CompareInner(const PEdge *e0, const PEdge *e1)
{
  TryCompareValues(e0->Kind(), e1->Kind());

  switch (e0->Kind()) {
  case EGK_Skip:
    break;
  case EGK_Assume: {
    const PEdgeAssume *ne0 = (const PEdgeAssume*) e0;
    const PEdgeAssume *ne1 = (const PEdgeAssume*) e1;
    TryCompareValues((int)ne0->IsNonZero(), (int)ne1->IsNonZero());
    TryCompareObjects(ne0->GetCondition(), ne1->GetCondition(), Exp);
    break;
  }
  case EGK_Assign: {
    const PEdgeAssign *ne0 = (const PEdgeAssign*) e0;
    const PEdgeAssign *ne1 = (const PEdgeAssign*) e1;
    TryCompareObjects(ne0->GetType(), ne1->GetType(), Type);
    TryCompareObjects(ne0->GetLeftSide(), ne1->GetLeftSide(), Exp);
    TryCompareObjects(ne0->GetRightSide(), ne1->GetRightSide(), Exp);
    break;
  }
  case EGK_Call: {
    const PEdgeCall *ne0 = (const PEdgeCall*) e0;
    const PEdgeCall *ne1 = (const PEdgeCall*) e1;
    TryCompareObjects(ne0->GetType(), ne1->GetType(), Type);
    TryCompareObjects(ne0->GetReturnValue(), ne1->GetReturnValue(), Exp);
    TryCompareObjects(ne0->GetInstanceObject(), ne1->GetInstanceObject(), Exp);
    TryCompareObjects(ne0->GetFunction(), ne1->GetFunction(), Exp);
    TryCompareValues(ne0->GetArgumentCount(), ne1->GetArgumentCount());
    for (size_t ind = 0; ind < ne0->GetArgumentCount(); ind++) {
      Exp *arg0 = ne0->GetArgument(ind);
      Exp *arg1 = ne1->GetArgument(ind);
      TryCompareObjects(arg0, arg1, Exp);
    }
    break;
  }
  case EGK_Loop: {
    const PEdgeLoop *ne0 = (const PEdgeLoop*) e0;
    const PEdgeLoop *ne1 = (const PEdgeLoop*) e1;
    TryCompareObjects(ne0->GetLoopId(), ne1->GetLoopId(), BlockId);
    break;
  }
  case EGK_Assembly:
    break;
  case EGK_Annotation: {
    const PEdgeAnnotation *ne0 = (const PEdgeAnnotation*) e0;
    const PEdgeAnnotation *ne1 = (const PEdgeAnnotation*) e1;
    TryCompareObjects(ne0->GetAnnotationId(), ne1->GetAnnotationId(), BlockId);
    break;
  }
  default:
    Assert(false);
  }

  return 0;
}

PEdge* PEdge::Copy(const PEdge *e)
{
  switch (e->Kind()) {
  case EGK_Skip:       return new PEdgeSkip       (*(PEdgeSkip*)e);
  case EGK_Assume:     return new PEdgeAssume     (*(PEdgeAssume*)e);
  case EGK_Assign:     return new PEdgeAssign     (*(PEdgeAssign*)e);
  case EGK_Call:       return new PEdgeCall       (*(PEdgeCall*)e);
  case EGK_Loop:       return new PEdgeLoop       (*(PEdgeLoop*)e);
  case EGK_Assembly:   return new PEdgeAssembly   (*(PEdgeAssembly*)e);
  case EGK_Annotation: return new PEdgeAnnotation (*(PEdgeAnnotation*)e);
  default:
    Assert(false);
    return NULL;
  }
}

void PEdge::Write(Buffer *buf, const PEdge *e)
{
  WriteOpenTag(buf, TAG_PEdge);
  WriteTagUInt32(buf, TAG_Kind, e->Kind());
  WriteTagUInt32(buf, TAG_Index, e->GetSource());
  WriteTagUInt32(buf, TAG_Index, e->GetTarget());
  
  switch (e->Kind()) {
  case EGK_Skip: {
    break;
  }
  case EGK_Assume: {
    const PEdgeAssume *ne = e->AsAssume();
    Exp::Write(buf, ne->GetCondition());
    if (ne->IsNonZero())
      WriteTagEmpty(buf, TAG_PEdgeAssumeNonZero);
    break;
  }
  case EGK_Assign: {
    const PEdgeAssign *ne = e->AsAssign();
    Type::Write(buf, ne->GetType());
    Exp::Write(buf, ne->GetLeftSide());
    Exp::Write(buf, ne->GetRightSide());
    break;
  }
  case EGK_Call: {
    const PEdgeCall *ne = e->AsCall();
    Type::Write(buf, ne->GetType());
    Exp::Write(buf, ne->GetFunction());
    if (ne->GetReturnValue() != NULL)
      Exp::Write(buf, ne->GetReturnValue());
    if (ne->GetInstanceObject() != NULL) {
      WriteOpenTag(buf, TAG_PEdgeCallInstance);
      Exp::Write(buf, ne->GetInstanceObject());
      WriteCloseTag(buf, TAG_PEdgeCallInstance);
    }
    if (ne->GetArgumentCount() > 0) {
      WriteOpenTag(buf, TAG_PEdgeCallArguments);
      for (size_t ind = 0; ind < ne->GetArgumentCount(); ind++)
        Exp::Write(buf, ne->GetArgument(ind));
      WriteCloseTag(buf, TAG_PEdgeCallArguments);
    }
    break;
  }
  case EGK_Loop: {
    const PEdgeLoop *ne = e->AsLoop();
    BlockId::Write(buf, ne->GetLoopId());
    break;
  }
  case EGK_Assembly: {
    break;
  }
  case EGK_Annotation: {
    const PEdgeAnnotation *ne = e->AsAnnotation();
    BlockId::Write(buf, ne->GetAnnotationId());
    break;
  }
  default:
    Assert(false);
    break;
  }
  WriteCloseTag(buf, TAG_PEdge);
}

PEdge* PEdge::Read(Buffer *buf)
{
  uint32_t kind = 0;
  uint32_t xsource = 0;
  uint32_t xtarget = 0;
  PPoint source = 0;
  PPoint target = 0;
  bool assume_nonzero = false;
  Type *type = NULL;
  Exp *exp0 = NULL;
  Exp *exp1 = NULL;
  BlockId *block = NULL;
  Vector<Exp*> call_arguments;
  Exp *call_instance = NULL;

  Try(ReadOpenTag(buf, TAG_PEdge));
  while (!ReadCloseTag(buf, TAG_PEdge)) {
    switch (PeekOpenTag(buf)) {
    case TAG_Kind: {
      Try(!kind);
      Try(ReadTagUInt32(buf, TAG_Kind, &kind));
      break;
    }
    case TAG_Index: {
      if (source != 0) {
        Try(!target);
        Try(ReadTagUInt32(buf, TAG_Index, &xtarget));
        target = (PPoint)xtarget;
      }
      else {
        Try(ReadTagUInt32(buf, TAG_Index, &xsource));
        source = (PPoint)xsource;
      }
      break;
    }
    case TAG_Exp: {
      if (exp0) {
        Try(!exp1);
        exp1 = Exp::Read(buf);
      }
      else {
        exp0 = Exp::Read(buf);
      }
      break;
    }
    case TAG_PEdgeAssumeNonZero: {
      Try(ReadTagEmpty(buf, TAG_PEdgeAssumeNonZero));
      assume_nonzero = true;
      break;
    }
    case TAG_PEdgeCallArguments: {
      Try(call_arguments.Empty());
      Try(ReadOpenTag(buf, TAG_PEdgeCallArguments));
      while (!ReadCloseTag(buf, TAG_PEdgeCallArguments)) {
        Exp *arg = Exp::Read(buf);
        call_arguments.PushBack(arg);
      }
      break;
    }
    case TAG_PEdgeCallInstance: {
      Try(!call_instance);
      Try(ReadOpenTag(buf, TAG_PEdgeCallInstance));
      call_instance = Exp::Read(buf);
      Try(ReadCloseTag(buf, TAG_PEdgeCallInstance));
      break;
    }
    case TAG_Type: {
      Try(!type);
      type = Type::Read(buf);
      break;
    }
    case TAG_BlockId: {
      Try(!block);
      block = BlockId::Read(buf);
      break;
    }
    default:
      Try(false);
    }
  }

  Try(source);
  switch (kind) {
  case EGK_Skip:
    return MakeSkip(source, target);
  case EGK_Assume:
    Try(exp0);
    return MakeAssume(source, target, exp0, assume_nonzero);
  case EGK_Assign:
    Try(type && exp0 && exp1);
    return MakeAssign(source, target, type, exp0, exp1);
  case EGK_Call: {
    Try(type);
    TypeFunction *fn_type = type->AsFunction();
    return MakeCall(source, target, fn_type,
                    exp1, call_instance, exp0, call_arguments);
  }
  case EGK_Loop:
    Try(block);
    return MakeLoop(source, target, block);
  case EGK_Assembly:
    return MakeAssembly(source, target);
  case EGK_Annotation:
    Try(block);
    return MakeAnnotation(source, target, block);
  default:
    Try(false);
  }
}

PEdge* PEdge::MakeSkip(PPoint source, PPoint target)
{
  PEdgeSkip xe(source, target);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeAssume(PPoint source, PPoint target,
                         Exp *cond, bool nonzero)
{
  PEdgeAssume xe(source, target, cond, nonzero);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeAssign(PPoint source, PPoint target, Type *type,
                         Exp *left_side, Exp *right_side)
{
  PEdgeAssign xe(source, target, type, left_side, right_side);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeCall(PPoint source, PPoint target, TypeFunction *type,
                       Exp *return_value, Exp *instance, Exp *function,
                       const Vector<Exp*> &arguments)
{
  PEdgeCall xe(source, target, type,
               return_value, instance, function, arguments);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeLoop(PPoint source, PPoint target, BlockId *loop)
{
  PEdgeLoop xe(source, target, loop);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeAssembly(PPoint source, PPoint target)
{
  PEdgeAssembly xe(source, target);
  return g_table.Lookup(xe);
}

PEdge* PEdge::MakeAnnotation(PPoint source, PPoint target, BlockId *annot)
{
  PEdgeAnnotation xe(source, target, annot);
  return g_table.Lookup(xe);
}

PEdge* PEdge::ChangeEdge(const PEdge *e, PPoint source, PPoint target)
{
  switch (e->Kind()) {
  case EGK_Skip: {
    return MakeSkip(source, target);
  }
  case EGK_Assume: {
    const PEdgeAssume *ne = e->AsAssume();

    Exp *scalar = ne->GetCondition();
    scalar->IncRef();

    bool nonzero = ne->IsNonZero();

    return MakeAssume(source, target, scalar, nonzero);
  }
  case EGK_Assign: {
    const PEdgeAssign *ne = e->AsAssign();

    Type *type = ne->GetType();
    type->IncRef();

    Exp *left = ne->GetLeftSide();
    left->IncRef();

    Exp *right = ne->GetRightSide();
    right->IncRef();

    return MakeAssign(source, target, type, left, right);
  }
  case EGK_Call: {
    const PEdgeCall *ne = e->AsCall();
    
    TypeFunction *type = ne->GetType();
    type->IncRef();

    Exp *retval = ne->GetReturnValue();
    if (retval)
      retval->IncRef();

    Exp *instance_object = ne->GetInstanceObject();
    if (instance_object)
      instance_object->IncRef();

    Exp *function = ne->GetFunction();
    if (function)
      function->IncRef();

    Vector<Exp*> arguments;
    for (size_t ind = 0; ind < ne->GetArgumentCount(); ind++) {
      Exp *arg = ne->GetArgument(ind);
      arg->IncRef();
      arguments.PushBack(arg);
    }

    return MakeCall(source, target, type,
                    retval, instance_object, function, arguments);
  }
  case EGK_Loop: {
    const PEdgeLoop *ne = e->AsLoop();

    BlockId *block = ne->GetLoopId();
    block->IncRef();

    return MakeLoop(source, target, block);
  }
  case EGK_Assembly: {
    return MakeAssembly(source, target);
  }
  case EGK_Annotation: {
    const PEdgeAnnotation *ne = e->AsAnnotation();

    BlockId *annot = ne->GetAnnotationId();
    annot->IncRef();

    return MakeAnnotation(source, target, annot);
  }
  default:
    Assert(false);
    return NULL;
  }
}

/////////////////////////////////////////////////////////////////////
// PEdgeSkip
/////////////////////////////////////////////////////////////////////

PEdgeSkip::PEdgeSkip(PPoint source, PPoint target)
  : PEdge(EGK_Skip, source, target)
{}

void PEdgeSkip::Print(OutStream &out) const
{
  out << "Skip(" << (long)m_source << "," << (long)m_target << ")";
}

void PEdgeSkip::PrintUI(OutStream &out) const
{
  out << "skip";
}

/////////////////////////////////////////////////////////////////////
// PEdgeAssume
/////////////////////////////////////////////////////////////////////

PEdgeAssume::PEdgeAssume(PPoint source, PPoint target, Exp *cond, bool nonzero)
  : PEdge(EGK_Assume, source, target), m_cond(cond), m_nonzero(nonzero)
{
  Assert(m_cond);
  m_hash = Hash32(m_hash, m_cond->Hash() * 2 + (m_nonzero ? 1 : 0));
}

void PEdgeAssume::DoVisit(ExpVisitor *visitor) const
{
  m_cond->DoVisit(visitor);
}

void PEdgeAssume::Print(OutStream &out) const
{
  out << "Assume("
      << (long)m_source << "," << (long)m_target << ", "
      << m_cond << ", " << (m_nonzero ? "true" : "false")
      << ")";
}

void PEdgeAssume::PrintUI(OutStream &out) const
{
  out << "assume(";

  Bit *bit = Exp::MakeNonZeroBit(m_cond);

  if (!m_nonzero)
    bit = Bit::MakeNot(bit);

  bit->PrintUI(out, false);
  bit->DecRef();

  out << ")";
}

void PEdgeAssume::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_cond->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// PEdgeAssign
/////////////////////////////////////////////////////////////////////

PEdgeAssign::PEdgeAssign(PPoint source, PPoint target, Type *type,
                         Exp *left_side, Exp *right_side)
  : PEdge(EGK_Assign, source, target),
    m_type(type), m_left_side(left_side), m_right_side(right_side)
{
  Assert(m_type);
  Assert(m_left_side);
  Assert(m_right_side);
  m_hash = Hash32(m_hash, m_type->Hash());
  m_hash = Hash32(m_hash, m_left_side->Hash());
  m_hash = Hash32(m_hash, m_right_side->Hash());
}

void VisitAssign(ExpVisitor *visitor,
                 Exp *left, Exp *right, Type *type)
{
  if (type != NULL && type->Kind() == YK_CSU) {
    String *csu_name = type->AsCSU()->GetCSUName();
    CompositeCSU *csu = CompositeCSUCache.Lookup(csu_name);

    if (csu != NULL) {
      for (size_t find = 0; find < csu->GetFieldCount(); find++) {
        const DataField &df = csu->GetField(find);

        Exp *new_left = NULL;
        if (left != NULL) {
          left->IncRef();
          df.field->IncRef();
          new_left = Exp::MakeFld(left, df.field);
        }

        Exp *new_right = NULL;
        if (right && right->IsDrf()) {
          Exp *target = right->AsDrf()->GetTarget();

          target->IncRef();
          df.field->IncRef();
          Exp *new_target = Exp::MakeFld(target, df.field);
          new_right = Exp::MakeDrf(new_target);
        }

        VisitAssign(visitor, new_left, new_right, df.field->GetType());

        if (new_left)
          new_left->DecRef();
        if (new_right)
          new_right->DecRef();
      }
    }

    CompositeCSUCache.Release(csu_name);
  }
  else {
    if (left) {
      // the left side of the assignment is an lvalue which is written.

      if (visitor->Kind() == VISK_Lval)
        visitor->Visit(left);

      bool old_found_lval = visitor->SetFoundLval(true);
      left->DoVisit(visitor);
      visitor->SetFoundLval(old_found_lval);
    }

    if (right)
      right->DoVisit(visitor);
  }
}

void PEdgeAssign::DoVisit(ExpVisitor *visitor) const
{
  VisitAssign(visitor, m_left_side, m_right_side, m_type);
}

void PEdgeAssign::Print(OutStream &out) const
{
  out << "Assign("
      << (long)m_source << "," << (long)m_target << ", "
      << m_left_side << " := " << m_right_side << ")";
}

void PEdgeAssign::PrintUI(OutStream &out) const
{
  m_left_side->PrintUI(out, true);
  out << " = ";
  m_right_side->PrintUIRval(out, false);
}

void PEdgeAssign::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_type->DecMoveRef(ov, nv);
  m_left_side->DecMoveRef(ov, nv);
  m_right_side->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// PEdgeCall
/////////////////////////////////////////////////////////////////////

PEdgeCall::PEdgeCall(PPoint source, PPoint target, TypeFunction *type,
                     Exp *return_value, Exp *instance, Exp *function,
                     const Vector<Exp*> &arguments)
  : PEdge(EGK_Call, source, target),
    m_type(type), m_return_value(return_value),
    m_instance_object(instance), m_function(function),
    m_arguments(arguments.Data()), m_argument_count(arguments.Size())
{
  Assert(m_type);
  Assert(m_function);
  AssertArray(m_arguments, m_argument_count);

  m_hash = Hash32(m_hash, m_type->Hash());
  if (m_return_value)
    m_hash = Hash32(m_hash, m_return_value->Hash());
  if (m_instance_object)
    m_hash = Hash32(m_hash, m_instance_object->Hash());
  m_hash = Hash32(m_hash, m_function->Hash());
  for (size_t ind = 0; ind < m_argument_count; ind++)
    m_hash = Hash32(m_hash, m_arguments[ind]->Hash());
}

Variable* PEdgeCall::GetDirectFunction() const
{
  if (m_function->IsVar())
    return m_function->AsVar()->GetVariable();

  return NULL;
}

BlockId* PEdgeCall::GetDirectCallee() const
{
  Variable *function = GetDirectFunction();
  if (function) {
    function->IncRef();
    return BlockId::Make(B_Function, function);
  }
  else {
    return NULL;
  }
}

void PEdgeCall::DoVisit(ExpVisitor *visitor) const
{
  if (m_return_value) {
    Type *ret_type = m_type->GetReturnType();
    VisitAssign(visitor, m_return_value, NULL, ret_type);
  }

  if (m_instance_object) {
    // the instance object is an lvalue.
    if (visitor->Kind() == VISK_Lval)
      visitor->Visit(m_instance_object);

    bool old_found_lval = visitor->SetFoundLval(true);
    m_instance_object->DoVisit(visitor);
    visitor->SetFoundLval(old_found_lval);
  }
  else {
    // the function is only relevant for non-instance calls.
    m_function->DoVisit(visitor);
  }

  for (size_t ind = 0; ind < m_argument_count; ind++) {
    Type *arg_type = m_type->GetArgumentType(ind);
    VisitAssign(visitor, NULL, m_arguments[ind], arg_type);
  }
}

void PEdgeCall::Print(OutStream &out) const
{
  out << "Call("
      << (long)m_source << "," << (long)m_target << ", ";
  if (m_return_value)
    out << m_return_value << " := ";

  if (m_instance_object) {
    if (m_function->IsVar()) {
      out << m_instance_object << "." << m_function;
    }
    else {
      Exp *empty = Exp::MakeEmpty();
      Exp *new_function = ExpReplaceExp(m_function, empty, m_instance_object);
      out << new_function;
      empty->DecRef();
      new_function->DecRef();
    }
  }
  else {
    out << m_function;
  }

  out << "(";
  for (size_t ind = 0; ind < m_argument_count; ind++) {
    if (ind)
      out << ",";
    out << m_arguments[ind];
  }
  out << "))";
}

void PEdgeCall::PrintUI(OutStream &out) const
{
  if (m_return_value) {
    m_return_value->PrintUI(out, true);
    out << " = ";
  }

  if (m_instance_object) {
    if (m_function->IsVar()) {
      if (ExpDrf *nobject = m_instance_object->IfDrf()) {
        nobject->GetTarget()->PrintUI(out, true);
        out << "->";
      }
      else {
        m_instance_object->PrintUI(out, true);
        out << ".";
      }
      m_function->PrintUI(out, true);
    }
    else {
      Exp *empty = Exp::MakeEmpty();
      Exp *new_function = ExpReplaceExp(m_function, empty, m_instance_object);
      new_function->PrintUI(out, true);
      empty->DecRef();
      new_function->DecRef();
    }
  }
  else {
    m_function->PrintUI(out, true);
  }

  out << "(";
  for (size_t ind = 0; ind < m_argument_count; ind++) {
    if (ind)
      out << ", ";
    m_arguments[ind]->PrintUIRval(out, false);
  }
  out << ")";
}

void PEdgeCall::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_type->DecMoveRef(ov, nv);
  if (m_return_value)
    m_return_value->DecMoveRef(ov, nv);
  if (m_instance_object)
    m_instance_object->DecMoveRef(ov, nv);
  m_function->DecMoveRef(ov, nv);

  for (size_t ind = 0; ind < m_argument_count; ind++)
    m_arguments[ind]->DecMoveRef(ov, nv);
}

void PEdgeCall::Persist()
{
  if (m_argument_count > 0) {
    Exp **new_arguments = new Exp*[m_argument_count];
    memcpy(new_arguments, m_arguments, m_argument_count * sizeof(Exp*));
    m_arguments = new_arguments;
  }
  else {
    m_arguments = NULL;
  }
}

void PEdgeCall::UnPersist()
{
  if (m_argument_count > 0) {
    delete[] m_arguments;
    m_arguments = NULL;
  }
}

/////////////////////////////////////////////////////////////////////
// PEdgeLoop
/////////////////////////////////////////////////////////////////////

PEdgeLoop::PEdgeLoop(PPoint source, PPoint target, BlockId *loop)
  : PEdge(EGK_Loop, source, target), m_loop(loop)
{
  Assert(m_loop);
  m_hash = Hash32(m_hash, m_loop->Hash());
}

BlockId* PEdgeLoop::GetDirectCallee() const
{
  m_loop->IncRef();
  return m_loop;
}

void PEdgeLoop::Print(OutStream &out) const
{
  out << "Loop(" << (long)m_source << "," << (long)m_target
      << ", " << m_loop->Loop()->Value() << ")";
}

void PEdgeLoop::PrintUI(OutStream &out) const
{
  out << "invoke(" << m_loop->LoopName() << ")";
}

void PEdgeLoop::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_loop->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// PEdgeAssembly
/////////////////////////////////////////////////////////////////////

PEdgeAssembly::PEdgeAssembly(PPoint source, PPoint target)
  : PEdge(EGK_Assembly, source, target)
{}

void PEdgeAssembly::Print(OutStream &out) const
{
  out << "Assembly(" << (long)m_source << "," << (long)m_target << ")";
}

void PEdgeAssembly::PrintUI(OutStream &out) const
{
  out << "assembly";
}

/////////////////////////////////////////////////////////////////////
// PEdgeAnnotation
/////////////////////////////////////////////////////////////////////

PEdgeAnnotation::PEdgeAnnotation(PPoint source, PPoint target, BlockId *annot)
  : PEdge(EGK_Annotation, source, target), m_annot(annot)
{
  Assert(m_annot);
  m_hash = Hash32(m_hash, m_annot->Hash());
}

void PEdgeAnnotation::Print(OutStream &out) const
{
  out << "Annotation(" << (long)m_source << "," << (long)m_target
      << "," << m_annot->Loop()->Value() << ")";
}

void PEdgeAnnotation::PrintUI(OutStream &out) const
{
  out << "annotation";
}

NAMESPACE_XGILL_END
