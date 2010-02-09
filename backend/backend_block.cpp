
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

#include "backend_block.h"
#include "backend_xdb.h"
#include <imlang/storage.h>
#include <memory/storage.h>
#include <memory/serial.h>
#include <util/monitor.h>

NAMESPACE_XGILL_BEGIN

// number of stages to generate in the callgraph sort.
#define CALLGRAPH_STAGES 5

ConfigOption option_incremental_files(CK_String, "incremental-files", "",
  "for incremental analysis, colon-separated list of changed files");

/////////////////////////////////////////////////////////////////////
// backend data
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

// databases accessed as writes are received.
static Xdb *g_csu_xdb = NULL;
static Xdb *g_body_xdb = NULL;
static Xdb *g_init_xdb = NULL;

// whether we've written out any function bodies.
static bool g_have_body = false;

// all CSUs, function bodies and globals we've written out.
static HashSet<String*,String> g_write_csu;
static HashSet<String*,String> g_write_body;
static HashSet<String*,String> g_write_init;

// for incremental analysis, names of functions which have are new or changed.
// this table does not hold references.
static HashSet<String*,String> g_body_new;

// map from filenames to all functions in that file which we've written out.
// holds references for the keys.
static HashTable<String*,String*,String> g_body_files;

// list of filenames whose contents have changed in some way.
static Vector<String*> g_file_changed;

// sets of all annotations that have been processed.
static HashTable<String*,BlockCFG*,String> g_annot_func;
static HashTable<String*,BlockCFG*,String> g_annot_init;
static HashTable<String*,BlockCFG*,String> g_annot_comp;

// sets of escape/callgraph information which the block backend has received.
static HashTable<String*,EscapeEdgeSet*,String> g_escape_forward;
static HashTable<String*,EscapeEdgeSet*,String> g_escape_backward;
static HashTable<String*,EscapeAccessSet*,String> g_escape_accesses;
static HashSet<CallEdgeSet*,HashObject> g_callers;
static HashSet<CallEdgeSet*,HashObject> g_callees;

// quickly check whether escape information has been seen.
// these do not hold references.
static HashSet<EscapeEdgeSet*,HashObject> g_seen_escape_edges;
static HashSet<EscapeAccessSet*,HashObject> g_seen_escape_accesses;

// visitor to write out generated annotations to the databases.

class WriteAnnotationVisitor : public HashTableVisitor<String*,BlockCFG*>
{
public:
  const char *db_name;
  WriteAnnotationVisitor(const char *_db_name) : db_name(_db_name) {}

  void Visit(String *&key, Vector<BlockCFG*> &cfg_list)
  {
    if (cfg_list.Empty()) {
      key->DecRef(&cfg_list);
      return;
    }

    Xdb *xdb = GetDatabase(db_name, true);
    static Buffer scratch_buf;

    // lookup and write any old entries first. this is only useful to do when
    // there isn't a manager running.
    XdbFindUncompressed(xdb, key, &scratch_buf);

    Vector<BlockCFG*> old_cfg_list;
    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG::ReadList(&read_buf, &old_cfg_list);
    scratch_buf.Reset();

    for (size_t ind = 0; ind < old_cfg_list.Size(); ind++) {
      BlockCFG *cfg = old_cfg_list[ind];

      // watch for duplicate CFGs.
      if (cfg_list.Contains(cfg)) {
        cfg->DecRef();
      }
      else {
        cfg->MoveRef(NULL, &cfg_list);
        cfg_list.PushBack(cfg);
      }
    }

    // write the old and new entries out.
    BlockCFG::WriteList(&scratch_buf, cfg_list);

    XdbReplaceCompress(xdb, key, &scratch_buf);
    scratch_buf.Reset();

    key->DecRef(&cfg_list);
    for (size_t ind = 0; ind < cfg_list.Size(); ind++)
      cfg_list[ind]->DecRef(&cfg_list);
  }
};

// visitors to write all generated escape/callgraph info to the databases.

