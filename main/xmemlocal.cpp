
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
#include <backend/backend_block.h>
#include <backend/backend_compound.h>
#include <util/config.h>
#include <util/monitor.h>

#include <solve/solver.h>

NAMESPACE_XGILL_USING

const char *USAGE = "xmemlocal [options] [function*]";

// the memory model for a function is entirely determined from its CFG
// and from the modsets of its callees. getting complete modsets is an
// iterative process due to recursion, and we want to make this iteration
// deterministic. the process for computing memory and modsets is as follows:
//
// 1. compute the memory/modsets for all but the last stage of the callgraph.
// 2. run one pass over the last stage of the callgraph to compute modset
// dependencies and the first cut of the modsets for these functions.
// 3. run additional passes over the last stage of the callgraph.
// maintain a worklist of the functions to analyze in the next pass
// (any function with a callee whose modset changed in the current pass).

// new memory or modset data is not written to the databases until a pass
// is finished. the next pass does not start until after this writing is done.
// this is managed using a barrier counter, which indicates the number of
// outstanding workers for the current pass which have made new memory/modset
// data but have not written it out yet.

// hash name for the modset dependency information (if it is being generated).
#define MODSET_DEPENDENCY_HASH "dependency_modset"

// scratch buffer for database compression.
static Buffer compress_buf("Buffer_xmemlocal");

ConfigOption pass_limit(CK_UInt, "pass-limit", "2",
                        "maximum number of passes to perform");

ConfigOption print_cfgs(CK_Flag, "print-cfgs", NULL,
                        "print input CFGs");

ConfigOption print_memory(CK_Flag, "print-memory", NULL,
                          "print generated memory information");

// counter indicating the index of the *next* pass we'll be processing.
#define COUNTER_STAGE "counter"

// counters used to make sure pending memory/modset data is written at the
// right point. BARRIER_PROCESS indicates the number of workers that have
// processed at least one function for the current stage and have not finished
// with the stage yet. BARRIER_WRITE indicates the number of workers that
// have finished processing functions for the stage but have not written
// out their results yet. results cannot be written out until BARRIER_PROCESS
// becomes zero, and the next stage cannot be started until both counters
// become zero.
#define BARRIER_PROCESS "barrier_process"
#define BARRIER_WRITE "barrier_write"

// number of callgraph stages.
static size_t g_stage_count = 0;

