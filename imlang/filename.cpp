
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

#include "filename.h"
#include "storage.h"
#include <backend/backend_xdb.h>

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// File normalization
/////////////////////////////////////////////////////////////////////

static const char *g_working_directory = NULL;
static const char *g_base_directory = NULL;

void SetWorkingDirectory(const char *path)
{
  Assert(!g_working_directory);
  g_working_directory = path;
}

void SetBaseDirectory(const char *path)
{
  Assert(!g_base_directory);

  char *new_path = strdup(path);
  int len = strlen(new_path);

  // cleanup any trailing '/' in the base directory.
  if (len > 0 && new_path[len - 1] == '/')
    new_path[len - 1] = 0;

  g_base_directory = new_path;
}

// remove uses of '.' from a path (except any leading '.').
static void CleanupPathDot(char *pos)
{
  char *old = pos;
  while (true) {
    pos = strchr(pos, '/');
    if (pos == NULL)
      break;

    if (pos[1] == '.' && pos[2] == '/') {
      strcpy(pos, &pos[2]);
      pos = old;
    }
    else {
      pos++;
    }
  }
}

// remove uses of '..' from a path (except any leading '..').
static void CleanupPathDotDot(char *pos)
{
  char *old = pos;
  while (true) {
    pos = strchr(pos, '/');
    if (pos == NULL)
      break;

    char *next_pos = strchr(pos + 1, '/');
    if (next_pos == NULL)
      break;;

    if (next_pos[1] == '.' && next_pos[2] == '.' && next_pos[3] == '/') {
      strcpy(pos, &next_pos[3]);
      pos = old;
    }
    else {
      pos++;
    }
  }
}

char* NormalizeFile(const char *file)
{
  Assert(g_working_directory && g_base_directory);

  static Buffer normal_buf;
  normal_buf.Reset();

  if (file[0] == '/') {
    // the supplied filename is already an absolute path, use that.
    normal_buf.Append(file, strlen(file) + 1);
  }
  else if (file[0] == '<') {
    // special compiler name, use it (we won't have the original source).
    normal_buf.Append(file, strlen(file) + 1);
  }
  else {
    // relative path from the working directory.
    normal_buf.Append(g_working_directory, strlen(g_working_directory));
    normal_buf.Append("/", 1);
    normal_buf.Append(file, strlen(file) + 1);
  }

  CleanupPathDot((char*) normal_buf.base);
  CleanupPathDotDot((char*) normal_buf.base);

  // remove the base directory prefix, if possible.
  size_t base_len = strlen(g_base_directory);
  if (strncmp((char*)normal_buf.base, g_base_directory, base_len) == 0) {
    Assert(normal_buf.base[base_len] == '/');
    return (char*) normal_buf.base + base_len + 1;
  }

  return (char*) normal_buf.base;
}

/////////////////////////////////////////////////////////////////////
// Source processing
/////////////////////////////////////////////////////////////////////

// data read so far about a preprocessed file. the filename itself is the key
// this data is associated with.
struct FileData
{
  // contents of the preprocessed file, pieced together from all the places
  // a # line directive appeared for that file.
  Buffer *contents;

  // line number of the next line that will be added for this file.
  // this is equal to the number of '\n' in the contents buffer + 1.
  size_t cur_line;

  // receives transaction result indicating whether this file has previously
  // been added to the source/preprocessed databases.
  size_t processed_var;

  FileData()
    : contents(NULL), cur_line(0), processed_var(0)
  {}
};

typedef HashTable<String*,FileData,String> FileDataTable;

class QueryFileData : public HashTableVisitor<String*,FileData>
{
public:
  Transaction *t;

  QueryFileData(Transaction *_t)
    : t(_t)
  {}

  void Visit(String *&file, Vector<FileData> &data_list)
  {
    Assert(data_list.Size() == 1);

    size_t var = t->MakeVariable(true);
    data_list[0].processed_var = var;

    TOperand *key_arg = new TOperandString(t, file->Value());

    t->PushAction(
      Backend::HashIsMember(t, PROCESSED_FILES_HASH, key_arg, var));
    t->PushAction(
      Backend::HashInsertKey(t, PROCESSED_FILES_HASH, key_arg));
  }
};

// scratch buffer for reading file contents.
static Buffer scratch_buf;

class DumpFileData : public HashTableVisitor<String*,FileData>
{
public:
  Transaction *query;
  Transaction *t;
 
  DumpFileData(Transaction *_query, Transaction *_t)
    : query(_query), t(_t)
  {}

