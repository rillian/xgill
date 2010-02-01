
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

// limit on how big we will let the merge scratch buffers grow before
// reallocating them to a smaller size. it is important to keep these
// relatively small as there may be several of them (though a fixed small
// number) and they are only needed occasionally.
#define MERGE_SCRATCH_BUF_LIMIT  (4 * 1048576)

/////////////////////////////////////////////////////////////////////
// MergeExternalLookup
/////////////////////////////////////////////////////////////////////

template <class T, class U, class V>
MergeExternalLookup<T,U,V>::MergeExternalLookup(const char *db_name)
  : m_db_name(db_name), m_enabled_timestamps(false),
    m_data_list_result(0), m_success_list_result(0),
    m_scratch_old("Buffer_merge_old"),
    m_scratch_new("Buffer_merge_new")
{}

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::LookupInsert(
                                   MergeExternalLookup<T,U,V>::Cache *cache,
                                   T *v)
{
  MergeExternalData<T,U,V> *empty_data = new MergeExternalData<T,U,V>();
  v->IncRef(empty_data);
  empty_data->key = v;

  cache->Insert(v, empty_data);
}

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::Remove(
                                   MergeExternalLookup<T,U,V>::Cache *cache,
                                   T *v, MergeExternalData<T,U,V> *data)
{
  // remember the objects being removed from the cache so we can write them
  // back to the underlying database later.
  m_flush_data.PushBack(data);
}

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::ReadKeys(Transaction *t)
{
  if (m_flush_data.Empty())
    return;

  // make sure timestamps are enabled on the database, so we will
  // know if a write occurs after this read transaction. we only need
  // to do this once.
  if (!m_enabled_timestamps) {
    m_enabled_timestamps = true;
    t->PushAction(Backend::XdbEnableTimeStamps(t, m_db_name));
  }

  // operands to store in the data list result.
  Vector<TOperand*> data_list_args;

  for (size_t dind = 0; dind < m_flush_data.Size(); dind++) {
    MergeExternalData<T,U,V> *data = m_flush_data[dind];

    // get a variable to hold the result of the database lookup.
    size_t data_var = t->MakeVariable();
    TOperand *data_arg = new TOperandVariable(t, data_var);
    data_list_args.PushBack(data_arg);

    TOperand *key_arg = new TOperandString(t, GetDatabaseKey(data->key));
    t->PushAction(Backend::XdbLookup(t, m_db_name, key_arg, data_var));
  }

  // make and return the list of all database entries.
  m_data_list_result = t->MakeVariable(true);
  t->PushAction(Backend::ListCreate(t, data_list_args, m_data_list_result));
}

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::WriteKeys(Transaction *nt, Transaction *t)
{
  if (m_flush_data.Empty())
    return;

  TOperandList *data_list = t->LookupList(m_data_list_result);
  Assert(data_list && data_list->GetCount() == m_flush_data.Size());

  // list of operands to store in the success list result.
  Vector<TOperand*> succ_list_args;

  // get the timestamp used by the read transaction. we can only commit
  // changes on database entries whose timestamp comes before this.
  TimeStamp stamp = t->GetTimeStamp();

  for (size_t ind = 0; ind < m_flush_data.Size(); ind++) {
    MergeExternalData<T,U,V> *data = m_flush_data[ind];

    TOperandString *data_arg = data_list->GetOperand(ind)->AsString();
    if (data_arg->GetDataLength() != 0)
      TOperandString::Uncompress(data_arg, &m_scratch_old);

    Buffer read_buf(m_scratch_old.base,
                    m_scratch_old.pos - m_scratch_old.base);
    MergeData(data, &read_buf, &m_scratch_new);

    TOperand *key_arg = new TOperandString(nt, GetDatabaseKey(data->key));
    TOperand *merge_arg = TOperandString::Compress(nt, &m_scratch_new);

    size_t cmp_var = 0;
    nt->PushAction(
      Backend::Compound::XdbReplaceTry(nt, m_db_name, key_arg, merge_arg,
                                       stamp, NULL, &cmp_var));

    TOperand *cmp_arg = new TOperandVariable(nt, cmp_var);
    succ_list_args.PushBack(cmp_arg);

    m_scratch_old.Reset();
    m_scratch_new.Reset();
  }

  // make and return the list of all success information.
  m_success_list_result = nt->MakeVariable(true);
  nt->PushAction(Backend::ListCreate(nt, succ_list_args,
                                       m_success_list_result));

  // reset the scratch buffers to a smaller size if necessary.
  if (m_scratch_old.size > MERGE_SCRATCH_BUF_LIMIT)
    m_scratch_old.Reset(MERGE_SCRATCH_BUF_LIMIT);
  if (m_scratch_new.size > MERGE_SCRATCH_BUF_LIMIT)
    m_scratch_new.Reset(MERGE_SCRATCH_BUF_LIMIT);
}

template <class T, class U, class V>
class DecRef_MergeExternalData : public HashTableVisitor<U*,V*>
{
 public:
  MergeExternalData<T,U,V> *data;
  DecRef_MergeExternalData(MergeExternalData<T,U,V> *_data) : data(_data) {}

