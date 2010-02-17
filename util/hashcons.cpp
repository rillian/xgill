
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

#include <stdlib.h>
#include "hashcons.h"
#include "buffer.h"

NAMESPACE_XGILL_BEGIN

TrackAlloc g_alloc_HashCons("HashCons");
TrackAlloc g_alloc_HashObject("HashObject");

/////////////////////////////////////////////////////////////////////
// HashObject
/////////////////////////////////////////////////////////////////////

#ifdef DEBUG

// get any reference breakpoint from the environment.
static uint64_t GetReferenceBreakpoint()
{
  char *str = getenv("XGILL_REFERENCE");
  if (str)
    return (uint64_t) atoi(str);
  else
    return 0;
}

uint64_t HashObject::g_reference_breakpoint = GetReferenceBreakpoint();
uint64_t HashObject::g_reference_stamp = 0;

#endif // DEBUG

bool HashObject::g_delete_unused = true;

void HashObject::Dbp() const
{
  Print(cout);
  cout << endl << flush;
}

void HashObject::ToString(Buffer *buf) const
{
  BufferOutStream out(buf);
  Print(out);
  out << '\0';
}

bool HashObject::EqualsString(const char *str) const
{
  Buffer string_buf;
  ToString(&string_buf);
  return (strcmp(str, (const char*) string_buf.base) == 0);
}

struct __HashObject_List
{
  static HashObject**  GetNext(HashObject *o) { return &o->m_next; }
  static HashObject*** GetPPrev(HashObject *o) { return &o->m_pprev; }
};

void HashObject::HashInsert(HashObject ***ppend, size_t *pcount)
{
  Assert(m_ppend == NULL && m_pcount == NULL);

  m_ppend = ppend;
  LinkedListInsert<HashObject,__HashObject_List>(m_ppend, this);

  m_pcount = pcount;
  (*m_pcount)++;
}

void HashObject::HashRemove()
{
  Assert(m_ppend != NULL && m_pcount != NULL);

  LinkedListRemove<HashObject,__HashObject_List>(m_ppend, this);
  m_ppend = NULL;

  (*m_pcount)--;
  m_pcount = NULL;
}

#ifdef DEBUG

void HashObject::PrintRefStamps()
{
  Assert(m_refs == m_ref_sources.Size());

  logout << "stamps:" << endl;
  for (size_t rind = 0; rind < m_ref_sources.Size(); rind++)
    logout << "  " << m_ref_sources[rind].w
           << " (" << (void*) m_ref_sources[rind].v << ")" << endl;
}

uint64_t HashObject::MinRefStamp()
{
  // the oldest stamp will be the first in the list.
  Assert(!m_ref_sources.Empty());
  return m_ref_sources[0].w;
}

#endif // DEBUG

/////////////////////////////////////////////////////////////////////
// HashCons
/////////////////////////////////////////////////////////////////////

HashCons<HashObject> *g_hashcons_list;

void RegisterHashCons(HashCons<HashObject> *hash)
{
  // this function is called during static initialization so make
  // sure that g_hashcons_list is initialized before we do anything to it.
  static bool initialized_hashcons_list = false;
  if (!initialized_hashcons_list) {
    g_hashcons_list = NULL;
    initialized_hashcons_list = true;
  }

  hash->m_hash_next = g_hashcons_list;
  g_hashcons_list = hash;
}

bool g_simple_hash_cons_counts = false;
bool g_skip_hash_cons_counts = false;
bool g_printed_hash_cons = false;

void PrintHashConsRoots()
{
  Assert(!g_printed_hash_cons);

  // set deletion bit so that when refcounts on hash objects go to zero
  // the object is not deleted and removed from the hash. thus we can
  // safely iterate through the hashes and drop references without
  // the contents of the hash itself changing.
  HashObject::g_delete_unused = false;

  bool found_object = false;
  HashCons<HashObject> *hash;

  // drop references on every object still in a HashCons
  hash = g_hashcons_list;
  while (hash != NULL) {
    if (hash->Size() != 0) {
      found_object = true;
      hash->DropAllChildRefs();
    }
    hash = hash->m_hash_next;
  }

  if (found_object) {
    logout << "HashCons leaked objects:" << endl;
    uint64_t min_stamp = (uint64_t) -1;

    // print out all objects that still have at least one reference
    hash = g_hashcons_list;
    while (hash != NULL) {
      hash->PrintLiveObjects(min_stamp);
      hash = hash->m_hash_next;
    }

#ifdef DEBUG
    logout << "Minimum leaked stamp: " << min_stamp << endl;
#endif

    logout << endl;
  }
}

void PrintHashConsCounts()
{
  Assert(!g_printed_hash_cons);

  HashCons<HashObject> *hash;

  // accumulate the count of leaked objects from every HashCons.
  size_t count = 0;

  hash = g_hashcons_list;
  while (hash != NULL) {
    count += hash->Size();
    hash = hash->m_hash_next;
  }

  if (count)
    logout << "HashCons leaked objects: " << count << endl;
}

void PrintHashCons()
{
  // only print the HashCons objects once.
  static bool printed_hash_cons = false;
  Assert(!printed_hash_cons);
  printed_hash_cons = true;

  if (g_skip_hash_cons_counts)
    {} // do nothing
  else if (g_simple_hash_cons_counts)
    PrintHashConsCounts();
  else
    PrintHashConsRoots();
}

NAMESPACE_XGILL_END