// make a transaction to get the next key from the worklist. the body data
// will not be set if there are no nodes remaining in the worklist.
// have_barrier increments whether we have a reference on the barrier counter;
// a reference will be added if a key is found and have_barrier is not set.
// if the worklist is empty, advances to the next stage if the counters are ok.
void MakeFetchTransaction(Transaction *t, bool have_barrier_process,
                          size_t stage_result,
                          size_t body_data_result, size_t modset_data_result,
                          size_t barrier_process_result,
                          size_t barrier_write_result)
{
  // $stage = CounterValue(stage)
  // $body_key = HashChooseKey(worklist_name)
  // $key_empty = StringIsEmpty($body_key)
  // if !$key_empty
  //   HashRemove(worklist_name, $body_key)
  //   $body_data = XdbLookup(src_body, $body_key)
  //   $modset_data = XdbLookup(src_modset, $body_key)
  //   CounterInc(barrier_process) // if !have_barrier_process
  // if $key_empty
  //   $barrier_process = CounterValue(barrier_process)
  //   $barrier_write = CounterValue(barrier_write)
  //   $process_done = ValueEqual($barrier_process, 0)
  //   $write_done = ValueEqual($barrier_write, 0)
  //   if $process_done
  //     if $write_done
  //       $use_sort = ValueLessEqual($stage, stage_count)
  //       if $use_sort
  //         $next_list = GraphSortKeys(callgraph, $stage)
  //       if !$use_sort
  //         $next_list = HashAllKeys(worklist_next)
  //         HashClear(worklist_next)
  //         BlockFlush()
  //       foreach $next_key in $next_list
  //         HashInsertKey(worklist_name, $next_key)
  //       CounterInc($stage)
  //       $stage = CounterValue($stage)

  TOperand *stage = new TOperandVariable(t, stage_result);

  TRANSACTION_MAKE_VAR(body_key);
  TRANSACTION_MAKE_VAR(key_empty);

  t->PushAction(Backend::CounterValue(t, COUNTER_STAGE, stage_result));
  t->PushAction(Backend::HashChooseKey(t, WORKLIST_FUNC_HASH, body_key_var));
  t->PushAction(Backend::StringIsEmpty(t, body_key, key_empty_var));

  TActionTest *non_empty_branch = new TActionTest(t, key_empty, false);
  t->PushAction(non_empty_branch);

  non_empty_branch->PushAction(
    Backend::HashRemove(t, WORKLIST_FUNC_HASH, body_key));
  non_empty_branch->PushAction(
    Backend::XdbLookup(t, BODY_DATABASE, body_key, body_data_result));
  non_empty_branch->PushAction(
    Backend::XdbLookup(t, MODSET_DATABASE, body_key, modset_data_result));

  if (!have_barrier_process)
    non_empty_branch->PushAction(Backend::CounterInc(t, BARRIER_PROCESS));

  TActionTest *empty_branch = new TActionTest(t, key_empty, true);
  t->PushAction(empty_branch);

  TOperand *barrier_process = new TOperandVariable(t, barrier_process_result);
  TOperand *barrier_write = new TOperandVariable(t, barrier_write_result);
  TRANSACTION_MAKE_VAR(process_done);
  TRANSACTION_MAKE_VAR(write_done);
  TRANSACTION_MAKE_VAR(use_sort);
  TRANSACTION_MAKE_VAR(next_list);

  TOperand *zero_val = new TOperandInteger(t, 0);
  TOperand *max_val = new TOperandInteger(t, g_stage_count);

  empty_branch->PushAction(
    Backend::CounterValue(t, BARRIER_PROCESS, barrier_process_result));
  empty_branch->PushAction(
    Backend::CounterValue(t, BARRIER_WRITE, barrier_write_result));
  empty_branch->PushAction(
    Backend::ValueEqual(t, barrier_process, zero_val, process_done_var));
  empty_branch->PushAction(
    Backend::ValueEqual(t, barrier_write, zero_val, write_done_var));

  TActionTest *process_done_branch = new TActionTest(t, process_done, true);
  empty_branch->PushAction(process_done_branch);
  TActionTest *write_done_branch = new TActionTest(t, write_done, true);
  process_done_branch->PushAction(write_done_branch);

  write_done_branch->PushAction(
    Backend::ValueLessEqual(t, stage, max_val, use_sort_var));

  TActionTest *use_sort_branch = new TActionTest(t, use_sort, true);
  use_sort_branch->PushAction(
    Backend::GraphSortKeys(t, CALLGRAPH_NAME, stage, next_list_var));
  write_done_branch->PushAction(use_sort_branch);

  TActionTest *not_sort_branch = new TActionTest(t, use_sort, false);
  not_sort_branch->PushAction(
    Backend::HashAllKeys(t, WORKLIST_FUNC_NEXT, next_list_var));
  not_sort_branch->PushAction(Backend::HashClear(t, WORKLIST_FUNC_NEXT));
  not_sort_branch->PushAction(Backend::BlockFlush(t));
  write_done_branch->PushAction(not_sort_branch);

  TRANSACTION_MAKE_VAR(next_key);

  TActionIterate *next_iterate =
    new TActionIterate(t, next_key_var, next_list);
  next_iterate->PushAction(
    Backend::HashInsertKey(t, WORKLIST_FUNC_HASH, next_key));
  write_done_branch->PushAction(next_iterate);
  write_done_branch->PushAction(Backend::CounterInc(t, COUNTER_STAGE));
  write_done_branch->PushAction(
    Backend::CounterValue(t, COUNTER_STAGE, stage_result));
}

// information about the generated memory and modsets for a function.
struct MemoryKeyData
{
  // new memory to write out.
  Vector<BlockMemory*> block_mems;

  // new modsets to write out.
  Vector<BlockModset*> block_mods;

  // old modsets, if there are any.
  Vector<BlockModset*> old_mods;

