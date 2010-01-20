
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
#include <imlang/storage.h>
#include <memory/storage.h>
#include <infer/infer.h>
#include <infer/invariant.h>
#include <util/config.h>
#include <solve/solver.h>

NAMESPACE_XGILL_USING

const char *USAGE = "xinfer [options] [function*]";

void MakeInitTransaction(Transaction *t, const Vector<const char*> &functions)
{
  // clear the output database if we are processing all functions.
  if (functions.Empty()) {
    t->PushAction(
      Backend::Compound::XdbClearIfNotHash(
        t, SUMMARY_DATABASE, WORKLIST_FUNC_HASH));
  }

  if (functions.Size()) {
    // insert the initial function/global names.
    for (size_t uind = 0; uind < functions.Size(); uind++) {
      TOperand *key = new TOperandString(t, functions[uind]);
      t->PushAction(Backend::HashInsertKey(t, WORKLIST_FUNC_HASH, key));
    }
  }
  else {
    // fill in the worklist hashes from the database keys.

    t->PushAction(
      Backend::Compound::HashCreateXdbKeys(
        t, WORKLIST_FUNC_HASH, BODY_DATABASE));

    // load the initial worklist sort information.
    t->PushAction(
      Backend::GraphLoadSort(t, WORKLIST_FUNC_SORT, BODY_SORT_FILE));
  }
}

void MakeFetchTransaction(Transaction *t,
                          size_t body_key_result, size_t body_data_result,
                          size_t memory_data_result,
                          size_t modset_data_result)
{
  t->PushAction(
    Backend::Compound::HashPopXdbKeyWithSort(
      t, WORKLIST_FUNC_SORT, WORKLIST_FUNC_HASH, BODY_DATABASE,
      body_key_result, body_data_result));

  TOperand *body_key_arg = new TOperandVariable(t, body_key_result);

  t->PushAction(
    Backend::XdbLookup(
      t, MEMORY_DATABASE, body_key_arg, memory_data_result));
  t->PushAction(
    Backend::XdbLookup(
      t, MODSET_DATABASE, body_key_arg, modset_data_result));
}

void MakeUpdateTransaction(Transaction *t, Buffer *function_name,
                           const Vector<BlockSummary*> &function_sums)
{
  TOperand *function_arg =
    new TOperandString(t, (const char*) function_name->base);

  TOperand *summary_data_arg = BlockSummaryCompress(t, function_sums);
  t->PushAction(Backend::XdbReplace(t, SUMMARY_DATABASE,
                                      function_arg, summary_data_arg));
}

ConfigOption print_cfgs(CK_Flag, "print-cfgs", NULL,
                        "print input CFGs");

ConfigOption print_memory(CK_Flag, "print-memory", NULL,
                          "print input memory information");

// how often to print allocation/timer information.
#define PRINT_FREQUENCY 50
size_t g_print_counter = 0;

void RunAnalysis(const Vector<const char*> &functions)
{
  static BaseTimer analysis_timer("xinfer_main");
  Transaction *t = new Transaction();

  // we will manually manage clearing of entries in the summary cache.
  BlockSummaryCache.SetLruEviction(false);

  // construct and submit the worklist create transaction
  MakeInitTransaction(t, functions);
  SubmitTransaction(t);
  t->Clear();

  while (true) {
    Timer _timer(&analysis_timer);
    ResetTimeout();

    g_print_counter++;

    if (g_print_counter % PRINT_FREQUENCY == 0) {
      PrintTimers();
      PrintAllocs();
    }

    // construct and submit a worklist fetch transaction.
    size_t body_key_result = t->MakeVariable(true);
    size_t body_data_result = t->MakeVariable(true);
    size_t memory_data_result = t->MakeVariable(true);
    size_t modset_data_result = t->MakeVariable(true);
    MakeFetchTransaction(t, body_key_result, body_data_result,
                         memory_data_result, modset_data_result);
    SubmitTransaction(t);

    TOperandString *body_key = t->LookupString(body_key_result);

    // make a copy of the function name.
    Buffer key_buf;

    Vector<BlockCFG*> block_cfgs;

    if (body_key->GetDataLength() > 1) {
      key_buf.Append(body_key->GetData(), body_key->GetDataLength());
      BlockCFGUncompress(t, body_data_result, &block_cfgs);
    }
    else {
      // done with all functions.
      t->Clear();
      break;
    }

    Vector<BlockMemory*> block_mems;
    BlockMemoryUncompress(t, memory_data_result, &block_mems);
    BlockMemoryCacheAddList(block_mems);

    Vector<BlockModset*> block_mods;
    TOperandString *modset_op = t->LookupString(modset_data_result);
    BlockModsetUncompress(t, modset_op, &block_mods);
    BlockModsetCacheAddList(block_mods);

    t->Clear();

    // generate summaries.

    logout << "Generating summaries for "
           << "\'" << (const char*) key_buf.base << "\'" << endl << flush;

    // clear out existing summaries from the cache. TODO: we should also
    // load all the callee summaries here, and add dependencies on them.
    BlockSummaryCache.Clear();

    // generate summaries for each of the CFGs.
    Vector<BlockSummary*> block_sums;

    for (size_t find = 0; find < block_cfgs.Size(); find++) {
      BlockCFG *cfg = block_cfgs[find];
      BlockId *id = cfg->GetId();

      BlockMemory *mcfg = GetBlockMemory(id);

      if (mcfg == NULL) {
        logout << "WARNING: Missing memory: " << id << endl;
        continue;
      }

      if (print_cfgs.IsSpecified())
        logout << cfg << endl;

      if (print_memory.IsSpecified())
        logout << mcfg << endl;

      id->IncRef();
      BlockSummary *sum = BlockSummary::Make(id);

      sum->SetMemory(mcfg);
      mcfg->DecRef();

      block_sums.PushBack(sum);
    }

    // make sure the cache knows about these summaries.
    BlockSummaryCacheAddList(block_sums, false);

    InferSummaries(block_sums);

    // print the summaries to screen.
    for (size_t find = 0; find < block_sums.Size(); find++) {
      BlockSummary *sum = block_sums[find];
        logout << "Computed summary:" << endl << sum << endl;
    }

    logout << "Elapsed: ";
    PrintTime(_timer.Elapsed());
    logout << endl << endl;

    MakeUpdateTransaction(t, &key_buf, block_sums);
    SubmitTransaction(t);
    t->Clear();

    // clear out held references on CFGs and summaries.
    for (size_t find = 0; find < block_cfgs.Size(); find++)
      block_cfgs[find]->DecRef();
    for (size_t find = 0; find < block_sums.Size(); find++)
      block_sums[find]->DecRef();
  }

  delete t;
}

int main(int argc, const char **argv)
{
  timeout.Enable();
  trans_remote.Enable();
  trans_initial.Enable();

  solver_use.Enable();
  solver_verbose.Enable();
  solver_constraint.Enable();

  print_invariants.Enable();
  print_cfgs.Enable();
  print_memory.Enable();

  Vector<const char*> functions;
  bool parsed = Config::Parse(argc, argv, &functions);
  if (!parsed) {
    Config::PrintUsage(USAGE);
    return 1;
  }

  // Solver::CheckSimplifications();

  ResetAllocs();
  AnalysisPrepare();

  RunAnalysis(functions);
  SubmitFinalTransaction();

  ClearBlockCaches();
  ClearMemoryCaches();
  AnalysisFinish(0);
}
