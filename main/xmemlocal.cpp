
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

ConfigOption pass_limit(CK_UInt, "pass-limit", "0",
                        "maximum number of passes to perform, 0 for no limit");

ConfigOption print_cfgs(CK_Flag, "print-cfgs", NULL,
                        "print input CFGs");

ConfigOption print_memory(CK_Flag, "print-memory", NULL,
                          "print generated memory information");

// number of callgraph stages.
static size_t g_stage_count = 0;

// number of stages we will actually analyze, zero for no limit (fixpointing).
static size_t g_stage_limit = 0;

// perform an initialization transaction to setup the callgraph/worklist.
void DoInitTransaction(Transaction *t, const Vector<const char*> &functions)
{
  // either load the worklist from file or seed it with the functions
  // we got from the command line.

  size_t count_var = t->MakeVariable(true);

  if (!functions.Empty()) {
    TOperandList *new_functions = new TOperandList(t);
    for (size_t ind = 0; ind < functions.Size(); ind++)
      new_functions->PushOperand(new TOperandString(t, functions[ind]));

    t->PushAction(Backend::BlockSeedWorklist(t, new_functions));

    // don't fixpoint analysis if we are running on command line functions.
    g_stage_limit = 1;
  }
  else {
    t->PushAction(Backend::BlockLoadWorklist(t, count_var));
  }

  SubmitTransaction(t);

  if (functions.Empty()) {
    // get the stage count and set the pass limit.
    g_stage_count = t->LookupInteger(count_var)->GetValue();

    if (pass_limit.UIntValue() != 0)
      g_stage_limit = g_stage_count + pass_limit.UIntValue();
  }
}

// perform a transaction to get the next key from the worklist. the body data
// will not be set if there are no nodes remaining in the worklist.
// have_process indicates whether we have a count on the process barrier,
// process_result and write_result receives whether any worker has a count
// on those barriers.
void DoFetchTransaction(Transaction *t, bool have_process,
                        size_t stage_result,
                        size_t body_data_result, size_t modset_data_result,
                        size_t process_result, size_t write_result)
{
  TRANSACTION_MAKE_VAR(body_key);
  TRANSACTION_MAKE_VAR(key_empty);

  t->PushAction(Backend::BlockCurrentStage(t, stage_result));
  t->PushAction(Backend::BlockPopWorklist(t, !have_process, body_key_var));
  t->PushAction(Backend::BlockHaveBarrierProcess(t, process_result));
  t->PushAction(Backend::BlockHaveBarrierWrite(t, write_result));
  t->PushAction(Backend::StringIsEmpty(t, body_key, key_empty_var));

  TActionTest *non_empty_branch = new TActionTest(t, key_empty, false);
  t->PushAction(non_empty_branch);

  non_empty_branch->PushAction(
    Backend::XdbLookup(t, BODY_DATABASE, body_key, body_data_result));
  non_empty_branch->PushAction(
    Backend::XdbLookup(t, MODSET_DATABASE, body_key, modset_data_result));

  SubmitTransaction(t);
}

// information about the generated modsets for a function.
struct MemoryKeyData
{
  // new modsets to write out.
  Vector<BlockModset*> block_mods;

  // whether the modset for the function itself has changed. modsets for inner
  // loops may have changed, so the modset needs to be written.
  bool mod_changed;

  // if this function depends on callee modsets, the names of those callees.
  Vector<Variable*> callees;

  MemoryKeyData() : mod_changed(false) {}

  ~MemoryKeyData() {
    DecRefVector<BlockModset>(block_mods, NULL);
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
                    Vector<BlockMemory*> *block_mems, MemoryKeyData *data)
{
  Variable *function = block_cfgs[0]->GetId()->BaseVar();
  logout << "Generating memory [#" << stage << "] "
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
    // this uses a cloned ID as we need to distinguish the modsets
    // we are generating during this pass from the modsets we generated
    // during a previous pass.
    function->IncRef();
    if (loop) loop->IncRef();
    BlockId *mod_id = BlockId::Make(id->Kind(), function, loop, true);
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

    block_mems->PushBack(mem);
    logout << endl;

    if (TimerAlarm::ActiveExpired()) {
      logout << "ERROR: Timeout while generating memory: ";
      PrintTime(TimerAlarm::ActiveElapsed());
      logout << endl;

      had_timeout = true;
    }

    TimerAlarm::ClearActive();
  }