  // if this function depends on callee modsets, the names of those callees.
  Vector<Variable*> callees;

  ~MemoryKeyData() {
    DecRefVector<BlockMemory>(block_mems, NULL);
    DecRefVector<BlockModset>(block_mods, NULL);
    DecRefVector<BlockModset>(old_mods, NULL);
    DecRefVector<Variable>(callees, NULL);
  }
};

// get all the direct and indirect callees of this function.
// we need this to do a batch load of all modsets for these callees,
// and to track dependencies introduced by the callee modsets.
void GetCalleeModsets(Transaction *t,
                      const Vector<BlockCFG*> &block_cfgs, size_t stage,
                      MemoryKeyData *data)
{
  Variable *function = block_cfgs[0]->GetId()->BaseVar();

  // process direct calls.
  for (size_t ind = 0; ind < block_cfgs.Size(); ind++) {
    BlockCFG *cfg = block_cfgs[ind];
    for (size_t eind = 0; eind < cfg->GetEdgeCount(); eind++) {
      if (PEdgeCall *edge = cfg->GetEdge(eind)->IfCall()) {
        Variable *callee = edge->GetDirectFunction();
        if (callee && !data->callees.Contains(callee)) {
          callee->IncRef();
          data->callees.PushBack(callee);
        }
      }
    }
  }

  // process indirect calls. how this works depends on the stage.
  if (stage < g_stage_count) {
    // there are no indirect calls in this function.
  }
  else if (stage == g_stage_count) {
    // there may be indirect calls, and this is the first time this function
    // has been processed. compute the targets, and store in the merge
    // list to write out when this stage finishes.
    for (size_t ind = 0; ind < block_cfgs.Size(); ind++)
      CallgraphProcessCFGIndirect(block_cfgs[ind], &data->callees);
  }
  else {
    // we already know the indirect targets of this function, get them
    // from the callgraph.
    CallEdgeSet *cset = CalleeCache.Lookup(function);
    for (size_t ind = 0; cset && ind < cset->GetEdgeCount(); ind++) {
      Variable *callee = cset->GetEdge(ind).callee;
      if (!data->callees.Contains(callee)) {
        callee->IncRef();
        data->callees.PushBack(callee);
      }
    }
    CalleeCache.Release(function);
  }

  // fetch the callee modsets as a single transaction.
  size_t modset_list_result = t->MakeVariable(true);
  TOperand *modset_list_arg = new TOperandVariable(t, modset_list_result);

  TRANSACTION_MAKE_VAR(modset_data);

  Vector<TOperand*> empty_list_args;
  t->PushAction(Backend::ListCreate(t, empty_list_args, modset_list_result));

  for (size_t find = 0; find < data->callees.Size(); find++) {
    Variable *callee = data->callees[find];
    TOperand *callee_arg = new TOperandString(t, callee->GetName()->Value());

    // don't get the modset if it is already cached.
    callee->IncRef();
    BlockId *id = BlockId::Make(B_Function, callee);

    if (!BlockModsetCache.IsMember(id)) {
      t->PushAction(Backend::XdbLookup(t, MODSET_DATABASE,
                                       callee_arg, modset_data_var));
      t->PushAction(Backend::ListPush(t, modset_list_arg, modset_data,
                                      modset_list_result));
    }

    id->DecRef();
  }

  SubmitTransaction(t);

  // add the fetched modsets to the modset cache.
  TOperandList *modset_list = t->LookupList(modset_list_result);
  for (size_t oind = 0; oind < modset_list->GetCount(); oind++) {
    TOperandString *modset_data = modset_list->GetOperand(oind)->AsString();

    Vector<BlockModset*> bmod_list;
    BlockModsetUncompress(t, modset_data, &bmod_list);
    BlockModsetCacheAddList(bmod_list);
  }

  t->Clear();
}