  void Visit(String *&file, Vector<FileData> &data_list)
  {
    Assert(data_list.Size() == 1);
    FileData data = data_list[0];

    TOperandBoolean *var_res = query->LookupBoolean(data.processed_var);
    if (!var_res->IsTrue()) {
      const char *file_name = file->Value();
      TOperand *key_arg = new TOperandString(t, file_name);

      // compress and store the preprocessed file contents.
      TOperandString *preproc_arg = TOperandString::Compress(t, data.contents);
      t->PushAction(
        Backend::XdbReplace(t, PREPROC_DATABASE, key_arg, preproc_arg));

      // don't look for the source for special compiler files.
      if (file_name[0] == '<')
        return;

      // reconstruct the absolute filename from the normalized name.
      Buffer abs_name;
      if (file_name[0] != '/') {
        abs_name.Append(g_base_directory, strlen(g_base_directory));
        abs_name.Append("/", 1);
      }
      abs_name.Append(file_name, strlen(file_name) + 1);

      const char *abs_cstr = (const char*) abs_name.base;

      // compress and store the source file contents.
      FileInStream file_in(abs_cstr);
      if (file_in.IsError()) {
        logout << "WARNING: Could not find source file: " << abs_cstr << endl;
      }
      else {
        ReadInStream(file_in, &scratch_buf);

        TOperandString *source_arg = TOperandString::Compress(t, &scratch_buf);
        t->PushAction(
          Backend::XdbReplace(t, SOURCE_DATABASE, key_arg, source_arg));

        scratch_buf.Reset();
      }
    }
  }
};

class ClearFileData : public HashTableVisitor<String*,FileData>
{
  void Visit(String *&file, Vector<FileData> &data_list)
  {
    file->DecRef(&data_list);

    Assert(data_list.Size() == 1);
    Assert(data_list[0].contents);
    delete data_list[0].contents;
  }
};

void ProcessPreprocessedFile(istream &in, const char *input_file)
{
  Assert(g_working_directory && g_base_directory);

  // read our entire input into a buffer.
  Buffer file_buf;
  ReadInStream(in, &file_buf);

  // table with contents read so far for 
  FileDataTable file_table;

  // name of the original file which was being parsed, from which we will
  // get whether this is a C or a C++ file.
  const char *base_file = NULL;

  char *pos = (char*) file_buf.base;

  FileData *cur_data = NULL;
  while (*pos) {

    if (*pos == '#' && *(pos+1) == ' ') {
      // found a preprocessor line directive.
      // currently we just parse lines of the format '# line "file" ...

      // eat the '#'
      pos++;

      char *end_pos = NULL;
      long line = strtol(pos, &end_pos, 10);
      Assert(end_pos);

      char *start_quote = strchr(end_pos, '"');
      Assert(start_quote);

      char *end_quote = strchr(start_quote + 1, '"');
      Assert(end_quote);

      char *end_line = strchr(pos, '\n');
      Assert(end_line && end_line > end_quote);

      if (base_file == NULL) {
        // just mark the first # line directive we see as the base file.
        base_file = start_quote + 1;
      }

      *end_quote = 0;
      String *file = String::Make(NormalizeFile(start_quote + 1));

      Vector<FileData> *entries = file_table.Lookup(file, true);
      if (entries->Empty()) {
        file->MoveRef(NULL, entries);
        entries->PushBack(FileData());

        cur_data = &entries->Back();
        cur_data->contents = new Buffer();
        cur_data->cur_line = line;
      }
      else {
        file->DecRef();
        Assert(entries->Size() == 1);

        cur_data = &entries->Back();
        while ((long) cur_data->cur_line < line) {
          cur_data->contents->Append("\n", 1);
          cur_data->cur_line++;
        }
      }

      pos = end_line + 1;
      continue;
    }

    if (cur_data == NULL) {
      // we can get here if the input is not actually preprocessed.
      // make data for the input file itself.
      Assert(input_file);
      String *file = String::Make(NormalizeFile(input_file));

      Vector<FileData> *entries = file_table.Lookup(file, true);
      Assert(entries->Empty());

      file->MoveRef(NULL, entries);
      entries->PushBack(FileData());

      cur_data = &entries->Back();
      cur_data->contents = new Buffer();
      cur_data->cur_line = 1;
    }

    char *end_line = strchr(pos, '\n');
    if (end_line == NULL) {
      cur_data->contents->Append(pos, strlen(pos));
      break;
    }

    cur_data->contents->Append(pos, end_line - pos + 1);
    cur_data->cur_line++;

    pos = end_line + 1;
  }

  // figure out which of the files we read need to be added to the dbs.
  Transaction *query = new Transaction();
  QueryFileData query_visit(query);
  file_table.VisitEach(&query_visit);
  SubmitTransaction(query);

  // add those files we found in the query.
  Transaction *dump = new Transaction();
  DumpFileData dump_visit(query, dump);
  file_table.VisitEach(&dump_visit);
  SubmitTransaction(dump);

  delete query;
  delete dump;

  ClearFileData clear_visit;
  file_table.VisitEach(&clear_visit);
}

NAMESPACE_XGILL_END
