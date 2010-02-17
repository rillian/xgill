
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
#include <unistd.h>

NAMESPACE_XGILL_BEGIN

// file to read/write worklist information.
#define WORKLIST_FILE "worklist.sort"

// number of stages to use when writing out the callgraph worklist.
#define CALLGRAPH_STAGES 5

BACKEND_IMPL_BEGIN

/////////////////////////////////////////////////////////////////////
// Backend construction data
/////////////////////////////////////////////////////////////////////

// databases accessed as writes are received.
static Xdb *g_csu_xdb = NULL;
static Xdb *g_body_xdb = NULL;
static Xdb *g_init_xdb = NULL;
static Xdb *g_source_xdb = NULL;
static Xdb *g_preproc_xdb = NULL;

// whether we are doing an incremental build.
static bool g_incremental = false;

// whether we've written out any function bodies.
static bool g_have_body = false;

typedef HashSet<String*,String> StringSet;
typedef HashTable<String*,String*,String> StringMap;

// all CSUs, function bodies and globals we've written out.
static StringSet g_write_csu;
static StringSet g_write_body;
static StringSet g_write_init;

// all files whose contents we've written out.
static StringSet g_write_files;

// function names which are new or changed from a previous run, only used
// for incremental builds. subset of g_write_body.
static StringSet g_body_new;

// map from function names to the files containing them.
// subset of g_write_body. filenames hold references.
static StringMap g_body_file;

// list of filenames whose source has changed since a previous run, only used
// for incremental builds.
static Vector<String*> g_file_changed;

// sets of all annotations that have been processed.
typedef HashTable<String*,BlockCFG*,String> AnnotationHash;
static AnnotationHash g_annot_func;
static AnnotationHash g_annot_init;
static AnnotationHash g_annot_comp;

// sets of escape/callgraph information which the block backend has received.
typedef HashTable<String*,EscapeEdgeSet*,String> EscapeEdgeHash;
typedef HashTable<String*,EscapeAccessSet*,String> EscapeAccessHash;
typedef HashSet<CallEdgeSet*,HashObject> CallEdgeHash;
static EscapeEdgeHash g_escape_forward;
static EscapeEdgeHash g_escape_backward;
static EscapeAccessHash g_escape_accesses;
static CallEdgeHash g_callers;
static CallEdgeHash g_callees;

// quickly check whether escape information has been seen.
// these do not hold references.
static HashSet<EscapeEdgeSet*,HashObject> g_seen_escape_edges;
static HashSet<EscapeAccessSet*,HashObject> g_seen_escape_accesses;