// read an escape edge set from buf and combine it with any in memory data.
EscapeEdgeSet* CombineEscapeEdge(Buffer *buf)
{
  Trace *source = NULL;
  bool forward = false;
  Vector<EscapeEdge> edges;
  EscapeEdgeSet::ReadMerge(buf, &source, &forward, &edges);

  EscapeEdgeSet *eset = EscapeEdgeSet::Make(source, forward);

  for (size_t ind = 0; ind < edges.Size(); ind++)
    eset->AddEdge(edges[ind]);
  return eset;
}

class WriteEscapeEdgeSetVisitor
  : public HashTableVisitor<String*,EscapeEdgeSet*>
{
public:
  bool forward;
  WriteEscapeEdgeSetVisitor(bool _forward) : forward(_forward) {}

  void Visit(String *&key, Vector<EscapeEdgeSet*> &eset_list)
  {
    Xdb *xdb = forward ?
        GetDatabase(ESCAPE_EDGE_FORWARD_DATABASE, true)
      : GetDatabase(ESCAPE_EDGE_BACKWARD_DATABASE, true);

    static Buffer scratch_buf;

    if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
      Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
      while (read_buf.pos != read_buf.base + read_buf.size) {
        EscapeEdgeSet *eset = CombineEscapeEdge(&read_buf);
        if (!eset_list.Contains(eset)) {
          eset->IncRef(&eset_list);
          eset_list.PushBack(eset);
        }
        eset->DecRef();
      }
    }

    for (size_t ind = 0; ind < eset_list.Size(); ind++) {
      EscapeEdgeSet *eset = eset_list[ind];
      EscapeEdgeSet::Write(&scratch_buf, eset);
      eset->DecRef(&eset_list);
    }

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    key->DecRef(&eset_list);
  }
};

// read an escape access set from buf and combine it with any in memory data.
EscapeAccessSet* CombineEscapeAccess(Buffer *buf)
{
  Trace *value = NULL;
  Vector<EscapeAccess> accesses;
  EscapeAccessSet::ReadMerge(buf, &value, &accesses);

  EscapeAccessSet *aset = EscapeAccessSet::Make(value);

  for (size_t ind = 0; ind < accesses.Size(); ind++)
    aset->AddAccess(accesses[ind]);
  return aset;
}

class WriteEscapeAccessSetVisitor
  : public HashTableVisitor<String*,EscapeAccessSet*>
{
public:
  void Visit(String *&key, Vector<EscapeAccessSet*> &aset_list)
  {
    Xdb *xdb = GetDatabase(ESCAPE_ACCESS_DATABASE, true);

    static Buffer scratch_buf;

    if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
      Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
      while (read_buf.pos != read_buf.base + read_buf.size) {
        EscapeAccessSet *aset = CombineEscapeAccess(&read_buf);
        if (!aset_list.Contains(aset)) {
          aset->IncRef(&aset_list);
          aset_list.PushBack(aset);
        }
        aset->DecRef();
      }
    }

    for (size_t ind = 0; ind < aset_list.Size(); ind++) {
      EscapeAccessSet *aset = aset_list[ind];
      EscapeAccessSet::Write(&scratch_buf, aset);
      aset->DecRef(&aset_list);
    }

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    key->DecRef(&aset_list);
  }
};

// read a call edge set from buf and combine it with any in memory data.
CallEdgeSet* CombineCallEdge(Buffer *buf)
{
  Variable *function = NULL;
  bool callers = false;
  Vector<CallEdge> edges;
  CallEdgeSet::ReadMerge(buf, &function, &callers, &edges);

  CallEdgeSet *cset = CallEdgeSet::Make(function, callers);

  for (size_t ind = 0; ind < edges.Size(); ind++)
    cset->AddEdge(edges[ind]);
  return cset;
}

class WriteCallEdgeSetVisitor : public HashSetVisitor<CallEdgeSet*>
{
public:
  bool callers;
  WriteCallEdgeSetVisitor(bool _callers) : callers(_callers) {}