  void Visit(U*&, Vector<V*> &values) {
    Assert(values.Size() == 1);
    values[0]->DecRef(data);
  }
};

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::CheckWrite(
                                   Transaction *nt,
                                   MergeExternalLookup<T,U,V>::Cache *cache)
{
  Assert(cache->GetExternalLookup() == this);

  if (m_flush_data.Empty())
    return;

  TOperandList *success_list = nt->LookupList(m_success_list_result);
  Assert(success_list && success_list->GetCount() == m_flush_data.Size());

  for (size_t ind = 0; ind < m_flush_data.Size(); ind++) {
    MergeExternalData<T,U,V> *data = m_flush_data[ind];

    TOperandBoolean *bool_arg = success_list->GetOperand(ind)->AsBoolean();
    if (bool_arg->IsTrue()) {
      // successfully inserted the data. clear out the new data.

      data->key->DecRef(data);
      if (data->single)
        data->single->DecRef(data);
      if (data->map) {
        DecRef_MergeExternalData<T,U,V> visitor(data);
        data->map->VisitEach(&visitor);
        delete data->map;
      }

      delete data;
    }
    else {
      // some other worker updated the database between our read and
      // write transaction. add the new data back into the cache
      // and try again later.

      cache->Insert(data->key, data);
    }
  }

  // reset the flushed data and other state.
  m_flush_data.Clear();
  m_data_list_result = 0;
  m_success_list_result = 0;
}

template <class T, class U, class V>
V* MergeExternalLookup<T,U,V>::LookupSingle(
                                 MergeExternalLookup<T,U,V>::Cache *cache,
                                 T *database_key, U *object_key, bool force)
{
  if (!force) {
    // return NULL if there is no member in the cache.
    if (!cache->IsMember(database_key))
      return NULL;
  }

  MergeExternalData<T,U,V> *data = cache->Lookup(database_key);
  cache->Release(database_key);

  if (data->single == NULL && data->map == NULL) {
    // zero objects in this data, make an empty one.
    Assert(force);
    data->single = MakeEmpty(object_key);
    data->single->MoveRef(NULL, data);
    return data->single;
  }

  if (data->single != NULL) {
    Assert(data->map == NULL);
    if (GetObjectKey(data->single) == object_key) {
      return data->single;
    }
    else if (!force) {
      return NULL;
    }
    else {
      // make the map and insert the old object.
      data->map = new HashTable<U*,V*,HashObject>(19);
      data->map->Insert(GetObjectKey(data->single), data->single);
      data->single = NULL;

      // fall through and we will insert an entry for the new object.
    }
  }

  Assert(data->map != NULL);

  Vector<V*> *entries = data->map->Lookup(object_key, true);
  if (entries->Empty()) {
    if (force) {
      V *empty_object = MakeEmpty(object_key);
      entries->PushBack(empty_object);
      empty_object->MoveRef(NULL, data);
      return empty_object;
    }
    else {
      return NULL;
    }
  }
  else {
    return entries->At(0);
  }
}

// a pointer is marked iff its low bit is set. to get the base pointer
// unset this low bit.

template <class T>
inline bool PointerMarked(T *v)
{
  return (((uint64_t)v) & (uint64_t) 0x1) != 0;
}

template <class T>
inline T* MarkPointer(T *v)
{
  return (T*) (((uint64_t)v) | (uint64_t) 0x1);
}

template <class T>
inline T* UnmarkPointer(T *v)
{
  return (T*) (((uint64_t)v) & ~ (uint64_t) 0x1);
}

template <class T, class U, class V>
V* MergeExternalLookup<T,U,V>::LookupMarkData(
                                 MergeExternalData<T,U,V> *new_data,
                                 U *object_key)
{
  if (new_data->single) {
    if (GetObjectKey(new_data->single) == object_key) {
      V *res = new_data->single;
      Assert(!PointerMarked<V>(res));
      new_data->single = MarkPointer<V>(res);
      return res;
    }
    else {
      return NULL;
    }
  }

  if (new_data->map) {
    Vector<V*> *entries = new_data->map->Lookup(object_key);
    if (entries) {
      Assert(entries->Size() == 1);
      V *res = entries->At(0);
      Assert(!PointerMarked<V>(res));
      entries->At(0) = MarkPointer<V>(res);
      return res;
    }
    else {
      return NULL;
    }
  }

  // the new data should contain at least one entry.
  Assert(false);
  return NULL;
}

template <class T, class U, class V>
class Unmark_MergeExternalData : public HashTableVisitor<U*,V*>
{
 public:
  Vector<V*> *unmarked_entries;
  Unmark_MergeExternalData(Vector<V*> *_unmarked_entries)
    : unmarked_entries(_unmarked_entries) {}

  void Visit(U*&, Vector<V*> &values) {
    Assert(values.Size() == 1);
    if (PointerMarked<V>(values[0])) {
      values[0] = UnmarkPointer<V>(values[0]);
    }
    else {
      unmarked_entries->PushBack(values[0]);
    }
  }
};

template <class T, class U, class V>
void MergeExternalLookup<T,U,V>::GetUnmarkedData(
                                   MergeExternalData<T,U,V> *new_data,
                                   Vector<V*> *unmarked_entries)
{
  if (new_data->single) {
    if (PointerMarked<V>(new_data->single)) {
      new_data->single = UnmarkPointer<V>(new_data->single);
    }
    else {
      unmarked_entries->PushBack(new_data->single);
    }
  }

  if (new_data->map) {
    Unmark_MergeExternalData<T,U,V> visitor(unmarked_entries);
    new_data->map->VisitEach(&visitor);
  }
}

#undef MERGE_SCRATCH_BUF_LIMIT