// write out any annotations for key in one of the annotation databases,
// and consume references for key/cfg_list stored in an annotation hashtable.
void WriteAnnotations(const char *db_name, String *key,
                      Vector<BlockCFG*> &cfg_list)
{
  if (cfg_list.Empty()) {
    key->DecRef(&cfg_list);
    return;
  }

  Xdb *xdb = GetDatabase(db_name, true);
  static Buffer scratch_buf;

  // lookup and write any old entries first. this is only useful when
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

// write out the escape edges for a given trace key, and consume references
// from an escape edge hashtable.
void WriteEscapeEdges(bool forward, String *key,
                      Vector<EscapeEdgeSet*> &eset_list)
{
  Xdb *xdb = forward ?
      GetDatabase(ESCAPE_EDGE_FORWARD_DATABASE, true)
    : GetDatabase(ESCAPE_EDGE_BACKWARD_DATABASE, true);

  static Buffer scratch_buf;

  // combine with any edges already in the database for this key.
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

// write out the escape accesses for a given trace key, and consume references
// from the escape access hashtable.
void WriteEscapeAccesses(String *key,
                         Vector<EscapeAccessSet*> &aset_list)
{
  Xdb *xdb = GetDatabase(ESCAPE_ACCESS_DATABASE, true);

  static Buffer scratch_buf;

  // combine with any accesses already in the database.
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

// write out a set of call edges, and consume references from a call edge hash.
void WriteCallEdges(bool callers, CallEdgeSet *cset)
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

// flush all escape/callgraph caches to disk.
void FlushEscapeBackend()
{
  HashIterate(g_escape_forward)
    WriteEscapeEdges(true, g_escape_forward.ItKey(),
                     g_escape_forward.ItValues());
  g_escape_forward.Clear();

  HashIterate(g_escape_backward)
    WriteEscapeEdges(false, g_escape_backward.ItKey(),
                     g_escape_backward.ItValues());
  g_escape_backward.Clear();

  HashIterate(g_escape_accesses)
    WriteEscapeAccesses(g_escape_accesses.ItKey(),
                        g_escape_accesses.ItValues());
  g_escape_accesses.Clear();

  HashIterate(g_callers)
    WriteCallEdges(true, g_callers.ItKey());
  g_callers.Clear();

  HashIterate(g_callees)
    WriteCallEdges(false, g_callees.ItKey());
  g_callees.Clear();

  g_seen_escape_edges.Clear();
  g_seen_escape_accesses.Clear();
}

/////////////////////////////////////////////////////////////////////
//
// worklist file overview
//
// whether we are doing an initial or incremental build, we will write the
// worklist file indicating all the functions we have bodies for and how
// to process those functions. functions are written out as 'file$name',
// so that we can identify deleted functions when doing incremental builds.
//
////////////////////// initial layout ///////////////////////////////
//
// #stage0
// fnlist
//
// #stage1
// fnlist
//
// ...
//
// #final
// fnlist (all remaining functions)
//
////////////////////// incremental layout ///////////////////////////
//
// #new
// fnlist (all new/changed functions)
//
// #old
// fnlist (all remaining functions)
//
/////////////////////////////////////////////////////////////////////

// structure for a file/function pair and lexicographic comparison for sorting.
struct FunctionFilePair
{
  String *function;
  String *file;

  FunctionFilePair() : function(NULL), file(NULL) {}
  FunctionFilePair(String *_function, String *_file)
    : function(_function), file(_file) {}

  static int Compare(FunctionFilePair v0, FunctionFilePair v1)
  {
    int cmp = strcmp(v0.file->Value(), v1.file->Value());
    if (cmp) return cmp;
    return strcmp(v0.function->Value(), v1.function->Value());
  }
};

// sort and write out a list of functions for the worklist file.
void WriteWorklistFunctions(OutStream &out, const Vector<String*> &functions)
{
  // make a list containing the files as well, so we can get a decent sort.
  Vector<FunctionFilePair> write_list;

  for (size_t ind = 0; ind < functions.Size(); ind++) {
    String *function = functions[ind];
    String *file = g_body_file.LookupSingle(function);
    write_list.PushBack(FunctionFilePair(function, file));
  }

  SortVector<FunctionFilePair,FunctionFilePair>(&write_list);

  for (size_t ind = 0; ind < write_list.Size(); ind++)
    out << write_list[ind].file << "$" << write_list[ind].function << endl;
}

// write out the worklist file for an initial build.
void WriteWorklistInitial()
{
  Assert(!g_incremental);

  BackendStringHash *callgraph_hash =
    GetNamedHash((const uint8_t*) CALLGRAPH_EDGES);
  BackendStringHash *indirect_hash =
    GetNamedHash((const uint8_t*) CALLGRAPH_INDIRECT);

  // list of all functions that have not been placed in a stage yet.
  Vector<String*> functions;

  HashIterate(g_write_body)
    functions.PushBack(g_write_body.ItKey());

  FileOutStream worklist_out(WORKLIST_FILE);

  // functions which are members of stages we've written out.
  StringSet stage_members;

  for (size_t stage = 0; stage < CALLGRAPH_STAGES; stage++) {
    // functions to write out in this stage.
    Vector<String*> stage_functions;

    // scan the functions which don't have a stage yet, try to add them
    // to this stage.
    size_t ind = 0;
    while (ind < functions.Size()) {
      String *func = functions[ind];

      // functions can go in this stage if all their callees are in a
      // previously handled stage, and they have no indirect calls.
      bool missed = false;

      // check for a direct call to a function not previously handled.
      // treat as handled any function we don't have a body for.
      Vector<String*> *callees =
        callgraph_hash ? callgraph_hash->Lookup(func, false) : NULL;
      if (callees) {
        for (size_t eind = 0; eind < callees->Size(); eind++) {
          String *callee = callees->At(eind);
          if (g_write_body.Lookup(callee) && !stage_members.Lookup(callee))
            missed = true;
        }
      }

      if (indirect_hash && indirect_hash->Lookup(func, false)) {
        // this function and anything which might transitively call it
        // will end up in the last stage.
        missed = true;
      }

      if (missed) {
        // this function must go in a later stage.
        ind++;
      }
      else {
        // add this function to the current stage.
        stage_functions.PushBack(func);
        functions[ind] = functions.Back();
        functions.PopBack();
      }
    }

    // write out the contents of this stage.
    worklist_out << "#stage" << stage << endl;
    WriteWorklistFunctions(worklist_out, stage_functions);
    worklist_out << endl;

    // functions we added in this stage can now go in the previous set.
    for (ind = 0; ind < stage_functions.Size(); ind++)
      stage_members.Insert(stage_functions[ind]);
  }

  // the final stage contains all the functions we weren't able to place
  // in a previous stage.
  worklist_out << "#final" << endl;
  WriteWorklistFunctions(worklist_out, functions);
}

// write out the worklist file for an incremental build.
void WriteWorklistIncremental()
{
  Assert(g_incremental);

  // read and store the contents of the old worklist file.
  Buffer worklist_buf;
  Vector<char*> worklist_strings;
  {
    FileInStream worklist_in(WORKLIST_FILE);
    ReadInStream(worklist_in, &worklist_buf);
    SplitBufferStrings(&worklist_buf, '\n', &worklist_strings);
  }

  FileOutStream worklist_out(WORKLIST_FILE);

  // get the list of new/changed functions.
  Vector<String*> new_functions;
  HashIterate(g_body_new)
    new_functions.PushBack(g_body_new.ItKey());

  // write out the list of new/changed functions.
  worklist_out << "#new" << endl;
  WriteWorklistFunctions(worklist_out, new_functions);
  worklist_out << endl;

  // get the list of old functions. these are all functions in the old worklist
  // file, except for functions in the new/changed list and functions which
  // have been deleted --- the file they were in has changed, but we did
  // not see them and add them to g_write_body.

  Vector<String*> old_functions;

  for (size_t ind = 0; ind < worklist_strings.Size(); ind++) {
    char *str = worklist_strings[ind];

    // skip blank lines and stage header lines.
    if (*str == 0) continue;
    if (*str == '#') continue;

    // the string should have the format 'file$function', extract the two.
    char *separator = strchr(str, '$');
    Assert(separator);
    *separator = 0;

    String *function = String::Make(separator + 1);

    if (g_body_new.Lookup(function)) {
      // this function is new/changed and has already been written out.
      function->DecRef();
      continue;
    }

    if (g_write_body.Lookup(function)) {
      // we saw the (unchanged) body so this function definitely exists.
      old_functions.PushBack(function);
      continue;
    }

    String *file = String::Make(str);

    if (!g_file_changed.Contains(file)) {
      // the file changed (so we should have recompiled it and any other
      // files including it), but we didn't see the function. treat as deleted.
      function->DecRef();
      file->DecRef();
      continue;
    }

    // we never saw the body for this function, but its source file was not
    // modified. assume it still exists. this is the common case, where the
    // file containing this function did not need to be rebuilt.
    // this can leave ghost functions in two cases, however. first, if the
    // file was outright deleted (or otherwise not part of the project anymore)
    // we will not be able to recognize this. second, and more esoteric:
    //
    // file.h:
    //   typedef int TYPE;
    //
    // file.cc:
    //   void foo(TYPE t) {}
    //
    // changing file.h to typedef another type will change the signature
    // of foo without changing the contents of file.cc, so doing an
    // incremental build will leave both versions of foo around.

    // keep the file for the function around, for WriteWorklistFunctions.
    g_body_file.Insert(function, file);

    old_functions.PushBack(function);
  }

  // write out the list of old functions.
  worklist_out << "#old" << endl;
  WriteWorklistFunctions(worklist_out, old_functions);

  // this will leave dangling references to the old functions in g_body_file,
  // so no more inserts/lookups can be done on g_body_file.
  DecRefVector<String>(old_functions, NULL);
}

/////////////////////////////////////////////////////////////////////
// Backend worklist data
/////////////////////////////////////////////////////////////////////

// current stage of the block worklist.
static size_t g_stage = 0;

// remaining elements for stages 0 through the stage count.
static Vector<Vector<String*>*> g_stage_worklist;

// active worklist for stages over the stage count. the next stage's worklist
// is in the WORKLIST_FUNC_NEXT hash.
static Vector<String*> g_overflow_worklist;

// counters for the process/write barriers.
static size_t g_barrier_process = 0;
static size_t g_barrier_write = 0;

/////////////////////////////////////////////////////////////////////
// Backend data cleanup
/////////////////////////////////////////////////////////////////////

// write out all block backend data to disk.
void FinishBlockBackend()
{
  // write out any annotations we found.

  HashIterate(g_annot_func)
    WriteAnnotations(BODY_ANNOT_DATABASE,
                     g_annot_func.ItKey(), g_annot_func.ItValues());

  HashIterate(g_annot_init)
    WriteAnnotations(INIT_ANNOT_DATABASE,
                     g_annot_init.ItKey(), g_annot_init.ItValues());

  HashIterate(g_annot_comp)
    WriteAnnotations(COMP_ANNOT_DATABASE,
                     g_annot_comp.ItKey(), g_annot_comp.ItValues());

  // flush any escape/callgraph changes.
  FlushEscapeBackend();

  // flush any worklist information.
  if (g_have_body) {
    if (g_incremental)
      WriteWorklistIncremental();
    else
      WriteWorklistInitial();
  }

  // drop references on written names.

  HashIterate(g_write_csu)   g_write_csu.ItKey()->DecRef();
  HashIterate(g_write_body)  g_write_body.ItKey()->DecRef();
  HashIterate(g_write_init)  g_write_init.ItKey()->DecRef();
  HashIterate(g_write_files) g_write_files.ItKey()->DecRef();
  HashIterate(g_body_file)   g_body_file.ItValues()[0]->DecRef();

  // drop references on worklist data.

  for (size_t ind = 0; ind < g_stage_worklist.Size(); ind++) {
    Vector<String*> *data = g_stage_worklist[ind];
    DecRefVector<String>(*data, NULL);
    delete data;
  }

  DecRefVector<String>(g_overflow_worklist, NULL);
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
    g_source_xdb = GetDatabase(SOURCE_DATABASE, true);
    g_preproc_xdb = GetDatabase(PREPROC_DATABASE, true);

    // figure out if we are doing an incremental analysis:
    // there is an existing worklist file.
    if (access(WORKLIST_FILE, F_OK) == 0)
      g_incremental = true;
  }

  static Buffer result_buf;

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_Name: {
      String *name = String::ReadWithTag(&read_buf, TAG_Name);

      if (!g_write_csu.Insert(name)) {
        name->IncRef();
        String::WriteWithTag(&result_buf, name, TAG_Name);
      }

      name->DecRef();
      break;
    }

    case TAG_BlockId: {
      BlockId *id = BlockId::Read(&read_buf);
      String *name = id->Function();

      if (id->Kind() == B_FunctionWhole) {
        if (!g_write_body.Insert(name)) {
          name->IncRef();
          BlockId::Write(&result_buf, id);
        }
      }
      else if (id->Kind() == B_Initializer) {
        if (!g_write_init.Insert(name)) {
          name->IncRef();
          BlockId::Write(&result_buf, id);
        }
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

      String *name = csu->GetName();
      Assert(g_write_csu.Lookup(name));

      Assert(g_csu_xdb);
      XdbReplaceCompress(g_csu_xdb, name, &write_buf);

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

      Xdb *xdb = NULL;
      if (id->Kind() == B_Function || id->Kind() == B_Loop) {
        xdb = g_body_xdb;

        Assert(g_write_body.Lookup(name));

        // remember the file this function was defined in.
        String *filename = function_cfgs[0]->GetBeginLocation()->FileName();
        filename->IncRef();
        g_body_file.Insert(name, filename);

        if (g_incremental) {
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
                if (!old_cfgs[ind]->IsEquivalent(function_cfgs[ind])) {
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

          if (incremental_new)
            g_body_new.Insert(name);
        }
      }
      else if (id->Kind() == B_Initializer) {
        xdb = g_init_xdb;

        Assert(g_write_init.Lookup(name));
      }
      Assert(xdb);

      XdbReplaceCompress(xdb, name, &write_buf);

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

bool BlockQueryFile(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, file_name, file_length);

  String *file = String::Make((const char*) file_name);
  bool found = g_write_files.Insert(file);

  if (found)
    file->DecRef();

  *result = new TOperandBoolean(t, found);
  return true;
}

bool BlockWriteFile(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, file_name, file_length);
  BACKEND_ARG_DATA(1, source_data, source_length);
  BACKEND_ARG_DATA(2, preproc_data, preproc_length);

  String *file = String::Make((const char*) file_name);

  if (g_incremental) {
    // compare the preprocessed contents with the old data to look for changes.
    bool preproc_new = false;

    static Buffer compare_buf;
    if (XdbFindUncompressed(g_preproc_xdb, file, &compare_buf)) {
      if (compare_buf.pos - compare_buf.base != (ptrdiff_t) preproc_length)
        preproc_new = true;
      else if (memcmp(compare_buf.base, preproc_data, preproc_length) != 0)
        preproc_new = true;
      compare_buf.Reset();
    }
    else {
      // entirely new file.
      preproc_new = true;
    }

    if (preproc_new && !g_file_changed.Contains(file)) {
      file->IncRef();
      g_file_changed.PushBack(file);
    }
  }

  Buffer source_buf(source_data, source_length);
  XdbReplaceCompress(g_source_xdb, file, &source_buf);

  Buffer preproc_buf(preproc_data, preproc_length);
  XdbReplaceCompress(g_preproc_xdb, file, &preproc_buf);

  file->DecRef();
  return true;
}

bool BlockLoadWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  if (!g_stage_worklist.Empty()) {
    // ignore duplicate loads.
    *result = new TOperandInteger(t, g_stage_worklist.Size() - 1);
    return true;
  }

  Buffer worklist_buf;
  Vector<char*> worklist_strings;
  {
    FileInStream worklist_in(WORKLIST_FILE);
    ReadInStream(worklist_in, &worklist_buf);
    SplitBufferStrings(&worklist_buf, '\n', &worklist_strings);
  }

  Vector<String*> *stage_list = NULL;

  // we are incremental if the first stage is '#new'. in this case
  // functions under the subsequent '#old' will be ignored.
  bool incremental = false;

  for (size_t ind = 0; ind < worklist_strings.Size(); ind++) {
    char *str = worklist_strings[ind];
    if (*str == 0) continue;

    if (*str == '#') {
      // new stage, check its name for incremental analysis.
      // the names are ignored for initial analysis.

      if (incremental) {
        Assert(!strcmp(str, "#old"));
        break;
      }

      if (!strcmp(str, "#new"))
        incremental = true;

      stage_list = new Vector<String*>();
      g_stage_worklist.PushBack(stage_list);
      continue;
    }

    // get the function name from the 'file$function' format.
    char *separator = strchr(str, '$');
    Assert(separator);
    String *function = String::Make(separator + 1);

    stage_list->PushBack(function);
  }

  if (g_stage_worklist.Empty()) {
    // no functions at all. make an empty stage.
    g_stage_worklist.PushBack(new Vector<String*>());
  }

  *result = new TOperandInteger(t, g_stage_worklist.Size() - 1);
  return true;
}

bool BlockSeedWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_LIST(0, functions);

  if (!g_stage_worklist.Empty()) {
    // ignore duplicate loads/seeds.
    return true;
  }

  Vector<String*> *seed_list = new Vector<String*>();
  g_stage_worklist.PushBack(seed_list);

  for (size_t ind = 0; ind < functions->GetCount(); ind++) {
    if (functions->GetOperand(ind)->Kind() != TO_String)
      BACKEND_FAIL(functions->GetOperand(ind));

    TOperandString *str = functions->GetOperand(ind)->AsString();
    if (!ValidString(str->GetData(), str->GetDataLength()))
      BACKEND_FAIL(str);

    seed_list->PushBack(String::Make((const char*) str->GetData()));
  }

  return true;
}

bool BlockCurrentStage(Transaction *t, const Vector<TOperand*> &arguments,
                       TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  *result = new TOperandInteger(t, g_stage);
  return true;
}

bool BlockPopWorklist(Transaction *t, const Vector<TOperand*> &arguments,
                      TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_BOOLEAN(0, have_barrier_process);

  Vector<String*> *worklist = (g_stage < g_stage_worklist.Size())
    ? g_stage_worklist[g_stage]
    : &g_overflow_worklist;

  if (!worklist->Empty()) {
    String *function = worklist->Back();
    const char *new_function = t->CloneString(function->Value());

    worklist->PopBack();
    function->DecRef();

    if (!have_barrier_process)
      g_barrier_process++;

    *result = new TOperandString(t, new_function);
    return true;
  }

  // the current stage is exhausted, either block or advance to the next stage
  // depending on whether the barriers are clear. either way we don't return
  // a function, the worker will have to pop the worklist again.

  *result = new TOperandString(t, "");

  if (g_barrier_process != 0 || g_barrier_write != 0)
    return true;

  g_stage++;

  if (g_stage >= g_stage_worklist.Size()) {
    // we are fixpointing after the initial pass over the callgraph, load the
    // overflow worklist from the hash storing new functions to analyze.

    BackendStringHash *next_hash =
      GetNamedHash((const uint8_t*) WORKLIST_FUNC_NEXT);

    if (next_hash) {
      HashIteratePtr(next_hash) {
        next_hash->ItKey()->IncRef();
        g_overflow_worklist.PushBack(next_hash->ItKey());
      }
      ClearStringHash(next_hash);
    }
  }

  return true;
}

bool BlockHaveBarrierProcess(Transaction *t,
                             const Vector<TOperand*> &arguments,
                             TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  *result = new TOperandBoolean(t, g_barrier_process != 0);
  return true;
}

bool BlockHaveBarrierWrite(Transaction *t,
                           const Vector<TOperand*> &arguments,
                           TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  *result = new TOperandBoolean(t, g_barrier_write != 0);
  return true;
}

bool BlockShiftBarrierProcess(Transaction *t,
                              const Vector<TOperand*> &arguments,
                              TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  if (g_barrier_process == 0)
    BACKEND_FAIL(NULL);

  g_barrier_process--;
  g_barrier_write++;
  return true;
}

bool BlockDropBarrierWrite(Transaction *t, const Vector<TOperand*> &arguments,
                           TOperand **result)
{
  BACKEND_ARG_COUNT(0);

  if (g_barrier_write == 0)
    BACKEND_FAIL(NULL);

  g_barrier_write--;
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
  BACKEND_REGISTER(BlockQueryFile);
  BACKEND_REGISTER(BlockWriteFile);
  BACKEND_REGISTER(BlockLoadWorklist);
  BACKEND_REGISTER(BlockSeedWorklist);
  BACKEND_REGISTER(BlockCurrentStage);
  BACKEND_REGISTER(BlockPopWorklist);
  BACKEND_REGISTER(BlockHaveBarrierProcess);
  BACKEND_REGISTER(BlockHaveBarrierWrite);
  BACKEND_REGISTER(BlockShiftBarrierProcess);
  BACKEND_REGISTER(BlockDropBarrierWrite);
}

static void finish_Block()
{
  Backend_IMPL::FinishBlockBackend();
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

TAction* BlockQueryFile(Transaction *t, const char *file, size_t var_result)
{
  BACKEND_CALL(BlockQueryFile, var_result);
  call->PushArgument(new TOperandString(t, file));
  return call;
}

TAction* BlockWriteFile(Transaction *t, const char *file,
                        TOperand *source_data, TOperand *preproc_data)
{
  BACKEND_CALL(BlockWriteFile, 0);
  call->PushArgument(new TOperandString(t, file));
  call->PushArgument(source_data);
  call->PushArgument(preproc_data);
  return call;
}

TAction* BlockLoadWorklist(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockLoadWorklist, var_result);
  return call;
}

TAction* BlockSeedWorklist(Transaction *t, TOperandList *functions)
{
  BACKEND_CALL(BlockSeedWorklist, 0);
  call->PushArgument(functions);
  return call;
}

TAction* BlockCurrentStage(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockCurrentStage, var_result);
  return call;
}

TAction* BlockPopWorklist(Transaction *t, bool have_barrier_process,
                          size_t var_result)
{
  BACKEND_CALL(BlockPopWorklist, var_result);
  call->PushArgument(new TOperandBoolean(t, have_barrier_process));
  return call;
}

TAction* BlockHaveBarrierProcess(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockHaveBarrierProcess, var_result);
  return call;
}

TAction* BlockHaveBarrierWrite(Transaction *t, size_t var_result)
{
  BACKEND_CALL(BlockHaveBarrierWrite, var_result);
  return call;
}

TAction* BlockShiftBarrierProcess(Transaction *t)
{
  BACKEND_CALL(BlockShiftBarrierProcess, 0);
  return call;
}

TAction* BlockDropBarrierWrite(Transaction *t)
{
  BACKEND_CALL(BlockDropBarrierWrite, 0);
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
