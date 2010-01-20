
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

#include "backend_graph.h"
#include "backend_hash.h"
#include "backend_util.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// backend data
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

struct SortInfo {
  String *name;              // holds a reference.

  // ordering of nodes in this sort.
  Vector<DataString*> *entries;

  SortInfo()
    : name(NULL), entries(NULL)
  {}
};

// list of all in-memory graph sorts
Vector<SortInfo> sorts;

void ClearSorts()
{
  for (size_t sind = 0; sind < sorts.Size(); sind++) {
    SortInfo &info = sorts[sind];

    info.name->DecRef(&sorts);

    if (info.entries) {
      for (size_t ind = 0; ind < info.entries->Size(); ind++)
        info.entries->At(ind)->DecRef(info.entries);
      delete info.entries;
    }
  }

  sorts.Clear();
}

SortInfo& GetSort(const uint8_t *name, bool do_create = true)
{
  String *name_str = String::Make((const char*)name);
  for (size_t sind = 0; sind < sorts.Size(); sind++) {
    SortInfo &info = sorts[sind];

    if (info.name == name_str) {
      name_str->DecRef();

      if (do_create && info.entries == NULL)
        info.entries = new Vector<DataString*>();

      return info;
    }
  }

  name_str->MoveRef(NULL, &sorts);

  Vector<DataString*> *entries = NULL;
  if (do_create)
    entries = new Vector<DataString*>();

  SortInfo info;
  info.name = name_str;
  info.entries = entries;
  sorts.PushBack(info);

  return sorts.Back();
}

// visitor to add the reverse of all edges in a graph to new_graph.
class GraphReverseVisitor : public HashTableVisitor<DataString*,DataString*>
{
 public:
  DataStringHash &new_graph;
  GraphReverseVisitor(DataStringHash &_new_graph) : new_graph(_new_graph) {}

  void Visit(DataString *&str, Vector<DataString*> &str_list) {
    for (size_t ind = 0; ind < str_list.Size(); ind++)
      new_graph.Insert(str_list[ind], str);
  }
};

// implementation of Kosaraju's algorithm for finding the SCCs in a graph.
// there are other algorithms that would be faster, but without better
// asymptotic efficiency and considerable additional complexity.

// data about each vertex we need for computing SCCs. the possible
// vertices are those mentioned either as a key or a value in
// the source hash.
struct SccData {
  // node this is the SCC data for.
  DataString *node;

  // linked entry for vertices not in the main vertex stack.
  SccData *remain_next, **remain_pprev;

  // linked entry for vertices in the main vertex stack. entries at the
  // beginning of this list are at the top of the stack.
  SccData *stack_next, **stack_pprev;

  SccData()
    : node(NULL), remain_next(NULL), remain_pprev(NULL),
      stack_next(NULL), stack_pprev(NULL)
  {}
};

struct __SccData_RemainList
{
  static SccData**  GetNext(SccData *o) { return &o->remain_next; }
  static SccData*** GetPPrev(SccData *o) { return &o->remain_pprev; }
};

struct __SccData_StackList
{
  static SccData**  GetNext(SccData *o) { return &o->stack_next; }
  static SccData*** GetPPrev(SccData *o) { return &o->stack_pprev; }
};

typedef HashTable<DataString*,SccData,HashObject> SccDataTable;

// all data we need to compute the SCCs for a graph.
struct SccComputeData
{
  // SCC information and stack/list positioning for each graph vertex.
  SccDataTable table;

  // linked list of all entries that have not been added to the stack
  // in the initial graph pass.
  SccData *remain_begin, **remain_pend;

  // stack of entries to process in the second graph pass.
  SccData *stack_begin, **stack_pend;

  // sort information which will receive the final topo sort.
  SortInfo &sort_info;

  SccComputeData(SortInfo &_sort_info)
    : sort_info(_sort_info)
  {
    LinkedListInit<SccData>(&remain_begin, &remain_pend);
    LinkedListInit<SccData>(&stack_begin, &stack_pend);
  }
};

// visitor to add all nodes mentioned as the source or target of an edge
// to an SCC data table.
class AddSccDataVisitor : public HashTableVisitor<DataString*,DataString*>
{
 public:
  SccComputeData &scc_data;

  AddSccDataVisitor(SccComputeData &_scc_data)
    : scc_data(_scc_data)
  {}

  void InsertCheck(DataString *node)
  {
    Vector<SccData> *entries = scc_data.table.Lookup(node, true);
    if (entries->Empty()) {
      entries->PushBack(SccData());
      SccData &data = entries->Back();
      data.node = node;
      LinkedListInsert<SccData,__SccData_RemainList>
        (&scc_data.remain_pend, &data);
    }
  }

  void Visit(DataString *&str, Vector<DataString*> &str_list) {
    InsertCheck(str);
    for (size_t ind = 0; ind < str_list.Size(); ind++)
      InsertCheck(str_list[ind]);
  }
};