// generate the memory and modset information for the specified CFGs.
// return whether the generation was successful (no timeout).
bool GenerateMemory(const Vector<BlockCFG*> &block_cfgs, size_t stage,
                    MemoryKeyData *data)
{
  Variable *function = block_cfgs[0]->GetId()->BaseVar();
  logout << "Generating memory for [#" << stage << "] "
         << "'" << function->GetName()->Value() << "'" << endl << flush;

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

    bool indirect = (stage > g_stage_count);

    MemoryClobberKind clobber_kind =
      indirect ? MCLB_Modset : MCLB_ModsetNoIndirect;

    id->IncRef();
    BlockMemory *mem =
      BlockMemory::Make(id, MSIMP_Scalar, MALIAS_Buffer, clobber_kind);

    mem->SetCFG(cfg);
    mem->ComputeTables();

    String *loop = id->Loop();

    // make a modset which we will fill in the new modset data from.
    // this uses a temporary ID as we need to distinguish the modsets
    // we are generating during this pass from the modsets we generated
    // during a previous pass.
    function->IncRef();
    BlockKind kind = B_ModsetFunction;
    if (loop) {
      loop->IncRef();
      kind = B_ModsetLoop;
    }
    BlockId *mod_id = BlockId::Make(kind, function, loop);
    BlockModset *mod = BlockModset::Make(mod_id);

    if (!TimerAlarm::ActiveExpired())
      mod->ComputeModset(mem, indirect);

    logout << "Computed modset:" << endl << mod << endl;
    data->block_mods.PushBack(mod);

    // add an entry to the modset cache. we process the function CFGs from
    // innermost loop to the outermost function, and will add loop modsets
    // to the cache as we go.
    id->IncRef(&BlockModsetCache);
    mod->IncRef(&BlockModsetCache);
    BlockModsetCache.Insert(id, mod);

    if (print_memory.IsSpecified()) {
      logout << "Computed memory:" << endl;
      mem->Print(logout);
      logout << endl;
    }

    data->block_mems.PushBack(mem);
    logout << endl;

    if (TimerAlarm::ActiveExpired()) {
      logout << "ERROR: Timeout while generating memory: ";
      PrintTime(TimerAlarm::ActiveElapsed());
      logout << endl;

      had_timeout = true;
      TimerAlarm::ClearActive();
    }
  }

  return !had_timeout;
}

