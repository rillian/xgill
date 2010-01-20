
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

#include "baked.h"
#include "storage.h"

#include <backend/merge_lookup.h>
#include <imlang/storage.h>

NAMESPACE_XGILL_BEGIN

// scratch buffer for doing compression.
static Buffer scratch_buf("Buffer_memory_compress");

// maximum capacities for the merge caches.

#define MERGE_CAP_ESCAPE_EDGE    40000
#define MERGE_CAP_ESCAPE_ACCESS  40000
#define MERGE_CAP_CALLGRAPH      10000

// maximum capacities for the storage caches.

#define CAP_BLOCK_MEMORY   10000
#define CAP_BLOCK_MODSET   10000
#define CAP_BLOCK_SUMMARY  10000
#define CAP_ESCAPE_EDGE    5000
#define CAP_ESCAPE_ACCESS  5000
#define CAP_CALLGRAPH      20000

// maximum number of entries, for each database, we will flush in a
// single transaction; the number of database operations performed by a
// transaction is at most this limit times the number of merge caches.
#define MERGE_TRANSACTION_LIMIT  20

void ClearMemoryCaches()
{
  BlockMemoryCache.Clear();
  BlockModsetCache.Clear();
  BlockSummaryCache.Clear();

  EscapeForwardCache.Clear();
  EscapeBackwardCache.Clear();
  EscapeAccessCache.Clear();

  CalleeCache.Clear();
  CallerCache.Clear();
}

String* GetTraceKey(Trace *trace)
{
  static Buffer key_buf;
  key_buf.Reset();

  switch (trace->Kind()) {
  case TK_Func: {
    const char *str = trace->GetFunction()->GetName()->Value();

    key_buf.Append("func:", 5);
    key_buf.Append(str, strlen(str) + 1);
    break;
  }
  case TK_Glob: {
    Variable *var = trace->GetValue()->Root();
    Assert(var != NULL && var->IsGlobal());
    const char *name = var->GetName()->Value();

    key_buf.Append("glob:", 5);
    key_buf.Append(name, strlen(name) + 1);
    break;
  }
  case TK_Comp: {
    const char *comp_name = trace->GetCSUName()->Value();
    key_buf.Append("comp:", 5);
    key_buf.Append(comp_name, strlen(comp_name));

    Field *field = trace->GetValue()->BaseField();
    if (field && !field->IsInstanceFunction()) {
      // regular field offset from the CSU type.
      const char *field_name = field->GetName()->Value();
      key_buf.Append(":", 1);
      key_buf.Append(field_name, strlen(field_name) + 1);
    }
    else {
      // virtual function or base class information on the CSU.
      key_buf.Ensure(1);
      *key_buf.pos = 0;
    }

    break;
  }
  default:
    Assert(false);
  }

  return String::Make((const char*)key_buf.base);
}

