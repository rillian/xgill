
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

#include <stdio.h>
#include <unistd.h>
#include <imlang/block.h>
#include <imlang/storage.h>
#include <memory/block.h>
#include <memory/escape.h>
#include <memory/storage.h>
#include <backend/backend_compound.h>
#include <util/config.h>
#include <util/monitor.h>

#include <solve/solver.h>

NAMESPACE_XGILL_USING

const char *USAGE = "xmemlocal [options] [function*]";

// hash name for the modset dependency information (if it is being generated).
#define MODSET_DEPENDENCY_HASH "dependency_modset"

// scratch buffer for database compression.
static Buffer compress_buf("Buffer_xmemlocal");

ConfigOption skip_indirect(CK_Flag, "skip-indirect", NULL,
                           "skip generation of indirect calls");

ConfigOption do_fixpoint(CK_Flag, "do-fixpoint", NULL,
                         "perform a fixpoint computation on modsets");

ConfigOption print_cfgs(CK_Flag, "print-cfgs", NULL,
                        "print input CFGs");

ConfigOption print_memory(CK_Flag, "print-memory", NULL,
                          "print generated memory information");

void MakeInitTransaction(Transaction *t, const Vector<const char*> &functions)
{
  // clear the output databases if we are processing all functions.
  if (functions.Empty()) {
    t->PushAction(
      Backend::Compound::XdbClearIfNotHash(
        t, MEMORY_DATABASE, WORKLIST_FUNC_HASH));
    t->PushAction(
      Backend::Compound::XdbClearIfNotHash(
        t, MODSET_DATABASE, WORKLIST_FUNC_HASH));
  }

  if (functions.Size()) {
    // insert the initial function/global names.
    for (size_t uind = 0; uind < functions.Size(); uind++) {
      TOperand *key = new TOperandString(t, functions[uind]);
      t->PushAction(Backend::HashInsertKey(t, WORKLIST_FUNC_HASH, key));
      t->PushAction(Backend::HashInsertKey(t, WORKLIST_GLOB_HASH, key));
    }
  }
  else {
    // fill in the worklist hash from the body and initializer databases.

    t->PushAction(
      Backend::Compound::HashCreateXdbKeys(
        t, WORKLIST_FUNC_HASH, BODY_DATABASE));

    t->PushAction(
      Backend::Compound::HashCreateXdbKeys(
        t, WORKLIST_GLOB_HASH, INIT_DATABASE));
  }
}

void MakeFetchTransaction(Transaction *t,
                          size_t body_key_result, size_t body_data_result,
                          size_t glob_key_result, size_t glob_data_result,
                          size_t modset_data_result)
{
  t->PushAction(
    Backend::Compound::HashRunIfEmpty(t, WORKLIST_FUNC_HASH,
      Backend::Compound::HashPopXdbKey(
        t, WORKLIST_GLOB_HASH, INIT_DATABASE,
        glob_key_result, glob_data_result)));

  t->PushAction(
    Backend::Compound::HashPopXdbKey(
      t, WORKLIST_FUNC_HASH, BODY_DATABASE,
      body_key_result, body_data_result));

  TOperand *body_key_arg = new TOperandVariable(t, body_key_result);

  t->PushAction(
    Backend::XdbLookup(
      t, MODSET_DATABASE, body_key_arg, modset_data_result));
}

void MakeUpdateTransaction(Transaction *t,
                           Buffer *key_buf, Buffer *old_modset_data,
                           const Vector<BlockMemory*> &function_mems,
                           const Vector<BlockModset*> &function_mods)
{
  TOperand *function_arg =
    new TOperandString(t, (const char*) key_buf->base);

  TOperandString *memory_data_arg = BlockMemoryCompress(t, function_mems);

  t->PushAction(Backend::XdbReplace(t, MEMORY_DATABASE,
                                    function_arg, memory_data_arg));

  if (!function_mods.Empty()) {
    TOperandString *modset_data_arg = BlockModsetCompress(t, function_mods);

    bool modset_changed = false;
    if (old_modset_data == NULL)
      modset_changed = true;
    else if (old_modset_data->size != modset_data_arg->GetDataLength())
      modset_changed = true;
    else if (memcmp(old_modset_data->base, modset_data_arg->GetData(),
                    old_modset_data->size) != 0)
      modset_changed = true;

    if (modset_changed && do_fixpoint.IsSpecified()) {
      t->PushAction(
        Backend::Compound::UpdateDependency(
          t, MODSET_DEPENDENCY_HASH, function_arg, WORKLIST_FUNC_HASH));
    }

    t->PushAction(Backend::XdbReplace(t, MODSET_DATABASE,
                                      function_arg, modset_data_arg));
  }
}