  void Visit(CallEdgeSet *&cset)
  {
    Xdb *xdb = callers ?
        GetDatabase(CALLER_DATABASE, true)
      : GetDatabase(CALLEE_DATABASE, true);
    String *key = cset->GetFunction()->GetName();

    static Buffer scratch_buf;

    if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
      Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
      CallEdgeSet *new_cset = CombineCallEdge(&read_buf);
      Assert(new_cset == cset);
      new_cset->DecRef();
    }

    CallEdgeSet::Write(&scratch_buf, cset);

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    cset->DecRef();
  }
};

// flush all escape/callgraph caches to disk.
void FlushEscapeBackend()
{
  WriteEscapeEdgeSetVisitor visitor_eset_forward(true);
  g_escape_forward.VisitEach(&visitor_eset_forward);
  g_escape_forward.Clear();

  WriteEscapeEdgeSetVisitor visitor_eset_backward(false);
  g_escape_backward.VisitEach(&visitor_eset_backward);
  g_escape_backward.Clear();

  WriteEscapeAccessSetVisitor visitor_aset;
  g_escape_accesses.VisitEach(&visitor_aset);
  g_escape_accesses.Clear();

  WriteCallEdgeSetVisitor visitor_cset_caller(true);
  g_callers.VisitEach(&visitor_cset_caller);
  g_callers.Clear();

  WriteCallEdgeSetVisitor visitor_cset_callee(false);
  g_callees.VisitEach(&visitor_cset_callee);
  g_callees.Clear();

  g_seen_escape_edges.Clear();
  g_seen_escape_accesses.Clear();
}

// write out either the program's callgraph or any incremental information,
// depending on whether we are doing incremental analysis.
void WriteFunctionData()
{
  if (option_incremental.IsSpecified()) {
    // write out the incremental file.
    ofstream out(INCREMENTAL_FILE);

    // write out all the changed files.
    for (size_t ind = 0; ind < g_file_changed.Size(); ind++)
      out << g_file_changed[ind]->Value() << endl;
    out << endl;

    // write out unchanged functions from changed files.
    for (size_t ind = 0; ind < g_file_changed.Size(); ind++) {
      String *filename = g_file_changed[ind];
      Vector<String*> *all_functions = g_body_files.Lookup(filename, false);
      if (all_functions) {
        for (size_t find = 0; find < all_functions->Size(); find++) {
          if (!g_body_new.Lookup(all_functions->At(find)))
            out << all_functions->At(find) << endl;
        }
      }
    }
    out << endl;

    // write out new or changed functions from changed files.
    for (size_t ind = 0; ind < g_file_changed.Size(); ind++) {
      String *filename = g_file_changed[ind];
      Vector<String*> *all_functions = g_body_files.Lookup(filename, false);
      if (all_functions) {
        for (size_t find = 0; find < all_functions->Size(); find++) {
          if (g_body_new.Lookup(all_functions->At(find)))
            out << all_functions->At(find) << endl;
        }
      }
    }

    DecRefVector<String>(g_file_changed, NULL);
  }
  else {
    // sort and write out the callgraph hash.
    BACKEND_IMPL::Backend_GraphSortHash((const uint8_t*) CALLGRAPH_NAME,
                                        (const uint8_t*) CALLGRAPH_INDIRECT,
                                        (const uint8_t*) BODY_DATABASE,
                                        (const uint8_t*) CALLGRAPH_NAME,
                                        CALLGRAPH_STAGES);
  }
}

/////////////////////////////////////////////////////////////////////
// Backend implementations
/////////////////////////////////////////////////////////////////////

bool BlockQueryAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, db_name, db_length);
  BACKEND_ARG_STRING(1, var_name, var_length);
  BACKEND_ARG_STRING(2, annot_name, annot_length);

  String *new_var_name = String::Make((const char*) var_name);

  Vector<BlockCFG*> *cfg_list = NULL;
  if (!strcmp((const char*) db_name, BODY_ANNOT_DATABASE))
    cfg_list = g_annot_func.Lookup(new_var_name);
  else if (!strcmp((const char*) db_name, INIT_ANNOT_DATABASE))
    cfg_list = g_annot_init.Lookup(new_var_name);
  else if (!strcmp((const char*) db_name, COMP_ANNOT_DATABASE))
    cfg_list = g_annot_comp.Lookup(new_var_name);
  else
    Assert(false);

  new_var_name->DecRef();

  bool found = false;
  for (size_t ind = 0; cfg_list && ind < cfg_list->Size(); ind++) {
    const char *exist_name = cfg_list->At(ind)->GetId()->Loop()->Value();
    if (!strcmp((const char*) annot_name, exist_name))
      found = true;
  }

  *result = new TOperandBoolean(t, found);
  return true;
}

