
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

#pragma once

// special hash table structure used by Solver to map (T,frame) pairs to
// the underlying solver's declaration or expression for that value
// in that frame. also maintains a stack of these declarations and expressions
// corresponding to the underlying solver's incremental push/pop stack,
// such that when the underlying solver is popped, all the declarations
// and expressions added since the corresponding push are removed from the
// hash table, in time linear in the number of declarations and expressions
// being removed.

#include <util/hashtable.h>

NAMESPACE_XGILL_BEGIN

// visitor class for solver hash tables.
template <class T, class U>
class SolverHashTableVisitor
{
 public:
  virtual void Visit(FrameId frame, T *o, U v) = 0;
};

template <class T, class U>
class SolverHashTable
{
 public:
  SolverHashTable<T,U>(size_t min_bucket_count = 89);
  ~SolverHashTable<T,U>() { Clear(); }

  // gets the object associated with o in the specified frame. if there
  // is none and force is specified then a new entry will be created with 0
  // as the initial value of the result.
  U* Lookup(size_t frame, T *o, bool force);

  // visit every entry in this hash table with the specified visitor.
  void VisitEach(SolverHashTableVisitor<T,U> *visitor);

  // push a new context onto the hashtable's stack. any new lookups will go
  // onto this topmost context. lookups are allowed when there is no pushed
  // context, in which case they go into a base context which cannot be popped.
  void PushContext();

  // pop the topmost context from the stack and remove all table entries
  // associated with that context.
  void PopContext();

  // clears all entries from this table.
  void Clear();

  // whether this hash is empty.
  bool IsEmpty() const { return m_entry_count == 0; }

 private:
  // copy constructor and assignment not allowed.
  SolverHashTable<T,U>(const SolverHashTable<T,U>&);
  SolverHashTable<T,U>& operator = (const SolverHashTable<T,U>&);

  // resize for a new bucket count.
  void Resize(size_t bucket_count);

  // check the bucket vs. object counts and resize if appropriate.
  void CheckBucketCount()
  {
    // use same resizing technique as HashTable::CheckBucketCount.
    if (m_bucket_count > m_min_bucket_count &&
        m_bucket_count > m_entry_count * 4)
      Resize(m_bucket_count / 2);
    else if (m_bucket_count < m_entry_count)
      Resize(m_bucket_count * 2 + 1);
  }

  // individual entry for an association in this table.
  struct HashEntry {
    size_t frame;
    T *source;
    U value;

    // linked for the bucket's list of entries.
    HashEntry *next, **pprev;

    // linked for the context's list of entries.
    HashEntry *context_next;

    HashEntry()
      : frame(0), source(NULL), value((U)0),
        next(NULL), pprev(NULL), context_next(NULL)
    {}
  };

  struct HashBucket {
    // head and tail of the list of entries in this bucket.
    HashEntry *e_begin, **e_pend;

    HashBucket() {
      LinkedListInit<HashEntry>(&e_begin, &e_pend);
    }
  };

  // allocator used for entries in this table.
  TrackAlloc &m_alloc;

  // buckets in this table.
  HashBucket *m_buckets;

  // number of buckets in this table.
  size_t m_bucket_count;

  // number of entries in this table.
  size_t m_entry_count;

  // minimum bucket count this table will resize to.
  size_t m_min_bucket_count;

  // list of active contexts for this table. a context is the head of a
  // linked list for all the entries defined for the context.
  Vector<HashEntry*> m_contexts;

  struct __HashEntry_List
  {
    static HashEntry**  GetNext(HashEntry *o) { return &o->next; }
    static HashEntry*** GetPPrev(HashEntry *o) { return &o->pprev; }
  };
};

extern TrackAlloc g_alloc_SolverHashTable;

/////////////////////////////////////////////////////////////////////
// SolverHashTable implementation
/////////////////////////////////////////////////////////////////////

template <class T, class U>
SolverHashTable<T,U>::SolverHashTable(size_t min_bucket_count)
  : m_alloc(g_alloc_SolverHashTable),
    m_buckets(NULL), m_bucket_count(0), m_entry_count(0),
    m_min_bucket_count(min_bucket_count)
{
  // we're not allocating until the first lookup occurs.
  Assert(min_bucket_count != 0);
}