void RunAnalysis(const Vector<const char*> &functions)
{
  static BaseTimer analysis_timer("xmemlocal_main");
  Transaction *t = new Transaction();

  // we will manually manage clearing of entries in the modset cache.
  BlockModsetCache.SetLruEviction(false);

  // load the callgraph sort.
  size_t stage_count_result = t->MakeVariable(true);
  t->PushAction(Backend::GraphLoadSort(t, CALLGRAPH_NAME, stage_count_result));
  SubmitTransaction(t);

  g_stage_count = t->LookupInteger(stage_count_result)->GetValue();
  t->Clear();

  // whether we have a reference on either of the barriers. if there is no
  // pending data then these will be false, otherwise exactly one will be true.
  bool have_barrier_process = false;
  bool have_barrier_write = false;

  // memory/modset data we have generated for the current stage but have
  // not written out yet.
  Vector<MemoryKeyData*> pending_data;

  // current stage being processed.
  size_t current_stage = 0;

  // whether we've processed any functions in the current stage.
  bool current_stage_processed = false;

  while (true) {
    Timer _timer(&analysis_timer);

    size_t stage_result = t->MakeVariable(true);
    size_t body_data_result = t->MakeVariable(true);
    size_t modset_data_result = t->MakeVariable(true);
    size_t barrier_process_result = t->MakeVariable(true);
    size_t barrier_write_result = t->MakeVariable(true);

    MakeFetchTransaction(t, have_barrier_process, stage_result,
                         body_data_result, modset_data_result,
                         barrier_process_result, barrier_write_result);
    SubmitTransaction(t);

    size_t new_stage = t->LookupInteger(stage_result)->GetValue() - 1;
    Assert(new_stage != (size_t) -1);

    if (!pending_data.Empty()) {
      // the stage should not have been advanced if we have pending data.
      Assert(new_stage == current_stage);
    }

    if (new_stage > current_stage) {
      if (pass_limit.UIntValue() > 0) {
        if (new_stage >= g_stage_count + pass_limit.UIntValue())
          break;
      }

      // if we never processed anything from the current one, either the
      // worklist has been drained or has become so small there's no useful
      // work anymore.
      if (new_stage > g_stage_count && !current_stage_processed)
        break;

      current_stage = new_stage;
      current_stage_processed = false;
    }

    if (!t->Lookup(body_data_result, false)) {
      // there are no more functions in this stage.
      size_t barrier_process =
        t->LookupInteger(barrier_process_result)->GetValue();
      size_t barrier_write =
        t->LookupInteger(barrier_write_result)->GetValue();
      t->Clear();

      if (have_barrier_process) {
        // we're done with processing functions from this stage.
        Assert(barrier_process > 0);

        t->PushAction(Backend::CounterDec(t, BARRIER_PROCESS));
        t->PushAction(Backend::CounterInc(t, BARRIER_WRITE));
        SubmitTransaction(t);
        t->Clear();

        have_barrier_process = false;
        have_barrier_write = true;

        // drop any modsets we have cached, these may change after
        // we start the next stage.
        BlockModsetCache.Clear();
        continue;
      }

      if (have_barrier_write && barrier_process == 0) {
        // everyone is done processing functions from this stage,
        // write out our results.
        Assert(barrier_write > 0);

        // amount of data we've added to the transaction.
        size_t data_written = 0;

        for (size_t ind = 0; ind < pending_data.Size(); ind++) {
          MemoryKeyData *data = pending_data[ind];
          String *function = data->block_mems[0]->GetId()->Function();
          size_t function_len = strlen(function->Value()) + 1;

          Buffer *buf = new Buffer(200);
          t->AddBuffer(buf);
          buf->Append(function->Value(), function_len);
          TOperandString *body_key =
            new TOperandString(t, buf->base, function_len);

          TOperandString *memory_data_arg =
            BlockMemoryCompress(t, data->block_mems);
          TOperandString *modset_data_arg =
            BlockModsetCompress(t, data->block_mods);

          data_written += memory_data_arg->GetDataLength();
          data_written += modset_data_arg->GetDataLength();

          t->PushAction(Backend::XdbReplace(t, MEMORY_DATABASE,
                                            body_key, memory_data_arg));
          t->PushAction(Backend::XdbReplace(t, MODSET_DATABASE,
                                            body_key, modset_data_arg));

          if (current_stage == g_stage_count) {
            // write out all dependencies this function has on callee modsets.
            for (size_t ind = 0; ind < data->callees.Size(); ind++) {
              String *callee = data->callees[ind]->GetName();
              size_t callee_len = strlen(callee->Value()) + 1;

              Buffer *buf = new Buffer(200);
              t->AddBuffer(buf);
              buf->Append(callee->Value(), callee_len);
              TOperandString *callee_key =
                new TOperandString(t, buf->base, callee_len);

              t->PushAction(Backend::HashInsertValue(t, MODSET_DEPENDENCY_HASH,
                                                     callee_key, body_key));
            }

            // add this function to the next worklist.
            t->PushAction(
              Backend::HashInsertKey(t, WORKLIST_FUNC_NEXT, body_key));
          }
          else if (current_stage > g_stage_count) {
            // check if our computed modset for the outer function differs
            // from the old modset.
            Assert(!data->block_mods.Empty() && !data->old_mods.Empty());

            BlockModset *new_mod = data->block_mods.Back();
            BlockModset *old_mod = data->old_mods.Back();

            Assert(new_mod->GetId()->Kind() == B_ModsetFunction);
            Assert(old_mod->GetId()->Kind() == B_Function);
            Assert(new_mod->GetId()->BaseVar() == old_mod->GetId()->BaseVar());

            if (!new_mod->Equivalent(old_mod)) {
              // add all the callers of this function to the next worklist.
              TRANSACTION_MAKE_VAR(caller_list);
              TRANSACTION_MAKE_VAR(caller_key);

              t->PushAction(Backend::HashLookup(t, MODSET_DEPENDENCY_HASH,
                                                body_key, caller_list_var));

              TActionIterate *caller_iter =
                new TActionIterate(t, caller_key_var, caller_list);
              caller_iter->PushAction(
                Backend::HashInsertKey(t, WORKLIST_FUNC_NEXT, caller_key));
              t->PushAction(caller_iter);
            }
          }

          if (data_written > TRANSACTION_DATA_LIMIT) {
            SubmitTransaction(t);
            t->Clear();
            data_written = 0;
          }

          delete data;
        }

        // write out any indirect callgraph edges we generated.
        WritePendingEscape();

        t->PushAction(Backend::CounterDec(t, BARRIER_WRITE));
        SubmitTransaction(t);
        t->Clear();

        pending_data.Clear();
        have_barrier_write = false;
        continue;
      }

      // either the current stage was empty and there was no processing
      // we could do, or we're waiting on some other worker to finish
      // processing functions in this stage or finish writing out its results.
      t->Clear();
      if (barrier_process > 0 || barrier_write > 0)
        sleep(1);
      continue;
    }

    have_barrier_process = true;
    current_stage_processed = true;

    Vector<BlockCFG*> block_cfgs;
    BlockCFGUncompress(t, body_data_result, &block_cfgs);
    Assert(!block_cfgs.Empty());

    TOperandString *modset_data = t->LookupString(modset_data_result);
    MemoryKeyData *data = new MemoryKeyData();
    BlockModsetUncompress(t, modset_data, &data->old_mods);

    // done with the transaction's returned data.
    t->Clear();

    GetCalleeModsets(t, block_cfgs, current_stage, data);
    bool success = GenerateMemory(block_cfgs, current_stage, data);

    if (success) {
      // remember this memory/modset data to write out later.
      pending_data.PushBack(data);
    }
    else {
      // had a timeout while generating the memory.
      // discard this incomplete data.
      delete data;
    }

    DecRefVector<BlockCFG>(block_cfgs, NULL);
  }

  t->Clear();

  // compute memory for global variables.

  t->PushAction(
    Backend::Compound::HashCreateXdbKeys(t, WORKLIST_GLOB_HASH, INIT_DATABASE));
  SubmitTransaction(t);
  t->Clear();

  while (true) {
    Timer _timer(&analysis_timer);

    size_t init_key_result = t->MakeVariable(true);
    size_t init_data_result = t->MakeVariable(true);

    t->PushAction(
      Backend::Compound::HashPopXdbKey(
        t, WORKLIST_GLOB_HASH, INIT_DATABASE,
        init_key_result, init_data_result));
    SubmitTransaction(t);

    TOperandString *init_key = t->LookupString(init_key_result);

    if (init_key->GetDataLength() == 1) {
      // done with all globals.
      t->Clear();
      break;
    }

    Vector<BlockCFG*> block_cfgs;
    BlockCFGUncompress(t, init_data_result, &block_cfgs);

    t->Clear();

    Vector<BlockMemory*> block_mems;

    for (size_t ind = 0; ind < block_cfgs.Size(); ind++) {
      if (uint32_t timeout = GetTimeout())
        TimerAlarm::StartActive(timeout);

      BlockCFG *cfg = block_cfgs[ind];
      BlockId *id = cfg->GetId();

      if (print_cfgs.IsSpecified())
        logout << cfg << endl;

      id->IncRef();
      BlockMemory *mem =
        BlockMemory::Make(id, MSIMP_Scalar, MALIAS_Buffer, MCLB_Modset);

      mem->SetCFG(cfg);
      mem->ComputeTables();

      block_mems.PushBack(mem);
    }

    String *function = block_cfgs[0]->GetId()->Function();

    init_key = new TOperandString(t, function->Value());
    TOperandString *memory_data_arg = BlockMemoryCompress(t, block_mems);

    t->PushAction(Backend::XdbReplace(t, MEMORY_DATABASE,
                                      init_key, memory_data_arg));
    SubmitTransaction(t);
    t->Clear();

    DecRefVector<BlockCFG>(block_cfgs, NULL);
    DecRefVector<BlockMemory>(block_mems, NULL);
  }

  delete t;
}

int main(int argc, const char **argv)
{
  timeout.Enable();
  trans_remote.Enable();
  trans_initial.Enable();

  print_cfgs.Enable();
  print_memory.Enable();
  print_indirect_calls.Enable();
  pass_limit.Enable();

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