bool BlockWriteAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  BlockCFG *annot_cfg = BlockCFG::Read(&read_buf);
  BlockId *id = annot_cfg->GetId();
  String *var_name = id->Function();
  data_buf.Reset();

  Vector<BlockCFG*> *cfg_list = NULL;

  switch (id->Kind()) {
  case B_AnnotationFunc:
    cfg_list = g_annot_func.Lookup(var_name, true); break;
  case B_AnnotationInit:
    cfg_list = g_annot_init.Lookup(var_name, true); break;
  case B_AnnotationComp:
    cfg_list = g_annot_comp.Lookup(var_name, true); break;
  default: Assert(false);
  }

  if (cfg_list->Empty()) {
    // first time we saw this key.
    var_name->IncRef(cfg_list);
  }

  annot_cfg->MoveRef(NULL, cfg_list);
  cfg_list->PushBack(annot_cfg);

  return true;
}

bool BlockQueryList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  // open up the databases if this is our first time here.
  static bool have_query = false;
  if (!have_query) {
    have_query = true;
    g_csu_xdb = GetDatabase(COMP_DATABASE, true);
    g_body_xdb = GetDatabase(BODY_DATABASE, true);
    g_init_xdb = GetDatabase(INIT_DATABASE, true);
  }

  static Buffer result_buf;

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_Name: {
      String *name = String::ReadWithTag(&read_buf, TAG_Name);

      if (!g_write_csu.Lookup(name))
        String::WriteWithTag(&result_buf, name, TAG_Name);

      name->DecRef();
      break;
    }

    case TAG_BlockId: {
      BlockId *id = BlockId::Read(&read_buf);

      if (id->Kind() == B_FunctionWhole) {
        if (!g_write_body.Lookup(id->Function()))
          BlockId::Write(&result_buf, id);
      }
      else if (id->Kind() == B_Initializer) {
        if (!g_write_init.Lookup(id->Function()))
          BlockId::Write(&result_buf, id);
      }
      else {
        Assert(false);
      }

      id->DecRef();
      break;
    }

    default:
      Assert(false);
    }
  }

  data_buf.Reset();

  if (result_buf.pos == result_buf.base) {
    // none of the elements in the list need to be processed.
    *result = new TOperandString(t, NULL, 0);
    return true;
  }

  Buffer *compress_buf = new Buffer();
  t->AddBuffer(compress_buf);

  CompressBufferInUse(&result_buf, compress_buf);
  result_buf.Reset();

  *result = new TOperandString(t, compress_buf->base,
                               compress_buf->pos - compress_buf->base);
  return true;
}

