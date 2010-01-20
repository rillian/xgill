
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

// functions and data for storing and retrieving intermediate language
// data structures.

#include "block.h"
#include <backend/backend_compound.h>

NAMESPACE_XGILL_BEGIN

// database name containing function/initializer CFGs.
#define BODY_DATABASE "src_body.xdb"
#define INIT_DATABASE "src_init.xdb"

// database name containing CSU type definitions.
#define COMP_DATABASE "src_comp.xdb"

// database name containing function/initializer/type annotation CFGs.
#define BODY_ANNOT_DATABASE "annot_body.xdb"
#define INIT_ANNOT_DATABASE "annot_init.xdb"
#define COMP_ANNOT_DATABASE "annot_comp.xdb"

// plaintext file with topological ordering for source functions.
#define BODY_SORT_FILE "src_body_topo.sort"

// hash name and sort for the callgraph we will topo sort from.
#define CALLGRAPH_HASH "callgraph_hash"
#define CALLGRAPH_SORT "callgraph_sort"

// database to receive contents of files.
#define SOURCE_DATABASE "file_source.xdb"

// database to receive contents of preprocessed files.
#define PREPROC_DATABASE "file_preprocess.xdb"

// hash storing files which have already been processed.
#define PROCESSED_FILES_HASH  "processed_files_hash"

// commonly used transaction hash and sort names.

// hash name for worklists.
#define WORKLIST_FUNC_HASH "worklist_func_hash"
#define WORKLIST_GLOB_HASH "worklist_glob_hash"
#define WORKLIST_COMP_HASH "worklist_comp_hash"

// sort name for function worklist.
#define WORKLIST_FUNC_SORT "worklist_func_sort"

// cache lookup structures.

typedef HashCache<BlockId*,BlockCFG*,BlockId>    Cache_BlockCFG;
typedef HashCache<String*,BlockCFG*,String>      Cache_Initializer;
typedef HashCache<String*,CompositeCSU*,String>  Cache_CompositeCSU;

// caches for looking up CFGs, initializers, and CSU types.
extern Cache_BlockCFG      BlockCFGCache;
extern Cache_Initializer   InitializerCache;
extern Cache_CompositeCSU  CompositeCSUCache;

typedef HashCache<String*,Vector<BlockCFG*>*,String>  Cache_Annotation;

// caches for looking up annotations on functions, initializers or CSU types.
extern Cache_Annotation  BodyAnnotCache;
extern Cache_Annotation  InitAnnotCache;
extern Cache_Annotation  CompAnnotCache;

// gets a reference on the CFG for id, loading it if necessary.
// NULL if the CFG was not found.
BlockCFG* GetBlockCFG(BlockId *id);

// add entries to the CFG cache without doing an explicit lookup.
// does *not* consume references on the CFGs.
void BlockCFGCacheAddListWithRefs(const Vector<BlockCFG*> &cfgs);

// clear out CFG and other intermediate language caches.
void ClearBlockCaches();

// read lists of compressed CFGs in transaction operations.
void BlockCFGUncompress(Transaction *t, size_t var_result,
                        Vector<BlockCFG*> *cfgs);

NAMESPACE_XGILL_END
