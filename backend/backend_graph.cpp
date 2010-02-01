
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

#include "backend_graph.h"
#include "backend_hash.h"
#include "backend_util.h"
#include "backend_xdb.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// backend data
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

// information about all nodes in some stage of some sort.
struct SortStageInfo {
  String *name;
  size_t stage;

  // list of all nodes in this stage.
  Vector<DataString*> entries;

  SortStageInfo() : name(NULL), stage(0) {}
};

// list of all in-memory graph sorts.
Vector<SortStageInfo> sort_stages;

void ClearSorts()
{
  for (size_t ind = 0; ind < sort_stages.Size(); ind++) {
    SortStageInfo &info = sort_stages[ind];
    info.name->DecRef(&sort_stages);

    DecRefVector<DataString>(info.entries, &sort_stages);
  }

  sort_stages.Clear();
}

SortStageInfo& GetSort(const uint8_t *name, size_t stage, bool clear = false)
{
  String *name_str = String::Make((const char*)name);
  for (size_t ind = 0; ind < sort_stages.Size(); ind++) {
    SortStageInfo &info = sort_stages[ind];

    if (info.name == name_str && info.stage == stage) {
      name_str->DecRef();

      if (clear && !info.entries.Empty()) {
        DecRefVector<DataString>(info.entries, &sort_stages);
        info.entries.Clear();
      }

      return info;
    }
  }

  name_str->MoveRef(NULL, &sort_stages);

  SortStageInfo info;
  info.name = name_str;
  info.stage = stage;
  sort_stages.PushBack(info);

  return sort_stages.Back();
}

// comparison for sorting keys within a stage.
struct SortDataString
{
  static int Compare(DataString *s0, DataString *s1)
  {
    if (s0->ValueLength() < s1->ValueLength())
      return -1;
    if (s0->ValueLength() > s1->ValueLength())
      return 1;
    return memcmp(s0->Value(), s1->Value(), s0->ValueLength());
  }
};

void Backend_GraphSortHash(const uint8_t *hash_name, const uint8_t *hash_unk,
                           const uint8_t *db_name,
                           const uint8_t *sort_name, size_t stage_count)
{
  Xdb *xdb = GetDatabase((const char*) db_name, true);
  Assert(xdb);

  DataStringHash *graph = GetNamedHash(hash_name);
  DataStringHash *unknown_hash = GetNamedHash(hash_unk);

  // get the last stage, which will contain all entries that could not
  // be placed in earlier stages. during construction it will contain
  // all entries that have not *yet* been placed in a stage.
  SortStageInfo &last_info = GetSort(sort_name, stage_count, true);
  Vector<DataString*> &key_list = last_info.entries;

  // buffer for storing key data.
  Buffer key;

  for (uint32_t stream = xdb->MinDataStream();
       stream <= xdb->MaxDataStream();
       stream++) {
    // reset the scratch buffer and get the next key.
    key.Reset();
    xdb->LookupKey(stream, &key);
    DataString *key_str = DataString::Make(key.base, key.pos - key.base);
    key_str->MoveRef(NULL, &sort_stages);
    key_list.PushBack(key_str);
  }

  // keys which are members of stages that have been finished.
  HashSet<DataString*,HashObject> stage_members;

  for (size_t stage = 0; stage < stage_count; stage++) {
    SortStageInfo &info = GetSort(sort_name, stage, true);

    // scan the keys which don't have a stage yet, and add to this stage
    // if all their outgoing edges are in a previous stage.
    size_t ind = 0;
    while (ind < key_list.Size()) {
      DataString *key = key_list[ind];
      Vector<DataString*> *entries = graph ? graph->Lookup(key, false) : NULL;

      bool missed = false;
      if (entries) {
        for (size_t eind = 0; eind < entries->Size(); eind++) {
          if (!stage_members.Lookup(entries->At(eind)))
            missed = true;
        }
      }

      if (unknown_hash && unknown_hash->Lookup(key, false)) {
        // this key and everything which reaches it will end up in
        // the last stage.
        missed = true;
      }

      if (missed) {
        // key must go in a later stage.
        ind++;
      }
      else {
        // no outgoing edges, add this key to the current stage.
        info.entries.PushBack(key);
        key_list[ind] = key_list.Back();
        key_list.PopBack();
      }
    }

    SortVector<DataString*,SortDataString>(&info.entries);

    for (ind = 0; ind < info.entries.Size(); ind++)
      stage_members.Insert(info.entries[ind]);
  }

  // the last stage now has all the entries we want.
  SortVector<DataString*,SortDataString>(&last_info.entries);

  Buffer name_buf;
  BufferOutStream name_out(&name_buf);
  name_out << (const char*) sort_name << ".sort";
  ofstream out(name_out.Base());

  for (size_t stage = 0; stage <= stage_count; stage++) {
    SortStageInfo &info = GetSort(sort_name, stage);

    for (size_t ind = 0; ind < info.entries.Size(); ind++) {
      DataString *node = info.entries[ind];
      if (ValidString(node->Value(), node->ValueLength()))
        out << (const char*) node->Value() << endl;
      else
        logout << "ERROR: Expected valid string in sort values" << endl;
    }

    if (stage != stage_count) {
      // blank lines are the separator between stages in the sort file.
      out << endl;
    }
  }
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

bool GraphSortHash(Transaction *t, const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, hash_unk, unk_length);
  BACKEND_ARG_STRING(2, db_name, db_length);
  BACKEND_ARG_STRING(3, sort_name, sort_length);
  BACKEND_ARG_INTEGER(4, stage_count);

  Backend_GraphSortHash(hash_name, hash_unk, db_name, sort_name, stage_count);
  return true;
}