  return !had_timeout;
}

// how often to print allocation/timer information.
#define PRINT_FREQUENCY 50
size_t g_print_counter = 0;

void RunAnalysis(const Vector<const char*> &functions)
{
  static BaseTimer analysis_timer("xmemlocal_main");
  Transaction *t = new Transaction();

  // we will manually manage clearing of entries in the modset cache.
  BlockModsetCache.SetLruEviction(false);

  // setup the callgraph sort or worklist seed.
  DoInitTransaction(t, functions);
  t->Clear();

  // whether we have a reference on either of the barriers. if there is no
  // pending data then these will be false, otherwise exactly one will be true.
  bool have_process = false;
  bool have_write = false;

  // memory/modset data we have generated for the current stage but have
  // not written out yet.
  Vector<MemoryKeyData*> pending_data;

  // current stage being processed.
  size_t current_stage = 0;

  // whether we've processed any functions in the current stage.
  bool current_stage_processed = false;

  while (true) {
    Timer _timer(&analysis_timer);

    g_print_counter++;

    if (g_print_counter % PRINT_FREQUENCY == 0) {
      PrintTimers();
      PrintAllocs();
    }

    size_t stage_result = t->MakeVariable(true);
    size_t body_data_result = t->MakeVariable(true);
    size_t modset_data_result = t->MakeVariable(true);
    size_t process_result = t->MakeVariable(true);
    size_t write_result = t->MakeVariable(true);

    DoFetchTransaction(t, have_process, stage_result,
                       body_data_result, modset_data_result,
                       process_result, write_result);

    size_t new_stage = t->LookupInteger(stage_result)->GetValue();

    if (new_stage > current_stage) {
      Assert(!have_process);
      Assert(!have_write);

      if (g_stage_limit > 0) {
        if (new_stage >= g_stage_limit) {
          cout << "Finished functions [#" << new_stage
               << "]: hit pass limit" << endl;
          break;
        }
      }

      // if we never processed anything from the old stage (and didn't
      // get an item for the new stage), either the worklist has been drained
      // or has become so small there's not enough work for this core.
      if (new_stage > g_stage_count && !current_stage_processed &&
          !t->Lookup(body_data_result, false)) {
        cout << "Finished functions [#" << new_stage
             << "]: exhausted worklist" << endl;
        break;
      }

      if (IsAnalysisRemote())
        cout << "New stage [#" << new_stage << "]" << endl;

      current_stage = new_stage;
      current_stage_processed = false;
    }

    if (!t->Lookup(body_data_result, false)) {
      // there are no more functions in this stage.
      bool set_barrier_process = t->LookupBoolean(process_result)->IsTrue();
      bool set_barrier_write = t->LookupBoolean(write_result)->IsTrue();
      t->Clear();

      if (have_process) {
        // we're done with processing functions from this stage.
        // shift our process count to a write count.
        Assert(set_barrier_process);

        t->PushAction(Backend::BlockShiftBarrierProcess(t));
        SubmitTransaction(t);
        t->Clear();

        have_process = false;
        have_write = true;

        // drop any modsets we have cached, these may change after
        // we start the next stage.
        BlockModsetCache.Clear();

        if (IsAnalysisRemote())
          cout << "Finished processing stage #" << current_stage << endl;
        continue;
      }

      if (have_write && !set_barrier_process) {
        // everyone is done processing functions from this stage,
        // write out our results.

        // amount of data we've added to the transaction.
        size_t data_written = 0;

        for (size_t ind = 0; ind < pending_data.Size(); ind++) {
          MemoryKeyData *data = pending_data[ind];
          String *function = data->block_mods[0]->GetId()->Function();
          size_t function_len = strlen(function->Value()) + 1;

          Buffer *buf = new Buffer(200);
          t->AddBuffer(buf);
          buf->Append(function->Value(), function_len);
          TOperandString *body_key =
            new TOperandString(t, buf->base, function_len);

          TOperandString *modset_data_arg =
            BlockModsetCompress(t, data->block_mods);

          data_written += modset_data_arg->GetDataLength();
          t->PushAction(Backend::XdbReplace(t, MODSET_DATABASE,
                                            body_key, modset_data_arg));

          if (data->mod_changed) {
            Assert(current_stage > g_stage_count);

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

            cout << "ModsetChanged [#" << current_stage << "]: "
                 << function->Value() << endl;
          }
          else if (current_stage == g_stage_count) {
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

          if (data_written > TRANSACTION_DATA_LIMIT) {
            SubmitTransaction(t);
            t->Clear();
            data_written = 0;
          }

          delete data;
        }

        // write out any indirect callgraph edges we generated.
        WritePendingEscape();

        t->PushAction(Backend::BlockDropBarrierWrite(t));
        SubmitTransaction(t);
        t->Clear();

        pending_data.Clear();
        have_write = false;

        if (IsAnalysisRemote())
          cout << "Finished writing stage #" << current_stage << endl;
        continue;
      }

      // either the current stage was empty and there was no processing
      // we could do, or we're waiting on some other worker to finish
      // processing functions in this stage or finish writing out its results.
      t->Clear();
      if (set_barrier_process || set_barrier_write)
        sleep(1);
      continue;
    }

    // we have a function to process.

    have_process = true;
    current_stage_processed = true;

    Vector<BlockCFG*> block_cfgs;
    BlockCFGUncompress(t, body_data_result, &block_cfgs);
    Assert(!block_cfgs.Empty());

    Vector<BlockModset*> old_mods;
    TOperandString *modset_data = t->LookupString(modset_data_result);
    BlockModsetUncompress(t, modset_data, &old_mods);

    // done with the transaction's returned data.
    t->Clear();

    // data to store the modset results.
    MemoryKeyData *data = new MemoryKeyData();

    Vector<BlockMemory*> block_mems;

    GetCalleeModsets(t, block_cfgs, current_stage, data);
    bool success = GenerateMemory(block_cfgs, current_stage,
                                  &block_mems, data);

    if (success) {
      // remember this memory/modset data to write out later.
      pending_data.PushBack(data);

      // write out the generated memory. other functions do not use this
      // and a lot of temporary data is generated which we want to get rid of.
      String *function = block_cfgs[0]->GetId()->Function();

      TOperand *body_key = new TOperandString(t, function->Value());
      TOperandString *memory_data_arg = BlockMemoryCompress(t, block_mems);

      t->PushAction(Backend::XdbReplace(t, MEMORY_DATABASE,
                                        body_key, memory_data_arg));
      SubmitTransaction(t);
      t->Clear();

      if (current_stage > g_stage_count) {
        // check if our computed modset for the outer function has changed.
        Assert(!data->block_mods.Empty() && !old_mods.Empty());

        BlockModset *new_mod = data->block_mods.Back();
        BlockModset *old_mod = old_mods.Back();

        Assert(new_mod->GetId()->IsClone());
        Assert(new_mod->GetId()->Kind() == B_Function);
        Assert(old_mod->GetId()->Kind() == B_Function);
        Assert(new_mod->GetId()->BaseVar() == old_mod->GetId()->BaseVar());

        if (new_mod->MergeModset(old_mod))
          data->mod_changed = true;
      }
    }
    else {
      // had a timeout while generating the memory.
      // discard this incomplete data.
      delete data;
    }

    DecRefVector<BlockCFG>(block_cfgs, NULL);
    DecRefVector<BlockMemory>(block_mems, NULL);
    DecRefVector<BlockModset>(old_mods, NULL);
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

    String *global = block_cfgs[0]->GetId()->Function();
    logout << "Generating initializer memory "
           << "'" << global->Value() << "'" << endl << flush;

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
      TimerAlarm::ClearActive();
    }

    init_key = new TOperandString(t, global->Value());
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