bool BlockWriteList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  static Buffer write_buf;

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_CompositeCSU: {
      CompositeCSU *csu = CompositeCSU::Read(&read_buf);
      CompositeCSU::Write(&write_buf, csu);

      Assert(g_csu_xdb);
      XdbReplaceCompress(g_csu_xdb, csu->GetName(), &write_buf);

      String *name = csu->GetName();
      name->IncRef();

      Try(!g_write_csu.Insert(name));

      csu->DecRef();
      write_buf.Reset();
      break;
    }

    case TAG_UInt32: {
      uint32_t count = 0;
      Try(ReadUInt32(&read_buf, &count));
      Assert(count);

      g_have_body = true;

      // the count indicates the number of CFGs for the next function/global.
      Vector<BlockCFG*> function_cfgs;

      for (size_t ind = 0; ind < count; ind++) {
        BlockCFG *cfg = BlockCFG::Read(&read_buf);
        BlockCFG::Write(&write_buf, cfg);
        function_cfgs.PushBack(cfg);
      }

      BlockId *id = function_cfgs[0]->GetId();

      String *name = id->Function();
      name->IncRef();

      Xdb *xdb = NULL;
      if (id->Kind() == B_Function || id->Kind() == B_Loop) {
        xdb = g_body_xdb;
        Try(!g_write_body.Insert(name));

        if (option_incremental.IsSpecified()) {
          // remember the file this function was defined in.
          String *filename = function_cfgs[0]->GetBeginLocation()->FileName();

          if (!g_body_files.Insert(filename, name))
            filename->IncRef();

          // mark as changed any files specified at the command line.
          // this is primarily so that we can account for files deleted by the
          // patch used for the incremental analysis, or files where the only
          // change was deleting functions. this only needs to be done once.
          static bool marked_option = false;
          if (!marked_option) {
            marked_option = true;
            const char *option = option_incremental_files.StringValue();

            Buffer option_buf;
            option_buf.Append((const uint8_t*) option, strlen(option) + 1);

            Vector<char*> option_files;
            SplitBufferStrings(&option_buf, ':', &option_files);

            for (size_t ind = 0; ind < option_files.Size(); ind++) {
              char *file = option_files[ind];
              if (*file)
                g_file_changed.PushBack(String::Make(file));
            }
          }

          // look for an old function and check if the new one is isomorphic.
          bool incremental_new = false;

          static Buffer compare_buf;
          if (XdbFindUncompressed(xdb, name, &compare_buf)) {
            // clone the old CFGs when reading them in to distinguish them
            // from the new CFGs we're writing out.
            Vector<BlockCFG*> old_cfgs;
            BlockCFG::ReadList(&compare_buf, &old_cfgs, true);

            if (old_cfgs.Size() == function_cfgs.Size()) {
              for (size_t ind = 0; ind < old_cfgs.Size(); ind++) {
                if (!old_cfgs[ind]->Isomorphic(function_cfgs[ind])) {
                  // change in the contents of this function/loop.
                  incremental_new = true;
                }
              }
            }
            else {
              // change in the number of loops.
              incremental_new = true;
            }

            DecRefVector<BlockCFG>(old_cfgs, NULL);
            compare_buf.Reset();
          }
          else {
            // this is a new function, there is no old one to compare with.
            incremental_new = true;
          }

          if (incremental_new) {
            g_body_new.Insert(name);

            // mark the file as changed if this was not already known.
            if (!g_file_changed.Contains(filename)) {
              filename->IncRef();
              g_file_changed.PushBack(filename);
            }
          }
        }
      }
      else if (id->Kind() == B_Initializer) {
        xdb = g_init_xdb;
        Try(!g_write_init.Insert(name));
      }
      Assert(xdb);

      XdbReplaceCompress(xdb, id->Function(), &write_buf);

      for (size_t ind = 0; ind < function_cfgs.Size(); ind++)
        function_cfgs[ind]->DecRef();
      write_buf.Reset();
      break;
    }

    // these append to the existing set if there is one.

    case TAG_EscapeEdgeSet: {
      EscapeEdgeSet *eset = CombineEscapeEdge(&read_buf);

      if (g_seen_escape_edges.Insert(eset)) {
        eset->DecRef();
        break;
      }

      String *key = GetTraceKey(eset->GetSource());

      Vector<EscapeEdgeSet*> *eset_list = (eset->IsForward()) ?
          g_escape_forward.Lookup(key, true)
        : g_escape_backward.Lookup(key, true);

      if (eset_list->Empty())
        key->IncRef(eset_list);
      eset->MoveRef(NULL, eset_list);
      eset_list->PushBack(eset);

      key->DecRef();
      break;
    }

    case TAG_EscapeAccessSet: {
      EscapeAccessSet *aset = CombineEscapeAccess(&read_buf);

      if (g_seen_escape_accesses.Insert(aset)) {
        aset->DecRef();
        break;
      }

      String *key = GetTraceKey(aset->GetValue());

      Vector<EscapeAccessSet*> *aset_list =
        g_escape_accesses.Lookup(key, true);

      if (aset_list->Empty())
        key->IncRef(aset_list);
      aset->MoveRef(NULL, aset_list);
      aset_list->PushBack(aset);

      key->DecRef();
      break;
    }

    case TAG_CallEdgeSet: {
      CallEdgeSet *cset = CombineCallEdge(&read_buf);

      bool exists = (cset->IsCallers()) ?
          g_callers.Insert(cset)
        : g_callees.Insert(cset);

      if (exists)
        cset->DecRef();
      break;
    }

    default:
      Assert(false);
    }
  }

  if (IsHighVmUsage()) {
    logout << "WARNING: High memory usage, flushing caches..." << endl;
    FlushEscapeBackend();
  }

  data_buf.Reset();
  return true;
}

