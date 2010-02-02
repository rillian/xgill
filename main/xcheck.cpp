
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
#include <check/checker.h>
#include <check/sufficient.h>
#include <util/config.h>

NAMESPACE_XGILL_USING

const char *USAGE = "xcheck [options] [function-check*]";

ConfigOption check_kind(CK_String, "check-kind", "write_overflow",
                        "assert kind to analyze");

ConfigOption check_file(CK_String, "check-file", "",
                        "file with list of checks to analyze");

ConfigOption xml_file(CK_String, "xml-out", "",
                      "file to receive XML report for single check");

ConfigOption append_reports(CK_Flag, "append", NULL,
                            "append reports to any existing database");

// database receiving display XML for unverified assertions.
const char *report_database = NULL;

void DoInitTransaction(Transaction *t, const Vector<const char*> &checks)
{
  // don't use the worklist hash at all if we are generating a single
  // XML file. we'll just run on the single check directly.
  if (xml_file.IsSpecified())
    return;

  // clear the output database unless we're appending to it.
  if (!append_reports.IsSpecified()) {
    t->PushAction(
      Backend::Compound::XdbClearIfNotHash(
          t, report_database, WORKLIST_FUNC_HASH));
  }

  // setup the analysis worklist hash.
  if (checks.Size()) {
    // just insert the specified functions into the worklist.
    // test for existence of the worklist hash so that if we crash and
    // restart we don't reinsert all the entries.

    size_t existvar = t->MakeVariable();
    TOperand *existarg = new TOperandVariable(t, existvar);

    TActionTest *nex_test = new TActionTest(t, existarg, false);
    t->PushAction(Backend::HashExists(t, WORKLIST_FUNC_HASH, existvar));
    t->PushAction(nex_test);

    for (size_t uind = 0; uind < checks.Size(); uind++) {
      const char *check = checks[uind];

      Buffer *buf = new Buffer();
      t->AddBuffer(buf);

      if (!BlockSummary::GetAssertFunction(checks[uind], buf)) {
        cout << "ERROR: Malformed check name: " << check << endl;
        continue;
      }

      TOperand *key = new TOperandString(t, (const char*) buf->base);
      nex_test->PushAction(
        Backend::HashInsertKey(t, WORKLIST_FUNC_HASH, key));
    }
  }
  else {
    // fill in the worklist hash from the body database.
    t->PushAction(
      Backend::Compound::HashCreateXdbKeys(
        t, WORKLIST_FUNC_HASH, BODY_DATABASE));
  }

  SubmitTransaction(t);
  t->Clear();
}

void MakeFetchTransaction(Transaction *t, const Vector<const char*> &checks,
                          size_t body_key_result, size_t body_data_result,
                          size_t memory_data_result,
                          size_t modset_data_result,
                          size_t summary_data_result)
{
  TOperand *body_key_arg = new TOperandVariable(t, body_key_result);

  if (xml_file.IsSpecified()) {
    // get the single check we will be analyzing.
    Assert(checks.Size() == 1);

    Buffer *buf = new Buffer();
    t->AddBuffer(buf);

    Try(BlockSummary::GetAssertFunction(checks[0], buf));
    TOperand *key = new TOperandString(t, (const char*) buf->base);
    t->PushAction(new TActionAssign(t, key, body_key_result));
    t->PushAction(
      Backend::XdbLookup(t, BODY_DATABASE, body_key_arg, body_data_result));
  }
  else {
    // pull a new function off the worklist.
    t->PushAction(
      Backend::Compound::HashPopXdbKey(
        t, WORKLIST_FUNC_HASH, BODY_DATABASE,
        body_key_result, body_data_result));
  }

  t->PushAction(
    Backend::XdbLookup(
      t, MEMORY_DATABASE, body_key_arg, memory_data_result));
  t->PushAction(
    Backend::XdbLookup(
      t, MODSET_DATABASE, body_key_arg, modset_data_result));
  t->PushAction(
    Backend::XdbLookup(
      t, SUMMARY_DATABASE, body_key_arg, summary_data_result));
}

void StoreDisplayPath(DisplayPath *path, const char *name, BlockId *id)
{
  static Buffer xml_buf("Buffer_xcheck_xml");
  path->WriteXML(&xml_buf);

  if (xml_file.IsSpecified()) {
    FileOutStream file_out(xml_file.StringValue());
    file_out.Put(xml_buf.base, xml_buf.pos - xml_buf.base);
  }
  else {
    static Buffer key_buf("Buffer_xcheck_key");
    static Buffer compress_buf("Buffer_xcheck_compress");

    // get the compressed XML for the database entry.
    CompressBufferInUse(&xml_buf, &compress_buf);

    Transaction *t = new Transaction();

    TOperand *key_arg = new TOperandString(t, name);
    TOperand *data_arg =
      new TOperandString(t, compress_buf.base,
                         compress_buf.pos - compress_buf.base);
    t->PushAction(Backend::XdbReplace(t, report_database, key_arg, data_arg));

    SubmitTransaction(t);
    delete t;

    key_buf.Reset();
    compress_buf.Reset();
  }

  xml_buf.Reset();
}