// push node and all vertices reachable from it onto the SCC data stack,
// ensuring that a node is pushed *after* its transitive children
// (modulo cycles) are.
void SccTraversePushStack(SccComputeData &scc_data,
                          DataStringHash &graph,
                          SccData *entry)
{
  if (entry->remain_pprev == NULL) {
    // already added this node to the stack by a previous DFS,
    // or visited previously in this same DFS. skip.
    return;
  }

  // remove the entry from the remain list. until the DFS on this
  // node completes, the entry is in limbo and not on either the
  // remaining or stack lists.
  LinkedListRemove<SccData,__SccData_RemainList>
    (&scc_data.remain_pend, entry);

  // recurse on all forward edges from the node.
  Vector<DataString*> *targets = graph.Lookup(entry->node, false);
  if (targets) {
    for (size_t ind = 0; ind < targets->Size(); ind++) {
      SccData &next_entry = scc_data.table.LookupSingle(targets->At(ind));
      SccTraversePushStack(scc_data, graph, &next_entry);
    }
  }

  // push the entry onto the top of the stack, now that the recursive
  // calls has completed and everything reachable from this node
  // will be further down the stack.
  LinkedListInsertHead<SccData,__SccData_StackList>
    (&scc_data.stack_begin, entry);
}

void SccTraversePopStack(SccComputeData &scc_data,
                         DataStringHash &reverse_graph,
                         SccData *entry)
{
  if (entry->stack_pprev == NULL) {
    // we already visited this node while popping the stack. treat it
    // as if it were removed from the reversed graph.
    return;
  }

  // everything reachable from entry in the reversed graph which has
  // not already been removed from the stack is in the same SCC.
  // it doesn't matter what order we add the vertices to the topo sort.

  // push this node onto the end of the sort list.
  entry->node->IncRef(scc_data.sort_info.entries);
  scc_data.sort_info.entries->PushBack(entry->node);

  // remove this node from the stack.
  LinkedListRemove<SccData,__SccData_StackList>
    (&scc_data.stack_pend, entry);

  // recurse on all reverse edges from the node.
  Vector<DataString*> *targets = reverse_graph.Lookup(entry->node, false);
  if (targets) {
    for (size_t ind = 0; ind < targets->Size(); ind++) {
      SccData &prev_entry = scc_data.table.LookupSingle(targets->At(ind));
      SccTraversePopStack(scc_data, reverse_graph, &prev_entry);
    }
  }
}

void Backend_GraphTopoSortHash(const uint8_t *hash_name,
                               const uint8_t *sort_name)
{
  SortInfo &info = GetSort(sort_name, false);

  // don't do anything if the sort already exists.
  if (info.entries != NULL)
    return;

  info.entries = new Vector<DataString*>();

  DataStringHash *graph = GetNamedHash(hash_name);

  // if there are no edges in the hash, don't do anything.
  if (graph == NULL)
    return;

  // reverse the edges of the graph to get incoming edges to each vertex.
  DataStringHash reverse_graph;
  GraphReverseVisitor reverse_visitor(reverse_graph);
  graph->VisitEach(&reverse_visitor);

  SccComputeData scc_data(info);

  // fill in initial SCC data and add items to the remaining list.
  AddSccDataVisitor init_visitor(scc_data);
  graph->VisitEach(&init_visitor);

  // consume the remaining list and fill in the SCC data stack.
  while (scc_data.remain_begin != NULL)
    SccTraversePushStack(scc_data, *graph, scc_data.remain_begin);

  // consume the SCC data stack and fill in the topo sorted nodes for info.
  while (scc_data.stack_begin != NULL)
    SccTraversePopStack(scc_data, reverse_graph, scc_data.stack_begin);

  Assert(info.entries->Size() == scc_data.table.GetEntryCount());
}

void Backend_GraphStoreSort(const uint8_t *sort_name, const uint8_t *file_name)
{
  SortInfo &info = GetSort(sort_name);
  ofstream out((const char*) file_name);

  for (size_t ind = 0; ind < info.entries->Size(); ind++) {
    DataString *node = info.entries->At(ind);
    if (ValidString(node->Value(), node->ValueLength()))
      out << (const char*) node->Value() << endl;
    else
      logout << "ERROR: Expected valid string in topo sort values" << endl;
  }
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

bool GraphSortExists(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, sort_name, sort_length);

  SortInfo &info = GetSort(sort_name, false);

  *result = new TOperandBoolean(t, info.entries != NULL);
  return true;
}

bool GraphTopoSortHash(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, sort_name, sort_length);

  Backend_GraphTopoSortHash(hash_name, sort_name);
  return true;
}

bool GraphStoreSort(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, sort_name, sort_length);
  BACKEND_ARG_STRING(1, file_name, file_length);

  Backend_GraphStoreSort(sort_name, file_name);
  return true;
}

