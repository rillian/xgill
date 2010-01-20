
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

#pragma once

#include "callgraph.h"
#include "escape.h"
#include "block.h"
#include "modset.h"
#include "summary.h"
#include <backend/merge_lookup.h>

NAMESPACE_XGILL_BEGIN

// functions and structures for storing and retrieving memory analysis
// data. these all use the BACKEND_XDBHASH backend.

// names of databases storing escape information.
#define ESCAPE_EDGE_FORWARD_DATABASE "escape_edge_forward.xdb"
#define ESCAPE_EDGE_BACKWARD_DATABASE "escape_edge_backward.xdb"
#define ESCAPE_ACCESS_DATABASE "escape_access.xdb"

// name of database storing the callees and callers of a function.
#define CALLER_DATABASE "body_caller.xdb"
#define CALLEE_DATABASE "body_callee.xdb"

// name of database storing per-function/initializer memory information.
#define MEMORY_DATABASE "body_memory.xdb"

// name of database storing per-function modset information.
#define MODSET_DATABASE "body_modset.xdb"

// name of database storing per-function analysis summaries.
#define SUMMARY_DATABASE "body_summary.xdb"

// clear the memory, modset and other caches from the memory representation.
// this does not include the merge caches below.
void ClearMemoryCaches();

// memory information caches.

typedef HashCache<BlockId*,BlockMemory*,BlockId> Cache_BlockMemory;
typedef HashCache<BlockId*,BlockModset*,BlockId> Cache_BlockModset;
typedef HashCache<BlockId*,BlockSummary*,BlockId> Cache_BlockSummary;

extern Cache_BlockMemory BlockMemoryCache;
extern Cache_BlockModset BlockModsetCache;
extern Cache_BlockSummary BlockSummaryCache;

// add entries to the caches without doing an explicit lookup.
// consume_references drops the existing references on values in the list.
void BlockMemoryCacheAddList(const Vector<BlockMemory*> &mcfgs,
                             bool consume_references = true);
void BlockModsetCacheAddList(const Vector<BlockModset*> &mods,
                             bool consume_references = true);
void BlockSummaryCacheAddList(const Vector<BlockSummary*> &sums,
                              bool consume_references = true);

// get a reference on the data for the specified ID. for unknown/failed IDs
// the memory will be NULL and modset/summary will be non-NULL but empty
// (except for any baked information).
BlockMemory* GetBlockMemory(BlockId *id);
BlockModset* GetBlockModset(BlockId *id);
BlockSummary* GetBlockSummary(BlockId *id);

// escape information caches.

// get the key in an escape edge or access database which stores data for lt.
String* GetTraceKey(Trace *trace);

typedef HashCache<Trace*,EscapeEdgeSet*,Trace> Cache_EscapeEdgeSet;
typedef HashCache<Trace*,EscapeAccessSet*,Trace> Cache_EscapeAccessSet;
typedef HashCache<Variable*,CallEdgeSet*,Variable> Cache_CallEdgeSet;

extern Cache_EscapeEdgeSet EscapeForwardCache;
extern Cache_EscapeEdgeSet EscapeBackwardCache;
extern Cache_EscapeAccessSet EscapeAccessCache;

extern Cache_CallEdgeSet CalleeCache;
extern Cache_CallEdgeSet CallerCache;

// merge caches for filling in escape information. these are only used
// when filling in the contents of the corresponding databases.

// get a reference on the key in the escape_edge or escape_access database
// which will be used to access escape information for lt.
String* GetTraceKey(Trace *trace);

// the lookup on these caches returns an empty set, and the flush
// method remembers any entries in the set for merging to the
// existing database. these caches are not automatically evicted;
// if a program uses these caches (i.e. it calls the EscapeProcess
// or CallgraphProcess methods from escape.h and callgraph.h),
// it must call FlushMergeCaches() periodically.

// keys in these caches are the result of GetTraceKey.
typedef MergeExternalLookup<String,Trace,EscapeEdgeSet> MergeEscapeEdge;
typedef MergeExternalLookup<String,Trace,EscapeAccessSet> MergeEscapeAccess;

extern MergeEscapeEdge::Cache MergeEscapeForwardCache;
extern MergeEscapeEdge::Cache MergeEscapeBackwardCache;
extern MergeEscapeAccess::Cache MergeEscapeAccessCache;

typedef MergeExternalLookup<Variable,Variable,CallEdgeSet> MergeCallEdge;

extern MergeCallEdge::Cache MergeCalleeCache;
extern MergeCallEdge::Cache MergeCallerCache;

// flush changes in the merge caches to disk and remove them from
// the caches. if lru_only is true then only the LRU entries are removed
// (according to the cache capacity), otherwise all entries up to a
// small fixed limit are removed.
void FlushMergeCaches(bool lru_only);

// returns whether all of the merge caches are empty of items to remove.
// if lru_only is true this includes only items exceeding the LRU limits
// of the cache, otherwise all entries.
bool MergeCachesEmpty(bool lru_only);

// disable LRU eviction on the merge caches and use the specified lists
// to store all entries that have been inserted into the merge caches.
void SetStaticMergeCaches(Vector<EscapeEdgeSet*> *escape_edge_list,
                          Vector<EscapeAccessSet*> *escape_access_list,
                          Vector<CallEdgeSet*> *call_edge_list);

// read/write lists of compressed memory info in transaction operations.
TOperandString* BlockMemoryCompress(Transaction *t,
                                    const Vector<BlockMemory*> &mcfgs);
void BlockMemoryUncompress(Transaction *t, size_t var_result,
                           Vector<BlockMemory*> *mcfgs);

// read/write lists of compressed modsets in transaction operations.
TOperandString* BlockModsetCompress(Transaction *t,
                                    const Vector<BlockModset*> &mods);
void BlockModsetUncompress(Transaction *t, TOperandString *op_data,
                           Vector<BlockModset*> *mods);

// read/write lists of compressed summaries in transaction operations.
TOperandString* BlockSummaryCompress(Transaction *t,
                                     const Vector<BlockSummary*> &sums);
void BlockSummaryUncompress(Transaction *t, TOperandString *op_data,
                            Vector<BlockSummary*> *sums);

// during indirect call generation, get the set of indirect calls generated
// for the specified function, NULL if there are none. this should only
// be called immediately after the indirect calls have been generated,
// i.e. during memory or modset construction.
CallEdgeSet* GetIndirectCallEdges(Variable *function);

// load into the modset cache the modsets for all callees of function.
// if dependency_hash is specified then function_name will be added to the
// hash for each queried callee. this should only be called under the
// same situations as GetIndirectCallEdges.
void GetCalleeModsets(Transaction *t, Variable *function,
                      const Vector<BlockCFG*> &function_cfgs,
                      const char *dependency_hash = NULL);

NAMESPACE_XGILL_END