void RunAnalysis(const Vector<const char*> &checks)
{
  static BaseTimer analysis_timer("xcheck_main");
  Transaction *t = new Transaction();

  // construct and submit the worklist create transaction.
  DoInitTransaction(t, checks);

  bool first = true;

  while (true) {
#ifndef DEBUG
    ResetTimeout(40);
#endif

    Timer _timer(&analysis_timer);

    // only go around the loop once if we are generating an XML file.
    // for XML output we don't use the worklist but run on a single assert.
    if (!first) {
      if (xml_file.IsSpecified())
        break;
    }
    first = false;

    // construct and submit a worklist fetch transaction.
    size_t body_key_result = t->MakeVariable(true);
    size_t body_data_result = t->MakeVariable(true);
    size_t memory_data_result = t->MakeVariable(true);
    size_t modset_data_result = t->MakeVariable(true);
    size_t summary_data_result = t->MakeVariable(true);
    MakeFetchTransaction(t, checks, body_key_result, body_data_result,
                         memory_data_result, modset_data_result,
                         summary_data_result);
    SubmitTransaction(t);

    // make sure the key is NULL terminated.
    TOperandString *body_key = t->LookupString(body_key_result);
    logout << endl;

    Assert(IsCStringOperand(body_key));

    if (body_key->GetDataLength() == 1) {
      // key will be the empty string when the hashes are empty and there
      // is nothing else to process.
      break;
    }

    Vector<BlockCFG*> function_cfgs;
    BlockCFGUncompress(t, body_data_result, &function_cfgs);

    if (function_cfgs.Empty()) {
      // logout << "WARNING: Missing CFG: "
      //        << (const char*) body_key->GetData() << endl;
      t->Clear();
      continue;
    }

    // this call does not consume the references on function_cfgs.
    BlockCFGCacheAddListWithRefs(function_cfgs);

    Vector<BlockMemory*> function_mems;
    BlockMemoryUncompress(t, memory_data_result, &function_mems);
    BlockMemoryCacheAddList(function_mems);

    Vector<BlockModset*> function_mods;
    TOperandString *modset_op = t->LookupString(modset_data_result);
    BlockModsetUncompress(t, modset_op, &function_mods);
    BlockModsetCacheAddList(function_mods);

    Vector<BlockSummary*> function_sums;
    TOperandString *summary_op = t->LookupString(summary_data_result);
    BlockSummaryUncompress(t, summary_op, &function_sums);
    BlockSummaryCacheAddList(function_sums);

    // make a copy of the function name.
    size_t body_key_len = body_key->GetDataLength();
    Buffer body_key_buf(body_key_len);
    memcpy(body_key_buf.base, body_key->GetData(), body_key_len);

    t->Clear();

    logout << "Checking: '"
           << (const char*) body_key_buf.base << "'" << endl << endl << flush;

    size_t assertion_count = 0;
    size_t redundant_count = 0;
    size_t success_count = 0;
    size_t report_count = 0;

    for (size_t cind = 0; cind < function_cfgs.Size(); cind++) {
      BlockCFG *cfg = function_cfgs[cind];
      BlockId *id = cfg->GetId();

      BlockMemory *mcfg = BlockMemoryCache.Lookup(id);
      BlockSummary *sum = BlockSummaryCache.Lookup(id);

      Assert(sum);
      sum->ComputeAssertNames();

      const Vector<AssertInfo> *asserts = sum->GetAsserts();
      size_t assert_count = VectorSize<AssertInfo>(asserts);

      if (mcfg) {
        mcfg->SetCFG(cfg);
      }
      else {
        // this should be an empty summary since we failed to generate memory
        // for the block.
        Assert(assert_count == 0);
        logout << "WARNING: Missing memory: " << id << endl;
      }

      for (size_t ind = 0; ind < assert_count; ind++) {
        const AssertInfo &info = asserts->At(ind);

        // only look at assertions with a kind we're interested in,
        // unless we have an explicit list of checks (in which case we'll
        // look at all those specified).
        if (checks.Empty()) {
          if (strcmp(AssertKindString(info.kind), check_kind.StringValue()))
            continue;
        }

        // ignore trivial and redundant assertions.
        if (info.cls != ASC_Check) {
          redundant_count++;
          continue;
        }

        assertion_count++;

        Assert(info.name_buf);
        const char *name = (const char*) info.name_buf->base;

        if (checks.Size()) {
          // the command line assertions are a filter on the checks we do.
          bool checks_match = false;

          for (size_t ind = 0; ind < checks.Size(); ind++) {
            if (!strcmp(checks[ind], name)) {
              checks_match = true;
              break;
            }
          }

          if (!checks_match)
            continue;
        }

        // reset the hard timeout at each new assertion. we want to avoid hard
        // failures as much as possible; this can make functions take a very
        // long time to analyze in the worst case. hard timeouts are disabled
        // if we're debugging.
#ifndef DEBUG
        ResetTimeout(40);
#endif

        // set a soft timeout for the checker/solver.
        if (uint32_t timeout = GetTimeout())
          TimerAlarm::StartActive(timeout);

        logout << "ASSERTION '" << name << "'" << endl;
        logout << "Point " << info.point << ": " << info.bit << endl;

        CheckerState *state = CheckAssertion(mcfg->GetId(), info);

        Solver *solver = state->GetSolver();
        solver->PrintTimers();

        if (state->GetReportKind() != RK_None) {
          ReportKind report = state->GetReportKind();
          const char *report_string = ReportString(report);

          logout << "REPORT " << report_string;
          logout << " '" << name << "'" << endl;

          state->PrintTraits();

          Assert(state->m_path);
          state->m_path->m_name = name;
          StoreDisplayPath(state->m_path, name, id);

          report_count++;
        }
        else {
          logout << "SUCCESS '" << name << "'" << endl;
          success_count++;
        }

        delete state;

        TimerAlarm::ClearActive();

        logout << endl << flush;
      }

      BlockMemoryCache.Release(id);
      BlockSummaryCache.Release(id);
    }

    BlockCFG *file_cfg = function_cfgs[0];
    const char *file_name = file_cfg->GetBeginLocation()->FileName()->Value();

    Assert(file_name);

    logout << "Finished: '" << (const char*) body_key_buf.base << "'"
         << " FILE " << file_name
         << " REDUNDANT " << redundant_count
         << " ASSERTION " << assertion_count
         << " SUCCESS " << success_count
         << " REPORT " << report_count << endl;

    logout << "Elapsed: ";
    PrintTime(_timer.Elapsed());
    logout << endl << endl << flush;

    // clear out held references on CFGs.
    for (size_t find = 0; find < function_cfgs.Size(); find++)
      function_cfgs[find]->DecRef();
  }

  delete t;
}