bool BlockFlush(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(0);
  FlushEscapeBackend();
  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// Backend
/////////////////////////////////////////////////////////////////////

static void start_Block()
{
  BACKEND_REGISTER(BlockQueryAnnot);
  BACKEND_REGISTER(BlockWriteAnnot);
  BACKEND_REGISTER(BlockQueryList);
  BACKEND_REGISTER(BlockWriteList);
  BACKEND_REGISTER(BlockFlush);
}

class DropWriteVisitor : public HashSetVisitor<String*>
{
  void Visit(String *&name) { name->DecRef(); }
};

class DropFileVisitor : public HashTableVisitor<String*,String*>
{
  void Visit(String *&filename, Vector<String*>&) { filename->DecRef(); }
};

static void finish_Block()
{
  // write out any annotations we found.

  Backend_IMPL::WriteAnnotationVisitor func_visitor(BODY_ANNOT_DATABASE);
  Backend_IMPL::g_annot_func.VisitEach(&func_visitor);

  Backend_IMPL::WriteAnnotationVisitor init_visitor(INIT_ANNOT_DATABASE);
  Backend_IMPL::g_annot_init.VisitEach(&init_visitor);

  Backend_IMPL::WriteAnnotationVisitor comp_visitor(COMP_ANNOT_DATABASE);
  Backend_IMPL::g_annot_comp.VisitEach(&comp_visitor);

  // flush any escape/callgraph changes.
  Backend_IMPL::FlushEscapeBackend();

  // flush any callgraph or incremental information.
  if (Backend_IMPL::g_have_body)
    Backend_IMPL::WriteFunctionData();

  // drop references on written names.

  DropWriteVisitor drop_visitor;
  Backend_IMPL::g_write_csu.VisitEach(&drop_visitor);
  Backend_IMPL::g_write_body.VisitEach(&drop_visitor);
  Backend_IMPL::g_write_init.VisitEach(&drop_visitor);

  DropFileVisitor drop_file_visitor;
  Backend_IMPL::g_body_files.VisitEach(&drop_file_visitor);
}

TransactionBackend backend_Block(start_Block, finish_Block);

/////////////////////////////////////////////////////////////////////
// Backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* BlockQueryList(Transaction *t, TOperand *query_data,
                        size_t var_result)
{
  BACKEND_CALL(BlockQueryList, var_result);
  call->PushArgument(query_data);
  return call;
}

TAction* BlockWriteList(Transaction *t, TOperand *write_data)
{
  BACKEND_CALL(BlockWriteList, 0);
  call->PushArgument(write_data);
  return call;
}

TAction* BlockQueryAnnot(Transaction *t, const char *db_name,
                         const char *var_name, const char *annot_name,
                         size_t var_result)
{
  BACKEND_CALL(BlockQueryAnnot, var_result);
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(new TOperandString(t, var_name));
  call->PushArgument(new TOperandString(t, annot_name));
  return call;
}

TAction* BlockWriteAnnot(Transaction *t, TOperand *annot_data)
{
  BACKEND_CALL(BlockWriteAnnot, 0);
  call->PushArgument(annot_data);
  return call;
}

TAction* BlockFlush(Transaction *t)
{
  BACKEND_CALL(BlockFlush, 0);
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