template <class T, class U>
U* SolverHashTable<T,U>::Lookup(size_t frame, T *o, bool force)
{
  if (m_bucket_count == 0) {
    Assert(m_buckets == NULL);
    Resize(m_min_bucket_count);
  }
  else {
    CheckBucketCount();
  }

  size_t ind = Hash32(frame, o->Hash()) % m_bucket_count;
  HashBucket *bucket = &m_buckets[ind];

  HashEntry *e = bucket->e_begin;
  while (e != NULL) {
    if (e->source == o && e->frame == frame)
      return &e->value;
    e = e->next;
  }

  if (force) {
    m_entry_count++;

    HashEntry *new_e = track_new_single<HashEntry>(m_alloc);
    new_e->source = o;
    new_e->frame = frame;

    o->IncRef(new_e);

    LinkedListInsert<HashEntry,__HashEntry_List>(&bucket->e_pend, new_e);

    // add this to the topmost context if there is one.
    if (!m_contexts.Empty()) {
      new_e->context_next = m_contexts.Back();
      m_contexts.Back() = new_e;
    }

    return &new_e->value;
  }
  else {
    return NULL;
  }
}

template <class T, class U>
void SolverHashTable<T,U>::VisitEach(SolverHashTableVisitor<T,U> *visitor)
{
  for (size_t ind = 0; ind < m_bucket_count; ind++) {
    HashEntry *entry = m_buckets[ind].e_begin;
    while (entry != NULL) {
      visitor->Visit(entry->frame, entry->source, entry->value);
      entry = entry->next;
    }
  }
}

template <class T, class U>
void SolverHashTable<T,U>::PushContext()
{
  m_contexts.PushBack(NULL);
}

template <class T, class U>
void SolverHashTable<T,U>::PopContext()
{
  HashEntry *context = m_contexts.Back();
  m_contexts.PopBack();

  // remove all entries in the list indicated by context.

  while (context != NULL) {
    HashEntry *remove = context;
    context = context->context_next;

    remove->source->DecRef(remove);

    // find the bucket the entry is in and disconnect the entry.
    size_t ind = Hash32(remove->frame, remove->source->Hash()) % m_bucket_count;
    HashBucket *bucket = &m_buckets[ind];

    LinkedListRemove<HashEntry,__HashEntry_List>(&bucket->e_pend, remove);

    track_delete_single<HashEntry>(m_alloc, remove);
    m_entry_count--;
  }
}

template <class T, class U>
void SolverHashTable<T,U>::Clear()
{
  for (size_t ind = 0; ind < m_bucket_count; ind++) {
    HashBucket *bucket = &m_buckets[ind];

    while (bucket->e_begin != NULL) {
      HashEntry *e = bucket->e_begin;
      e->source->DecRef(e);

      LinkedListRemove<HashEntry,__HashEntry_List>(&bucket->e_pend, e);

      track_delete_single<HashEntry>(m_alloc, e);
      m_entry_count--;
    }
  }

  if (m_buckets != NULL) {
    track_delete<HashBucket>(m_alloc, m_buckets);
    m_buckets = NULL;
  }

  Assert(m_entry_count == 0);
  m_bucket_count = 0;
  m_contexts.Clear();
}

template <class T, class U>
void SolverHashTable<T,U>::Resize(size_t bucket_count)
{
  Assert(bucket_count >= m_min_bucket_count);
  HashBucket *buckets = track_new<HashBucket>(m_alloc, bucket_count);

  for (size_t ind = 0; ind < m_bucket_count; ind++) {
    HashBucket *bucket = &m_buckets[ind];

    while (bucket->e_begin != NULL) {
      HashEntry *e = bucket->e_begin;
      LinkedListRemove<HashEntry,__HashEntry_List>(&bucket->e_pend, e);

      size_t nind = Hash32(e->frame, e->source->Hash()) % bucket_count;
      HashBucket *nbucket = &buckets[nind];
      LinkedListInsert<HashEntry,__HashEntry_List>(&nbucket->e_pend, e);
    }
  }

  if (m_buckets != NULL)
    track_delete<HashBucket>(m_alloc, m_buckets);

  m_buckets = buckets;
  m_bucket_count = bucket_count;
}

NAMESPACE_XGILL_END