int main(int argc, const char **argv)
{
  timeout.Enable();
  trans_remote.Enable();
  trans_initial.Enable();

  checker_verbose.Enable();
  checker_sufficient.Enable();
  checker_assign.Enable();
  checker_dump.Enable();
  checker_depth.Enable();
  // checker_fixup.Enable();

  solver_use.Enable();
  solver_verbose.Enable();
  solver_constraint.Enable();

  check_kind.Enable();
  check_file.Enable();
  xml_file.Enable();
  append_reports.Enable();

  Vector<const char*> checks;
  bool parsed = Config::Parse(argc, argv, &checks);
  if (!parsed) {
    Config::PrintUsage(USAGE);
    return 1;
  }

  // get the output database we're writing to.
  if (!xml_file.IsSpecified()) {
    const char *kind_str = check_kind.StringValue();
    report_database = (char*) malloc(20 + strlen(kind_str));
    sprintf((char*)report_database, "report_%s.xdb", kind_str);
  }

  // augment the list of checks by reading from a file if necessary.
  Vector<char*> file_checks;
  Buffer check_file_buf;

  if (check_file.IsSpecified()) {
    // read in check names from the specified file.
    FileInStream fin(check_file.StringValue());
    Assert(!fin.IsError());

    ReadInStream(fin, &check_file_buf);
    SplitBufferStrings(&check_file_buf, '\n', &file_checks);

    for (size_t ind = 0; ind < file_checks.Size(); ind++) {
      char *check = file_checks[ind];

      // eat any leading quote.
      if (*check == '\'' || *check == '\"')
        check++;

      // eat any trailing quote.
      size_t length = strlen(check);
      if (check[length - 1] == '\'' || check[length - 1] == '\"')
        check[length - 1] = 0;

      checks.PushBack(check);
    }
  }

  Vector<const char*> new_checks;

  // unescape any HTML markup in the checks - &amp; &lt; &gt;
  for (size_t ind = 0; ind < checks.Size(); ind++) {
    const char *check = checks[ind];
    char *new_check = new char[strlen(check) + 1];

    const char *pos = check;
    char *new_pos = new_check;
    while (*pos) {
      switch (*pos) {
      case '&':
        if (strncmp(pos, "&amp;", 5) == 0) {
          pos += 5;
          *new_pos++ = '&';
          break;
        }
        else if (strncmp(pos, "&lt;", 4) == 0) {
          pos += 4;
          *new_pos++ = '<';
          break;
        }
        else if (strncmp(pos, "&gt;", 4) == 0) {
          pos += 4;
          *new_pos++ = '>';
          break;
        }
        // fall through
      default:
        *new_pos++ = *pos++;
      }
    }
    *new_pos = *pos;

    new_checks.PushBack(new_check);
  }

  // Solver::CheckSimplifications();

  ResetAllocs();
  AnalysisPrepare();

  if (trans_initial.IsSpecified())
    SubmitInitialTransaction();
  RunAnalysis(new_checks);
  SubmitFinalTransaction();

  ClearBlockCaches();
  ClearMemoryCaches();
  AnalysisFinish(0);
}