bool GraphLoadSort(Transaction *t, const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, sort_name, sort_length);

  Buffer name_buf;
  BufferOutStream name_out(&name_buf);
  name_out << (const char*) sort_name << ".sort";
  ifstream in(name_out.Base());

  if (in.IsError()) {
    // this is OK, just leave the sort empty.
    *result = new TOperandInteger(t, 0);
    return true;
  }

  Buffer file_buf;
  ReadInStream(in, &file_buf);

  Vector<char*> entry_names;
  SplitBufferStrings(&file_buf, '\n', &entry_names);

  size_t stage_count = 0;
  SortStageInfo *info = & GetSort(sort_name, 0, true);

  for (size_t eind = 0; eind < entry_names.Size(); eind++) {
    char *str = entry_names[eind];

    if (!*str) {
      // blank line, this is a separator for a new stage.
      stage_count++;
      info = & GetSort(sort_name, stage_count, true);
      continue;
    }

    DataString *node = DataString::Make((uint8_t*) str, strlen(str) + 1);

    node->MoveRef(NULL, &sort_stages);
    info->entries.PushBack(node);
  }

  *result = new TOperandInteger(t, stage_count);
  return true;
}

bool GraphPopSort(Transaction *t, const Vector<TOperand*> &arguments,
                  TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, sort_name, sort_length);
  BACKEND_ARG_INTEGER(1, stage);

  SortStageInfo &info = GetSort(sort_name, stage);

  if (info.entries.Empty()) {
    *result = new TOperandString(t, "");
  }
  else {
    DataString *node = info.entries.Back();
    size_t length = node->ValueLength();

    Buffer *buf = new Buffer(length);
    t->AddBuffer(buf);

    memcpy(buf->base, node->Value(), length);
    *result = new TOperandString(t, buf->base, length);

    node->DecRef(&sort_stages);
    info.entries.PopBack();
  }

  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend
/////////////////////////////////////////////////////////////////////

static void start_Graph()
{
  BACKEND_REGISTER(GraphSortHash);
  BACKEND_REGISTER(GraphLoadSort);
  BACKEND_REGISTER(GraphPopSort);
}

static void finish_Graph()
{
  BACKEND_IMPL::ClearSorts();
}

TransactionBackend backend_Graph(start_Graph, finish_Graph);

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* GraphTopoSortHash(Transaction *t,
                           const char *hash_name, const char *hash_unk,
                           const char *db_name,
                           const char *sort_name, size_t stage_count)
{
  BACKEND_CALL(GraphTopoSortHash, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(new TOperandString(t, hash_unk));
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(new TOperandString(t, sort_name));
  call->PushArgument(new TOperandInteger(t, stage_count));
  return call;
}

TAction* GraphLoadSort(Transaction *t,
                       const char *sort_name, size_t stage_count)
{
  BACKEND_CALL(GraphLoadSort, 0);
  call->PushArgument(new TOperandString(t, sort_name));
  call->PushArgument(new TOperandInteger(t, stage_count));
  return call;
}

TAction* GraphReverseSort(Transaction *t,
                          const char *sort_name, size_t stage,
                          size_t var_result)
{
  BACKEND_CALL(GraphReverseSort, var_result);
  call->PushArgument(new TOperandString(t, sort_name));
  call->PushArgument(new TOperandInteger(t, stage));
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