// how often to print allocation information.
#define PRINT_ALLOC_FREQUENCY 50
size_t g_alloc_counter = 0;

void RunAnalysis(const Vector<const char*> &functions)
{
  static BaseTimer analysis_timer("xmemlocal_main");
  Transaction *t = new Transaction();

  // we will manually manage clearing of entries in the modset cache.
  BlockModsetCache.SetLruEviction(false);

  // construct and submit the worklist create transaction
  MakeInitTransaction(t, functions);
  SubmitTransaction(t);
  t->Clear();

  // generate modset dependency information if we will be fixpointing.
  const char *dependency_hash = NULL;
  if (do_fixpoint.IsSpecified())
    dependency_hash = MODSET_DEPENDENCY_HASH;

  while (true) {
    Timer _timer(&analysis_timer);

    g_alloc_counter++;
    g_alloc_counter %= PRINT_ALLOC_FREQUENCY;

    if (g_alloc_counter == 0) {
      PrintTimers();
      PrintAllocs();
    }

    // construct and submit a worklist fetch transaction.
    size_t body_key_result = t->MakeVariable(true);
    size_t body_data_result = t->MakeVariable(true);
    size_t glob_key_result = t->MakeVariable(true);
    size_t glob_data_result = t->MakeVariable(true);
    size_t modset_data_result = t->MakeVariable(true);
    MakeFetchTransaction(t, body_key_result, body_data_result,
                         glob_key_result, glob_data_result,
                         modset_data_result);
    SubmitTransaction(t);

    TOperandString *body_key = t->LookupString(body_key_result);
    TOperandString *glob_key = t->LookupString(glob_key_result, false);

    // make a copy of the function/initializer name.
    Buffer key_buf;

    Vector<BlockCFG*> block_cfgs;
    bool is_function;

    if (body_key->GetDataLength() > 1) {
      key_buf.Append(body_key->GetData(), body_key->GetDataLength());
      BlockCFGUncompress(t, body_data_result, &block_cfgs);
      is_function = true;
    }
    else if (glob_key && glob_key->GetDataLength() > 1) {
      key_buf.Append(glob_key->GetData(), glob_key->GetDataLength());
      BlockCFGUncompress(t, glob_data_result, &block_cfgs);
      is_function = false;
    }
    else {
      // done with all functions and initializers.
      t->Clear();
      break;
    }

    if (block_cfgs.Empty()) {
      t->Clear();
      continue;
    }

    // make a copy of the modset data. watch out for the case where
    // there is no previous modset data.

    TOperandString *modset_data = t->LookupString(modset_data_result);

    size_t modset_data_len = modset_data->GetDataLength();
    Buffer modset_data_buf(modset_data_len);
    if (modset_data_len) {
      Assert(is_function);
      memcpy(modset_data_buf.base, modset_data->GetData(), modset_data_len);
    }

    // done with the transaction's returned data.
    t->Clear();

    Vector<BlockMemory*> block_mems;
    Vector<BlockModset*> block_mods;

    logout << "Generating memory for "
           << "'" << (const char*) key_buf.base << "'" << endl << flush;

    if (is_function) {
      Variable *function = block_cfgs[0]->GetId()->BaseVar();

      if (!skip_indirect.IsSpecified()) {
        // process any indirect calls in the CFGs. this will fill in the
        // merge cache for the function so that GetIndirectCallEdges
        // works during memory/modset computation.
        for (size_t cind = 0; cind < block_cfgs.Size(); cind++)
          CallgraphProcessCFGIndirect(block_cfgs[cind]);
      }

      if (dependency_hash) {
        // clear out any old modsets from the cache.
        BlockModsetCache.Clear();
      }

      // pull in callee modsets and add dependencies on them.
      GetCalleeModsets(t, function, block_cfgs, dependency_hash);
    }

    // did we have a timeout while processing the CFGs?
    bool had_timeout = false;

    // generate memory information and (possibly) modsets for each CFG.
    for (size_t cind = 0; cind < block_cfgs.Size(); cind++) {
      // set a soft timeout for memory/modset computation.
      // we don't set a hard timeout as these break the indirect callgraph.
      if (uint32_t timeout = GetTimeout())
        TimerAlarm::StartActive(timeout);

      BlockCFG *cfg = block_cfgs[cind];
      BlockId *id = cfg->GetId();

      if (print_cfgs.IsSpecified())
        logout << cfg << endl;

      id->IncRef();
      BlockMemory *mem =
        BlockMemory::Make(id, MSIMP_Scalar, MALIAS_Buffer, MCLB_Modset);

      if (is_function) {
        Variable *function = id->BaseVar();
        String *loop = id->Loop();

        // make a modset which we will fill in the new modset data from.
        // this uses a scratch ID because in the case of direct recursion
        // we may need to refer to the modset for the id when generating
        // this modset.
        function->IncRef();
        if (loop)
          loop->IncRef();
        BlockId *scratch_id = BlockId::Make(B_Scratch, function, loop);
        BlockModset *scratch_mod = BlockModset::Make(scratch_id);

        mem->SetCFG(cfg);
        mem->ComputeTables();

        if (!TimerAlarm::ActiveExpired())
          scratch_mod->ComputeModset(mem);

        // update the modset for the real ID and get rid of the scratch modset.
        id->IncRef();
        BlockModset *mod = BlockModset::Make(id);
        mod->CopyModset(scratch_mod);
        scratch_mod->DecRef();

        logout << "Computed modset:" << endl << mod << endl;

        block_mods.PushBack(mod);

        // add an entry to the modset cache. we process the function CFGs from
        // innermost loop to the outermost function, and will add loop modsets
        // to the cache as we go.
        id->IncRef(&BlockModsetCache);
        mod->IncRef(&BlockModsetCache);
        BlockModsetCache.Insert(id, mod);
      }
      else {
        mem->SetCFG(cfg);
        mem->ComputeTables();
      }

      if (print_memory.IsSpecified()) {
        logout << "Computed memory:" << endl;
        mem->Print(logout);
        logout << endl;
      }

      block_mems.PushBack(mem);
      logout << endl;

      if (TimerAlarm::ActiveExpired()) {
        logout << "ERROR: Timeout while generating memory: ";
	PrintTime(TimerAlarm::ActiveElapsed());
	logout << endl;

        had_timeout = true;
      }

      TimerAlarm::ClearActive();
    }

    // write out the generated modsets and memory. skip this if there was
    // a timeout, in which case the data is incomplete.
    if (!had_timeout) {
      MakeUpdateTransaction(t, &key_buf,
                            modset_data_len ? &modset_data_buf : NULL,
                            block_mems, block_mods);
      SubmitTransaction(t);
      t->Clear();
    }

    // clear out held references on CFGs, memory and modset information.
    for (size_t find = 0; find < block_cfgs.Size(); find++)
      block_cfgs[find]->DecRef();
    for (size_t find = 0; find < block_mems.Size(); find++)
      block_mems[find]->DecRef();
    for (size_t find = 0; find < block_mods.Size(); find++)
      block_mods[find]->DecRef();
  }

  // flush the callgraph caches.
  while (!MergeCachesEmpty(false))
    FlushMergeCaches(false);

  delete t;
}

struct whatever {
  static int Compare(char *a, char *b) { return strcmp(a, b); }
};

int main(int argc, const char **argv)
{
  timeout.Enable();
  trans_remote.Enable();
  trans_initial.Enable();

  // do_fixpoint.Enable();
  skip_indirect.Enable();
  print_cfgs.Enable();
  print_memory.Enable();
  print_indirect_calls.Enable();

  Vector<const char*> functions;
  bool parsed = Config::Parse(argc, argv, &functions);
  if (!parsed) {
    Config::PrintUsage(USAGE);
    return 1;
  }

  // Solver::CheckSimplifications();

  ResetAllocs();
  AnalysisPrepare();

  if (trans_initial.IsSpecified())
    SubmitInitialTransaction();
  RunAnalysis(functions);
  SubmitFinalTransaction();

  ClearBlockCaches();
  ClearMemoryCaches();
  AnalysisFinish(0);
}