/////////////////////////////////////////////////////////////////////
// BlockMemory lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_BlockMemory : public Cache_BlockMemory::ExternalLookup
{
  void LookupInsert(Cache_BlockMemory *cache, BlockId *id)
  {
    Assert(id->Kind() == B_Function || id->Kind() == B_Loop ||
           id->Kind() == B_Initializer);

    String *function = id->Function();
    const char *function_name = function->Value();

    if (!DoLookupTransaction(MEMORY_DATABASE, function_name, &scratch_buf)) {
      id->IncRef(cache);
      cache->Insert(id, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<BlockMemory*> mcfg_list;
    BlockMemory::ReadList(&read_buf, &mcfg_list);

    scratch_buf.Reset();
    read_buf.Reset();

    bool found = false;

    for (size_t ind = 0; ind < mcfg_list.Size(); ind++) {
      BlockMemory *mcfg = mcfg_list[ind];
      BlockId *mcfg_id = mcfg->GetId();

      if (id == mcfg_id)
        found = true;

      mcfg_id->IncRef(cache);
      mcfg->MoveRef(NULL, cache);
      cache->Insert(mcfg_id, mcfg);
    }

    if (!found) {
      id->IncRef(cache);
      cache->Insert(id, NULL);
    }
  }

  void Remove(Cache_BlockMemory *cache, BlockId *id, BlockMemory *mcfg)
  {
    id->DecRef(cache);
    if (mcfg != NULL)
      mcfg->DecRef(cache);
  }
};

ExternalLookup_BlockMemory lookup_BlockMemory;
Cache_BlockMemory BlockMemoryCache(&lookup_BlockMemory, CAP_BLOCK_MEMORY);

void BlockMemoryCacheAddList(const Vector<BlockMemory*> &mcfgs,
                             bool consume_references)
{
  for (size_t ind = 0; ind < mcfgs.Size(); ind++) {
    BlockMemory *mcfg = mcfgs[ind];
    BlockId *id = mcfg->GetId();

    if (!consume_references)
      mcfg->IncRef();

    id->IncRef(&BlockMemoryCache);
    mcfg->MoveRef(NULL, &BlockMemoryCache);
    BlockMemoryCache.Insert(id, mcfg);
  }
}

BlockMemory* GetBlockMemory(BlockId *id)
{
  BlockMemory *mcfg = BlockMemoryCache.Lookup(id);

  if (mcfg == NULL) {
    BlockMemoryCache.Release(id);
    return NULL;
  }

  BlockCFG *cfg = GetBlockCFG(id);
  if (cfg) {
    mcfg->SetCFG(cfg);
    cfg->DecRef();
  }

  mcfg->IncRef();
  BlockMemoryCache.Release(id);

  return mcfg;
}

/////////////////////////////////////////////////////////////////////
// BlockModset lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_BlockModset : public Cache_BlockModset::ExternalLookup
{
  void LookupInsert(Cache_BlockModset *cache, BlockId *id)
  {
    Assert(id->Kind() == B_Function || id->Kind() == B_Loop);
    String *function = id->Function();
    const char *function_name = function->Value();

    if (!DoLookupTransaction(MODSET_DATABASE, function_name, &scratch_buf)) {
     missing:
      // ensure there is always a modset, even if empty.
      id->IncRef();
      BlockModset *bmod = BlockModset::Make(id);

      FillBakedModset(bmod);

      id->IncRef(cache);
      bmod->MoveRef(NULL, cache);
      cache->Insert(id, bmod);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<BlockModset*> bmod_list;
    BlockModset::ReadList(&read_buf, &bmod_list);

    scratch_buf.Reset();
    read_buf.Reset();

    bool found = false;

    for (size_t ind = 0; ind < bmod_list.Size(); ind++) {
      BlockModset *bmod = bmod_list[ind];
      BlockId *bmod_id = bmod->GetId();

      if (id == bmod_id)
        found = true;

      // augment this modset with any baked information.
      FillBakedModset(bmod);

      bmod_id->IncRef(cache);
      bmod->MoveRef(NULL, cache);
      cache->Insert(bmod_id, bmod);
    }

    if (!found)
      goto missing;
  }

  void Remove(Cache_BlockModset *cache, BlockId *id, BlockModset *bmod)
  {
    id->DecRef(cache);
    if (bmod != NULL)
      bmod->DecRef(cache);
  }
};

ExternalLookup_BlockModset lookup_BlockModset;
Cache_BlockModset BlockModsetCache(&lookup_BlockModset, CAP_BLOCK_MODSET);

void BlockModsetCacheAddList(const Vector<BlockModset*> &mods,
                             bool consume_references)
{
  for (size_t ind = 0; ind < mods.Size(); ind++) {
    BlockModset *bmod = mods[ind];
    BlockId *id = bmod->GetId();

    if (!consume_references)
      bmod->IncRef();

    id->IncRef(&BlockModsetCache);
    bmod->MoveRef(NULL, &BlockModsetCache);
    BlockModsetCache.Insert(id, bmod);
  }
}

BlockModset* GetBlockModset(BlockId *id)
{
  BlockModset *bmod = BlockModsetCache.Lookup(id);

  Assert(bmod);
  bmod->IncRef();

  BlockModsetCache.Release(id);
  return bmod;
}

/////////////////////////////////////////////////////////////////////
// BlockSummary lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_BlockSummary : public Cache_BlockSummary::ExternalLookup
{
  void LookupInsert(Cache_BlockSummary *cache, BlockId *id)
  {
    Assert(id->Kind() == B_Function || id->Kind() == B_Loop ||
           id->Kind() == B_Initializer);

    // no stored summaries for initializer blocks yet.
    if (id->Kind() == B_Initializer) {
     missing:
      // ensure there is always a summary, even if empty.
      id->IncRef();
      BlockSummary *sum = BlockSummary::Make(id);

      FillBakedSummary(sum);

      id->IncRef(cache);
      sum->MoveRef(NULL, cache);
      cache->Insert(id, sum);
      return;
    }

    String *function = id->Function();
    const char *function_name = function->Value();

    if (!DoLookupTransaction(SUMMARY_DATABASE, function_name, &scratch_buf))
      goto missing;

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<BlockSummary*> sum_list;
    BlockSummary::ReadList(&read_buf, &sum_list);

    scratch_buf.Reset();
    read_buf.Reset();

    bool found = false;

    for (size_t ind = 0; ind < sum_list.Size(); ind++) {
      BlockSummary *sum = sum_list[ind];
      BlockId *sum_id = sum->GetId();

      if (id == sum_id)
        found = true;

      // augment this summary with any baked information.
      FillBakedSummary(sum);

      sum_id->IncRef(cache);
      sum->MoveRef(NULL, cache);
      cache->Insert(sum_id, sum);
    }

    if (!found)
      goto missing;
  }

  void Remove(Cache_BlockSummary *cache, BlockId *id, BlockSummary *sum)
  {
    id->DecRef(cache);
    if (sum != NULL)
      sum->DecRef(cache);
  }
};

ExternalLookup_BlockSummary lookup_BlockSummary;
Cache_BlockSummary BlockSummaryCache(&lookup_BlockSummary, CAP_BLOCK_SUMMARY);

void BlockSummaryCacheAddList(const Vector<BlockSummary*> &sums,
                              bool consume_references)
{
  for (size_t ind = 0; ind < sums.Size(); ind++) {
    BlockSummary *sum = sums[ind];
    BlockId *id = sum->GetId();

    if (!consume_references)
      sum->IncRef();

    id->IncRef(&BlockSummaryCache);
    sum->MoveRef(NULL, &BlockSummaryCache);
    BlockSummaryCache.Insert(id, sum);
  }
}

BlockSummary* GetBlockSummary(BlockId *id)
{
  BlockSummary *sum = BlockSummaryCache.Lookup(id);

  Assert(sum);
  sum->IncRef();

  BlockSummaryCache.Release(id);
  return sum;
}

/////////////////////////////////////////////////////////////////////
// EscapeEdge lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_EscapeEdge
  : public Cache_EscapeEdgeSet::ExternalLookup
{
 public:
  const char *m_database;
  ExternalLookup_EscapeEdge(const char *database)
    : m_database(database)
  {}

  void LookupInsert(Cache_EscapeEdgeSet *cache, Trace *trace)
  {
    String *key = GetTraceKey(trace);

    if (!DoLookupTransaction(m_database, key->Value(), &scratch_buf)) {
      trace->IncRef(cache);
      cache->Insert(trace, NULL);

      key->DecRef();
      return;
    }

    key->DecRef();

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<EscapeEdgeSet*> eset_list;
    EscapeEdgeSet::ReadList(&read_buf, &eset_list);

    scratch_buf.Reset();
    read_buf.Reset();

    bool found = false;

    for (size_t ind = 0; ind < eset_list.Size(); ind++) {
      EscapeEdgeSet *eset = eset_list[ind];
      Trace *use_trace = eset->GetSource();

      if (use_trace == trace)
        found = true;

      use_trace->IncRef(cache);
      eset->MoveRef(NULL, cache);
      cache->Insert(use_trace, eset);
    }

    if (!found) {
      trace->IncRef(cache);
      cache->Insert(trace, NULL);
    }
  }

  void Remove(Cache_EscapeEdgeSet *cache, Trace *trace, EscapeEdgeSet *eset)
  {
    trace->DecRef(cache);
    if (eset != NULL)
      eset->DecRef(cache);
  }
};

ExternalLookup_EscapeEdge
  lookup_EscapeForward(ESCAPE_EDGE_FORWARD_DATABASE);
Cache_EscapeEdgeSet EscapeForwardCache(&lookup_EscapeForward, CAP_ESCAPE_EDGE);

ExternalLookup_EscapeEdge
  lookup_EscapeBackward(ESCAPE_EDGE_BACKWARD_DATABASE);
Cache_EscapeEdgeSet EscapeBackwardCache(&lookup_EscapeBackward, CAP_ESCAPE_EDGE);

/////////////////////////////////////////////////////////////////////
// EscapeAccess lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_EscapeAccess
  : public Cache_EscapeAccessSet::ExternalLookup
{
  void LookupInsert(Cache_EscapeAccessSet *cache, Trace *trace)
  {
    String *key = GetTraceKey(trace);

    if (!DoLookupTransaction(ESCAPE_ACCESS_DATABASE,
                             key->Value(), &scratch_buf)) {
      trace->IncRef(cache);
      cache->Insert(trace, NULL);

      key->DecRef();
      return;
    }

    key->DecRef();

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<EscapeAccessSet*> aset_list;
    EscapeAccessSet::ReadList(&read_buf, &aset_list);

    scratch_buf.Reset();
    read_buf.Reset();

    bool found = false;

    for (size_t ind = 0; ind < aset_list.Size(); ind++) {
      EscapeAccessSet *aset = aset_list[ind];
      Trace *use_trace = aset->GetValue();

      if (use_trace == trace)
        found = true;

      use_trace->IncRef(cache);
      aset->MoveRef(NULL, cache);
      cache->Insert(use_trace, aset);
    }

    if (!found) {
      trace->IncRef(cache);
      cache->Insert(trace, NULL);
    }
  }

  void Remove(Cache_EscapeAccessSet *cache, Trace *trace, EscapeAccessSet *aset)
  {
    trace->DecRef(cache);
    if (aset != NULL)
      aset->DecRef(cache);
  }
};

ExternalLookup_EscapeAccess lookup_EscapeAccess;
Cache_EscapeAccessSet EscapeAccessCache(&lookup_EscapeAccess, CAP_ESCAPE_ACCESS);

/////////////////////////////////////////////////////////////////////
// CallEdge lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_CallEdge : public Cache_CallEdgeSet::ExternalLookup
{
 public:
  const char *m_database;
  ExternalLookup_CallEdge(const char *database)
    : m_database(database)
  {}

  void LookupInsert(Cache_CallEdgeSet *cache, Variable *func)
  {
    if (!DoLookupTransaction(m_database, func->GetName()->Value(), &scratch_buf)) {
      func->IncRef(cache);
      cache->Insert(func, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    CallEdgeSet *cset = CallEdgeSet::Read(&read_buf);

    scratch_buf.Reset();
    read_buf.Reset();

    func->IncRef(cache);
    cset->MoveRef(NULL, cache);
    cache->Insert(func, cset);
  }

  void Remove(Cache_CallEdgeSet *cache, Variable *func, CallEdgeSet *cset)
  {
    func->DecRef(cache);
    if (cset != NULL)
      cset->DecRef(cache);
  }
};

ExternalLookup_CallEdge lookup_Caller(CALLER_DATABASE);
Cache_CallEdgeSet CallerCache(&lookup_Caller, CAP_CALLGRAPH);

ExternalLookup_CallEdge lookup_Callee(CALLEE_DATABASE);
Cache_CallEdgeSet CalleeCache(&lookup_Callee, CAP_CALLGRAPH);

/////////////////////////////////////////////////////////////////////
// Escape merge caches
/////////////////////////////////////////////////////////////////////

static Vector<EscapeEdgeSet*> *g_escape_edge_list = NULL;

class Lookup_MergeEscapeEdge : public MergeEscapeEdge
{
 public:
  bool m_forward;
  Lookup_MergeEscapeEdge(bool forward)
    : MergeEscapeEdge(forward
                      ? ESCAPE_EDGE_FORWARD_DATABASE
                      : ESCAPE_EDGE_BACKWARD_DATABASE),
      m_forward(forward)
  {}

  Trace* GetObjectKey(EscapeEdgeSet *eset) { return eset->GetSource(); }

  EscapeEdgeSet* MakeEmpty(Trace *source)
  {
    source->IncRef();
    EscapeEdgeSet *eset = EscapeEdgeSet::Make(source, m_forward, true);
    Assert(eset->GetEdgeCount() == 0);

    if (g_escape_edge_list)
      g_escape_edge_list->PushBack(eset);
    return eset;
  }

  const char* GetDatabaseKey(String *key) { return key->Value(); }

  void MergeData(MergeExternalData<String,Trace,EscapeEdgeSet> *new_data,
                 Buffer *old_data, Buffer *merged_data)
  {
    while (old_data->pos != old_data->base + old_data->size) {
      Trace *source = NULL;
      bool forward = false;
      Vector<EscapeEdge> edges;

      EscapeEdgeSet::ReadMerge(old_data, &source, &forward, &edges);
      size_t old_count = edges.Size();

      EscapeEdgeSet *new_eset = LookupMarkData(new_data, source);
      if (new_eset) {
        for (size_t eind = 0; eind < new_eset->GetEdgeCount(); eind++)
          edges.PushBack(new_eset->GetEdge(eind));
      }

      EscapeEdgeSet::WriteMerge(merged_data, source, forward, edges);

      // drop references on the old edges.
      source->DecRef();
      for (size_t eind = 0; eind < old_count; eind++) {
        edges[eind].target->DecRef();
        edges[eind].where.id->DecRef();
      }
    }

    // write out any remaining unseen entries.
    Vector<EscapeEdgeSet*> remaining_new_data;
    GetUnmarkedData(new_data, &remaining_new_data);

    for (size_t ind = 0; ind < remaining_new_data.Size(); ind++) {
      EscapeEdgeSet *eset = remaining_new_data[ind];
      EscapeEdgeSet::Write(merged_data, eset);
    }
  }
};

Lookup_MergeEscapeEdge lookup_MergeEscapeForward(true);
MergeEscapeEdge::Cache
  MergeEscapeForwardCache(&lookup_MergeEscapeForward,
                          MERGE_CAP_ESCAPE_EDGE, false);

Lookup_MergeEscapeEdge lookup_MergeEscapeBackward(false);
MergeEscapeEdge::Cache
  MergeEscapeBackwardCache(&lookup_MergeEscapeBackward,
                           MERGE_CAP_ESCAPE_EDGE, false);


static Vector<EscapeAccessSet*> *g_escape_access_list = NULL;

class Lookup_MergeEscapeAccess : public MergeEscapeAccess
{
 public:
  Lookup_MergeEscapeAccess()
    : MergeEscapeAccess(ESCAPE_ACCESS_DATABASE)
  {}

  Trace* GetObjectKey(EscapeAccessSet *aset) { return aset->GetValue(); }

  EscapeAccessSet* MakeEmpty(Trace *value)
  {
    value->IncRef();
    EscapeAccessSet *aset = EscapeAccessSet::Make(value, true);
    Assert(aset->GetAccessCount() == 0);

    if (g_escape_access_list)
      g_escape_access_list->PushBack(aset);
    return aset;
  }

  const char* GetDatabaseKey(String *key) { return key->Value(); }

  void MergeData(MergeExternalData<String,Trace,EscapeAccessSet> *new_data,
                 Buffer *old_data, Buffer *merged_data)
  {
    while (old_data->pos != old_data->base + old_data->size) {
      Trace *value = NULL;
      Vector<EscapeAccess> accesses;

      EscapeAccessSet::ReadMerge(old_data, &value, &accesses);
      size_t old_count = accesses.Size();

      EscapeAccessSet *new_aset = LookupMarkData(new_data, value);
      if (new_aset) {
        for (size_t aind = 0; aind < new_aset->GetAccessCount(); aind++)
          accesses.PushBack(new_aset->GetAccess(aind));
      }

      EscapeAccessSet::WriteMerge(merged_data, value, accesses);

      // drop references on the old accesses.
      value->DecRef();
      for (size_t aind = 0; aind < old_count; aind++) {
        const EscapeAccess &access = accesses[aind];

        access.target->DecRef();
        access.where.id->DecRef();
        if (access.field)
          access.field->DecRef();
      }
    }

    // write out any remaining unseen entries.
    Vector<EscapeAccessSet*> remaining_new_data;
    GetUnmarkedData(new_data, &remaining_new_data);

    // write out any remaining unseen entries.
    for (size_t ind = 0; ind < remaining_new_data.Size(); ind++) {
      EscapeAccessSet *aset = remaining_new_data[ind];
      EscapeAccessSet::Write(merged_data, aset);
    }
  }
};

Lookup_MergeEscapeAccess lookup_MergeEscapeAccess;
Lookup_MergeEscapeAccess::Cache
  MergeEscapeAccessCache(&lookup_MergeEscapeAccess,
                         MERGE_CAP_ESCAPE_ACCESS, false);

/////////////////////////////////////////////////////////////////////
// Callgraph merge caches
/////////////////////////////////////////////////////////////////////

static Vector<CallEdgeSet*> *g_call_edge_list = NULL;

class Lookup_MergeCallEdge : public MergeCallEdge
{
 public:
  bool m_callers;
  Lookup_MergeCallEdge(bool callers)
    : MergeCallEdge(callers ? CALLER_DATABASE : CALLEE_DATABASE),
      m_callers(callers)
  {}

  Variable* GetObjectKey(CallEdgeSet *cset) { return cset->GetFunction(); }

  CallEdgeSet* MakeEmpty(Variable *function)
  {
    function->IncRef();
    CallEdgeSet *cset = CallEdgeSet::Make(function, m_callers, true);
    Assert(cset->GetEdgeCount() == 0);

    if (g_call_edge_list)
      g_call_edge_list->PushBack(cset);
    return cset;
  }

  const char* GetDatabaseKey(Variable *key) { return key->GetName()->Value(); }

  void MergeData(MergeExternalData<Variable,Variable,CallEdgeSet> *new_data,
                 Buffer *old_data, Buffer *merged_data)
  {
    Assert(new_data->single != NULL);
    CallEdgeSet *cset = new_data->single;

    if (old_data->size == 0) {
      // empty old data. write out the new data and return.
      CallEdgeSet::Write(merged_data, cset);
      return;
    }

    Variable *function = NULL;
    bool callers = false;
    Vector<CallEdge> edges;

    // read in the old data.
    CallEdgeSet::ReadMerge(old_data, &function, &callers, &edges);
    Assert(function == cset->GetFunction());
    Assert(callers == cset->IsCallers());
    size_t old_count = edges.Size();

    // add the new edges to the old edges. we shouldn't see any
    // duplicate edges between the two sets.
    for (size_t eind = 0; eind < cset->GetEdgeCount(); eind++)
      edges.PushBack(cset->GetEdge(eind));

    // write out the merged data.
    CallEdgeSet::WriteMerge(merged_data, function, callers, edges);

    // drop references on the old data.
    function->DecRef();
    for (size_t eind = 0; eind < old_count; eind++) {
      edges[eind].where.id->DecRef();
      edges[eind].callee->DecRef();
    }
  }
};

Lookup_MergeCallEdge lookup_MergeCaller(true);
MergeCallEdge::Cache
  MergeCallerCache(&lookup_MergeCaller, MERGE_CAP_CALLGRAPH, false);

Lookup_MergeCallEdge lookup_MergeCallee(false);
MergeCallEdge::Cache
  MergeCalleeCache(&lookup_MergeCallee, MERGE_CAP_CALLGRAPH, false);

/////////////////////////////////////////////////////////////////////
// Merge cache transactions
/////////////////////////////////////////////////////////////////////

void FlushMergeCaches(bool lru_only)
{
  // remove entries from the cache and fill in the flush data
  // for each lookup structure.
  if (lru_only) {
    MergeEscapeForwardCache.RemoveLruEntries(MERGE_TRANSACTION_LIMIT);
    MergeEscapeBackwardCache.RemoveLruEntries(MERGE_TRANSACTION_LIMIT);
    MergeEscapeAccessCache.RemoveLruEntries(MERGE_TRANSACTION_LIMIT);
    MergeCalleeCache.RemoveLruEntries(MERGE_TRANSACTION_LIMIT);
    MergeCallerCache.RemoveLruEntries(MERGE_TRANSACTION_LIMIT);
  }
  else {
    MergeEscapeForwardCache.Clear(MERGE_TRANSACTION_LIMIT);
    MergeEscapeBackwardCache.Clear(MERGE_TRANSACTION_LIMIT);
    MergeEscapeAccessCache.Clear(MERGE_TRANSACTION_LIMIT);
    MergeCalleeCache.Clear(MERGE_TRANSACTION_LIMIT);
    MergeCallerCache.Clear(MERGE_TRANSACTION_LIMIT);
  }

  Transaction *t = new Transaction();

  lookup_MergeEscapeForward.ReadKeys(t);
  lookup_MergeEscapeBackward.ReadKeys(t);
  lookup_MergeEscapeAccess.ReadKeys(t);
  lookup_MergeCallee.ReadKeys(t);
  lookup_MergeCaller.ReadKeys(t);

  if (t->GetActionCount() == 0) {
    // none of the cache flush methods was invoked and the sets remain empty.
    // cleanup and return, we don't need to submit a transaction.
    delete t;
    return;
  }

  // submit the initial transaction to get the contents of each key
  // needed by the flush data lists.
  SubmitTransaction(t);

  Transaction *nt = new Transaction();

  lookup_MergeEscapeForward.WriteKeys(nt, t);
  lookup_MergeEscapeBackward.WriteKeys(nt, t);
  lookup_MergeEscapeAccess.WriteKeys(nt, t);
  lookup_MergeCallee.WriteKeys(nt, t);
  lookup_MergeCaller.WriteKeys(nt, t);

  // submit the final transaction to write out the merged data.
  SubmitTransaction(nt);

  lookup_MergeEscapeForward.CheckWrite(nt, &MergeEscapeForwardCache);
  lookup_MergeEscapeBackward.CheckWrite(nt, &MergeEscapeBackwardCache);
  lookup_MergeEscapeAccess.CheckWrite(nt, &MergeEscapeAccessCache);
  lookup_MergeCallee.CheckWrite(nt, &MergeCalleeCache);
  lookup_MergeCaller.CheckWrite(nt, &MergeCallerCache);

  delete t;
  delete nt;
}

bool MergeCachesEmpty(bool lru_only)
{
  bool empty = true;

  if (lru_only) {
    empty &= !MergeEscapeForwardCache.HasLru();
    empty &= !MergeEscapeBackwardCache.HasLru();
    empty &= !MergeEscapeAccessCache.HasLru();
    empty &= !MergeCalleeCache.HasLru();
    empty &= !MergeCallerCache.HasLru();
  }
  else {
    empty &= MergeEscapeForwardCache.IsEmpty();
    empty &= MergeEscapeBackwardCache.IsEmpty();
    empty &= MergeEscapeAccessCache.IsEmpty();
    empty &= MergeCalleeCache.IsEmpty();
    empty &= MergeCallerCache.IsEmpty();
  }

  return empty;
}

void SetStaticMergeCaches(Vector<EscapeEdgeSet*> *escape_edge_list,
                          Vector<EscapeAccessSet*> *escape_access_list,
                          Vector<CallEdgeSet*> *call_edge_list)
{
  MergeEscapeForwardCache.SetLruEviction(false);
  MergeEscapeBackwardCache.SetLruEviction(false);
  MergeEscapeAccessCache.SetLruEviction(false);
  MergeCalleeCache.SetLruEviction(false);
  MergeCallerCache.SetLruEviction(false);

  g_escape_edge_list = escape_edge_list;
  g_escape_access_list = escape_access_list;
  g_call_edge_list = call_edge_list;
}


/////////////////////////////////////////////////////////////////////
// Memory data compression
/////////////////////////////////////////////////////////////////////

TOperandString* BlockMemoryCompress(Transaction *t,
                              const Vector<BlockMemory*> &mcfgs)
{
  Buffer *data = new Buffer();
  t->AddBuffer(data);

  BlockMemory::WriteList(&scratch_buf, mcfgs);
  CompressBufferInUse(&scratch_buf, data);
  scratch_buf.Reset();

  return new TOperandString(t, data->base, data->pos - data->base);
}

void BlockMemoryUncompress(Transaction *t, size_t var_result,
                           Vector<BlockMemory*> *mcfgs)
{
  TOperandString *op_data = t->LookupString(var_result);
  if (op_data->GetDataLength() == 0)
    return;

  Buffer read_buf(op_data->GetData(), op_data->GetDataLength());
  UncompressBuffer(&read_buf, &scratch_buf);
  Buffer data(scratch_buf.base, scratch_buf.pos - scratch_buf.base);

  BlockMemory::ReadList(&data, mcfgs);
  scratch_buf.Reset();
}

TOperandString* BlockModsetCompress(Transaction *t,
                                    const Vector<BlockModset*> &mods)
{
  Buffer *data = new Buffer();
  t->AddBuffer(data);

  BlockModset::WriteList(&scratch_buf, mods);
  CompressBufferInUse(&scratch_buf, data);
  scratch_buf.Reset();

  return new TOperandString(t, data->base, data->pos - data->base);
}

void BlockModsetUncompress(Transaction *t, TOperandString *op_data,
                           Vector<BlockModset*> *mods)
{
  if (op_data->GetDataLength() == 0)
    return;

  Buffer read_buf(op_data->GetData(), op_data->GetDataLength());
  UncompressBuffer(&read_buf, &scratch_buf);
  Buffer data(scratch_buf.base, scratch_buf.pos - scratch_buf.base);

  BlockModset::ReadList(&data, mods);
  scratch_buf.Reset();
}

TOperandString* BlockSummaryCompress(Transaction *t,
                               const Vector<BlockSummary*> &sums)
{
  Buffer *data = new Buffer();
  t->AddBuffer(data);

  BlockSummary::WriteList(&scratch_buf, sums);
  CompressBufferInUse(&scratch_buf, data);
  scratch_buf.Reset();

  return new TOperandString(t, data->base, data->pos - data->base);
}

void BlockSummaryUncompress(Transaction *t, TOperandString *op_data,
                            Vector<BlockSummary*> *sums)
{
  if (op_data->GetDataLength() == 0)
    return;

  Buffer read_buf(op_data->GetData(), op_data->GetDataLength());
  UncompressBuffer(&read_buf, &scratch_buf);
  Buffer data(scratch_buf.base, scratch_buf.pos - scratch_buf.base);

  BlockSummary::ReadList(&data, sums);
  scratch_buf.Reset();
}

CallEdgeSet* GetIndirectCallEdges(Variable *function)
{
  // get the call edge set computed by ProcessCFGIndirect.
  MergeCallEdge *callee_lookup =
    (MergeCallEdge*) MergeCalleeCache.GetExternalLookup();

  return callee_lookup->LookupSingle(&MergeCalleeCache,
                                     function, function, false);
}

void GetCalleeModsets(Transaction *t, Variable *function,
                      const Vector<BlockCFG*> &function_cfgs,
                      const char *dependency_hash)
{
  Vector<Variable*> callees;

  // process direct callees.
  for (size_t cind = 0; cind < function_cfgs.Size(); cind++) {
    BlockCFG *cfg = function_cfgs[cind];
    for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
      PEdgeCall *edge = cfg->GetEdge(eind)->IfCall();
      if (!edge)
        continue;

      if (Variable *function = edge->GetDirectFunction())
        callees.PushBack(function);
    }
  }

  // process indirect callees.
  CallEdgeSet *indirect_callees = GetIndirectCallEdges(function);
  if (indirect_callees) {
    for (size_t eind = 0; eind < indirect_callees->GetEdgeCount(); eind++) {
      const CallEdge &edge = indirect_callees->GetEdge(eind);
      callees.PushBack(edge.callee);
    }
  }

  // distinct callee functions we need to fetch the modset for.
  Vector<Variable*> fetch_callees;

  for (size_t ind = 0; ind < callees.Size(); ind++) {
    Variable *callee = callees[ind];

    if (fetch_callees.Contains(callee))
      continue;

    // check if there is already a modset for this function in the cache.
    callee->IncRef();
    BlockId *id = BlockId::Make(B_Function, callee);

    if (!BlockModsetCache.IsMember(id)) {
      fetch_callees.PushBack(callee);
    }
    else {
      // the modset cache should be kept flushed between functions
      // if we are adding dependencies on the modset data.
      Assert(dependency_hash == NULL);
    }

    id->DecRef();
  }

  if (fetch_callees.Size()) {
    size_t modset_list_result = t->MakeVariable(true);

    // make a transaction to fetch all the callee modsets.

    TOperand *modset_list_arg = new TOperandVariable(t, modset_list_result);
    TOperand *function_arg = new TOperandString(t, function->GetName()->Value());

    size_t modset_data_var = t->MakeVariable();
    TOperand *modset_data_arg = new TOperandVariable(t, modset_data_var);

    Vector<TOperand*> empty_list_args;
    t->PushAction(Backend::ListCreate(t, empty_list_args,
                                      modset_list_result));

    for (size_t find = 0; find < fetch_callees.Size(); find++) {
      Variable *callee = fetch_callees[find];
      TOperand *callee_arg = new TOperandString(t, callee->GetName()->Value());

      if (dependency_hash != NULL)
        t->PushAction(Backend::HashInsertValue(t, dependency_hash,
                                               callee_arg, function_arg));
      t->PushAction(Backend::XdbLookup(t, MODSET_DATABASE,
                                       callee_arg, modset_data_var));
      t->PushAction(Backend::ListPush(t, modset_list_arg, modset_data_arg,
                                      modset_list_result));
    }

    SubmitTransaction(t);

    // add the fetched modsets to the modset cache.

    TOperandList *modset_list = t->LookupList(modset_list_result);
    for (size_t oind = 0; oind < modset_list->GetCount(); oind++) {
      TOperandString *modset_data = modset_list->GetOperandString(oind);

      Vector<BlockModset*> bmod_list;
      BlockModsetUncompress(t, modset_data, &bmod_list);
      BlockModsetCacheAddList(bmod_list);
    }

    t->Clear();
  }
}

NAMESPACE_XGILL_END
