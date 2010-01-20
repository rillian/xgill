
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

#include "storage.h"

NAMESPACE_XGILL_BEGIN

static Buffer scratch_buf("Buffer_imlang_storage");

// maximum size we will tolerate for the scratch buffer before
// shrinking it back down.
#define SCRATCH_BUF_LIMIT  (10 * 1048576)

void ClearBlockCaches()
{
  BlockCFGCache.Clear();
  InitializerCache.Clear();
  CompositeCSUCache.Clear();

  BodyAnnotCache.Clear();
  InitAnnotCache.Clear();
  CompAnnotCache.Clear();
}

// maximum capacities for the storage caches.

#define CAP_BLOCK_CFG    50000
#define CAP_INITIALIZER  25000
#define CAP_CSU          50000
#define CAP_ANNOTATION   100000

/////////////////////////////////////////////////////////////////////
// BlockCFG lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_BlockCFG : public Cache_BlockCFG::ExternalLookup
{
  void LookupInsert(Cache_BlockCFG *cache, BlockId *id)
  {
    Assert(id->Kind() == B_Function || id->Kind() == B_Loop);
    String *function = id->Function();
    const char *function_name = function->Value();

    if (!DoLookupTransaction(BODY_DATABASE, function_name, &scratch_buf)) {
      id->IncRef(cache);
      cache->Insert(id, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    Vector<BlockCFG*> cfg_list;
    BlockCFG::ReadList(&read_buf, &cfg_list);

    scratch_buf.Reset();

    for (size_t ind = 0; ind < cfg_list.Size(); ind++) {
      BlockCFG *cfg = cfg_list[ind];
      BlockId *id = cfg->GetId();

      id->IncRef(cache);
      cfg->MoveRef(NULL, cache);
      cache->Insert(id, cfg);
    }
  }

  void Remove(Cache_BlockCFG *cache, BlockId *id, BlockCFG *cfg)
  {
    id->DecRef(cache);
    if (cfg != NULL)
      cfg->DecRef(cache);
  }
};

ExternalLookup_BlockCFG lookup_BlockCFG;
Cache_BlockCFG BlockCFGCache(&lookup_BlockCFG, CAP_BLOCK_CFG);

BlockCFG* GetBlockCFG(BlockId *id)
{
  BlockCFG *cfg = NULL;

  switch (id->Kind()) {

  case B_Initializer:
    cfg = InitializerCache.Lookup(id->Function());
    if (cfg)
      cfg->IncRef();
    InitializerCache.Release(id->Function());
    break;

  case B_Function:
  case B_Loop:
    cfg = BlockCFGCache.Lookup(id);
    if (cfg)
      cfg->IncRef();
    BlockCFGCache.Release(id);
    break;

  default:
    Assert(false);
  }

  return cfg;
}

void BlockCFGCacheAddListWithRefs(const Vector<BlockCFG*> &cfgs)
{
  for (size_t ind = 0; ind < cfgs.Size(); ind++) {
    BlockCFG *cfg = cfgs[ind];
    BlockId *id = cfg->GetId();

    id->IncRef(&BlockCFGCache);
    cfg->IncRef(&BlockCFGCache);
    BlockCFGCache.Insert(id, cfg);
  }
}

void BlockCFGUncompress(Transaction *t, size_t var_result,
                        Vector<BlockCFG*> *cfgs)
{
  // if the compression buffer is using an inordinate amount of space
  // then reallocate it. some CFGs (particularly static initializers)
  // are gigantic, even if their compressed size is small; there's just
  // lots of redundancy.
  if (scratch_buf.size > SCRATCH_BUF_LIMIT)
    scratch_buf.Reset(SCRATCH_BUF_LIMIT);

  TOperandString *op_data = t->LookupString(var_result);

  // check for unknown functions.
  if (op_data->GetDataLength() == 0)
    return;

  TOperandString::Uncompress(op_data, &scratch_buf);
  Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);

  BlockCFG::ReadList(&read_buf, cfgs);
  scratch_buf.Reset();
}

/////////////////////////////////////////////////////////////////////
// Initializer lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_Initializer : public Cache_Initializer::ExternalLookup
{
  void LookupInsert(Cache_Initializer *cache, String *var)
  {
    if (!DoLookupTransaction(INIT_DATABASE, var->Value(), &scratch_buf)) {
      var->IncRef(cache);
      cache->Insert(var, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG *cfg = BlockCFG::Read(&read_buf);

    scratch_buf.Reset();

    var->IncRef(cache);
    cfg->MoveRef(NULL, cache);
    cache->Insert(var, cfg);
  }

  void Remove(Cache_Initializer *cache, String *var, BlockCFG *cfg)
  {
    var->DecRef(cache);
    if (cfg != NULL)
      cfg->DecRef(cache);
  }
};

ExternalLookup_Initializer lookup_Initializer;
Cache_Initializer InitializerCache(&lookup_Initializer, CAP_INITIALIZER);

/////////////////////////////////////////////////////////////////////
// CompositeCSU lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_CompositeCSU : public Cache_CompositeCSU::ExternalLookup
{
  void LookupInsert(Cache_CompositeCSU *cache, String *name)
  {
    if (!DoLookupTransaction(COMP_DATABASE, name->Value(), &scratch_buf)) {
      name->IncRef(cache);
      cache->Insert(name, NULL);
      return;
    }

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    CompositeCSU *csu = CompositeCSU::Read(&read_buf);

    scratch_buf.Reset();

    name->IncRef(cache);
    csu->MoveRef(NULL, cache);
    cache->Insert(name, csu);
  }

  void Remove(Cache_CompositeCSU *cache, String *name, CompositeCSU *csu)
  {
    name->DecRef(cache);
    if (csu != NULL)
      csu->DecRef(cache);
  }
};

ExternalLookup_CompositeCSU lookup_CompositeCSU;
Cache_CompositeCSU CompositeCSUCache(&lookup_CompositeCSU, CAP_CSU);

/////////////////////////////////////////////////////////////////////
// Annotation lookup
/////////////////////////////////////////////////////////////////////

class ExternalLookup_Annotation : public Cache_Annotation::ExternalLookup
{
public:
  const char *m_db_name;
  ExternalLookup_Annotation(const char *_db_name) : m_db_name(_db_name) {}

  void LookupInsert(Cache_Annotation *cache, String *name)
  {
    name->IncRef(cache);

    if (!DoLookupTransaction(m_db_name, name->Value(), &scratch_buf)) {
      cache->Insert(name, NULL);
      return;
    }

    Vector<BlockCFG*> *cfg_list = new Vector<BlockCFG*>();

    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG::ReadList(&read_buf, cfg_list);

    scratch_buf.Reset();

    for (size_t ind = 0; ind < cfg_list->Size(); ind++)
      cfg_list->At(ind)->MoveRef(NULL, cfg_list);
    cache->Insert(name, cfg_list);
  }

  void Remove(Cache_Annotation *cache,
              String *name, Vector<BlockCFG*> *cfg_list)
  {
    name->DecRef(cache);

    if (cfg_list) {
      for (size_t ind = 0; ind < cfg_list->Size(); ind++)
        cfg_list->At(ind)->DecRef(cfg_list);
      delete cfg_list;
    }
  }
};

ExternalLookup_Annotation lookup_BodyAnnot(BODY_ANNOT_DATABASE);
ExternalLookup_Annotation lookup_InitAnnot(INIT_ANNOT_DATABASE);
ExternalLookup_Annotation lookup_CompAnnot(COMP_ANNOT_DATABASE);

Cache_Annotation BodyAnnotCache(&lookup_BodyAnnot, CAP_ANNOTATION);
Cache_Annotation InitAnnotCache(&lookup_InitAnnot, CAP_ANNOTATION);
Cache_Annotation CompAnnotCache(&lookup_CompAnnot, CAP_ANNOTATION);

NAMESPACE_XGILL_END