bool GraphLoadSort(Transaction *t, const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, sort_name, sort_length);
  BACKEND_ARG_STRING(1, file_name, file_length);

  SortInfo &info = GetSort(sort_name, false);

  // don't do anything if the sort already exists.
  if (info.entries != NULL)
    return true;

  info.entries = new Vector<DataString*>();
  ifstream in((char*) file_name);

  if (in.IsError()) {
    // this is OK, just leave the sort empty.
    return true;
  }

  Buffer file_buf;
  ReadInStream(in, &file_buf);

  Vector<char*> entry_names;
  SplitBufferStrings(&file_buf, '\n', &entry_names);

  for (size_t eind = 0; eind < entry_names.Size(); eind++) {
    char *str = entry_names[eind];
    DataString *node = DataString::Make((uint8_t*) str, strlen(str) + 1);

    node->MoveRef(NULL, info.entries);
    info.entries->PushBack(node);
  }

  return true;
}

bool GraphReverseSort(Transaction *t, const Vector<TOperand*> &arguments,
                      TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, sort_name, sort_length);

  SortInfo &info = GetSort(sort_name);

  size_t maxpoint = info.entries->Size();
  size_t midpoint = maxpoint / 2;

  for (size_t ind = 0; ind < midpoint; ind++) {
    DataString *node = info.entries->At(ind);
    info.entries->At(ind) = info.entries->At(maxpoint - ind);
    info.entries->At(maxpoint - ind) = node;
  }

  return true;
}

bool GraphGetMaxSort(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, sort_name, sort_length);

  SortInfo &info = GetSort(sort_name);

  if (info.entries->Empty()) {
    *result = new TOperandString(t, "");
  }
  else {
    DataString *node = info.entries->Back();
    size_t length = node->ValueLength();

    Buffer *buf = new Buffer(length);
    t->AddBuffer(buf);

    memcpy(buf->base, node->Value(), length);
    *result = new TOperandString(t, buf->base, length);
  }

  return true;
}

bool GraphRemoveMaxSort(Transaction *t, const Vector<TOperand*> &arguments,
                        TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, sort_name, sort_length);

  SortInfo &info = GetSort(sort_name);

  if (!info.entries->Empty()) {
    info.entries->Back()->DecRef(info.entries);
    info.entries->PopBack();
  }

  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend
/////////////////////////////////////////////////////////////////////

static void start_Graph()
{
  BACKEND_REGISTER(GraphSortExists);
  BACKEND_REGISTER(GraphTopoSortHash);
  BACKEND_REGISTER(GraphStoreSort);
  BACKEND_REGISTER(GraphLoadSort);
  BACKEND_REGISTER(GraphReverseSort);
  BACKEND_REGISTER(GraphGetMaxSort);
  BACKEND_REGISTER(GraphRemoveMaxSort);
}

static void finish_Graph()
{
  BACKEND_IMPL::ClearSorts();
}

TransactionBackend backend_Graph(start_Graph, finish_Graph);

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* GraphSortExists(Transaction *t,
                         const char *sort_name,
                         size_t var_result)
{
  BACKEND_CALL(GraphSortExists, var_result);
  call->PushArgument(new TOperandString(t, sort_name));
  return call;
}

TAction* GraphTopoSortHash(Transaction *t,
                           const char *hash_name,
                           const char *sort_name)
{
  BACKEND_CALL(GraphTopoSortHash, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(new TOperandString(t, sort_name));
  return call;
}

TAction* GraphStoreSort(Transaction *t,
                        const char *sort_name,
                        const char *file_name)
{
  BACKEND_CALL(GraphStoreSort, 0);
  call->PushArgument(new TOperandString(t, sort_name));
  call->PushArgument(new TOperandString(t, file_name));
  return call;
}

TAction* GraphLoadSort(Transaction *t,
                       const char *sort_name,
                       const char *file_name)
{
  BACKEND_CALL(GraphLoadSort, 0);
  call->PushArgument(new TOperandString(t, sort_name));
  call->PushArgument(new TOperandString(t, file_name));
  return call;
}

TAction* GraphReverseSort(Transaction *t,
                          const char *sort_name)
{
  BACKEND_CALL(GraphReverseSort, 0);
  call->PushArgument(new TOperandString(t, sort_name));
  return call;
}

TAction* GraphGetMaxSort(Transaction *t,
                         const char *sort_name,
                         size_t var_result)
{
  BACKEND_CALL(GraphGetMaxSort, var_result);
  call->PushArgument(new TOperandString(t, sort_name));
  return call;
}

TAction* GraphRemoveMaxSort(Transaction *t,
                            const char *sort_name)
{
  BACKEND_CALL(GraphRemoveMaxSort, 0);
  call->PushArgument(new TOperandString(t, sort_name));
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
