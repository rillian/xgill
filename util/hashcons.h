
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

#include "hashtable.h"
#include "hashcache.h"
#include "buffer.h"
#include "config.h"

NAMESPACE_XGILL_BEGIN

extern TrackAlloc g_alloc_HashObject;
extern TrackAlloc g_alloc_HashCons;

// analysis determinism

// a critically important property for analyses to maintain is determinism.
// running an analysis on the same input should always produce the same
// result. this is most important for debugging, but also reproducibility
// of warnings in general. the main way in which the program can become
// non-deterministic is if program values or execution order depend on
// the values of pointers. pointers are fungible and runs of the same
// program on the same input can assign different values and orderings to
// the result of the same allocations per the whim of the underlying libraries
// and operating system.

// the main way around this problem is instead of comparing pointers,
// compare hashes of their contents, which do not depend on the pointer
// itself and will be the same across separate executions of the program.

// hash-consing structures

// all references on a hash-consed object are held for a particular ORef.
// this ORef is supplied when the reference is acquired, and again when
// the reference is deleted, to ensure we know who holds all references
// at any time and where those references were acquired. this information
// is only accumulated if DEBUG is defined.
typedef const void* ORef;

// define to turn on cross-checking of hash values during serialization.
// #define SERIAL_CHECK_HASHES

// superclass of all objects which can be hash-consed or which
// can have references originating in a pool.
class HashObject
{
 public:
  // leaf HashObjects which can be inserted into a HashCons structure must
  // define two static methods with the following signatures. Compare
  // will only be called on HashObjects with identical Hash() values
  // (thus it will usually return 0). Compare must be deterministic.
  // see the macros TryCompareValues and TryCompareObjects below for use
  // in writing Compare() functions.

  // static int Compare(const HashObject *o1, const HashObject *o2);
  // static HashObject* Copy(const HashObject *o);

  // leaf HashObjects which can be serialized must define two static methods
  // with the following signatures. Read returns NULL on failure,
  // otherwise adds a reference to its return value in the same fashion
  // as HashCons.Lookup()

  // static HashObject* Read(Buffer *buf);
  // static void Write(Buffer *buf, const HashObject *o);

  // any HashObject can be used as the hash type in a HashTable.
  static inline uint32_t Hash(uint32_t hash, HashObject *v) {
    return v->Hash();
  }

 public:
  // construct a new HashObject. constructors for HashObjects that refer
  // to other HashObjects consume a single reference for each pointer to
  // that other HashObject. the sources of these references are typically
  // set to NULL before the constructor, and must not be modified by
  // the constructor itself (MoveRefs will instead be called). constructors
  // must fill in m_hash appropriately.
  HashObject()
    : m_next(NULL), m_pprev(NULL), m_refs(0), m_hash(0),
      m_ppend(NULL), m_pcount(NULL)
  {}

  // call DecMoveRef with the specified ov/nv on all of the references
  // which are held by this object and will be removed when it is deleted.
  // this is called after the constructor to either move the references
  // for a fresh object or remove the references for a duplicate object.
  virtual void DecMoveChildRefs(ORef ov, ORef nv) {}

  // add a reference to this object. v is the source which holds
  // the reference. the same v can hold multiple references.
  void IncRef(ORef v = NULL)
  {
    m_refs++;

#ifdef DEBUG
    InsertReferenceSource(v);
#endif
  }

  // remove a reference from this object. if the reference count goes to zero
  // then removes this object from its parent hash, removes references it
  // holds on its children, unpersists its stored data, and deletes it.
  // v must have been supplied as the source for a previous IncRef().
  void DecRef(ORef v = NULL)
  {
    if (m_refs == 0) {
      Breakpoint(this);
      Assert(false);
    }

#ifdef DEBUG
    RemoveReferenceSource(v);
#endif

    m_refs--;

    if (m_refs == 0 && g_delete_unused) {
      HashRemove();
      DecMoveChildRefs(this, NULL);
      UnPersist();
      delete this;
    }
  }

  // move a reference for this object from one source to another.
  void MoveRef(ORef ov, ORef nv)
  {
#ifdef DEBUG
    RemoveReferenceSource(ov);
    InsertReferenceSource(nv);
#endif
  }

  // move a reference for this object from one source to another,
  // unless the new source is NULL in which case the reference
  // will be removed entirely.
  void DecMoveRef(ORef ov, ORef nv)
  {
    if (nv)
      MoveRef(ov, nv);
    else
      DecRef(ov);
  }

  // get the number of references on this object. for debugging.
  size_t Refs() const { return m_refs; }

  // get the hash value of this object.
  uint32_t Hash() const { return m_hash; }

  // print info on this object to out.
  virtual void Print(OutStream &out) const = 0;

  // debugging print this object to standard out.
  void Dbp() const;

  // convert this object to a string (equal to the result of calling Print()),
  // and append that string to buf.
  void ToString(Buffer *buf) const;

  // whether this object equals the specified string. for debugging.
  bool EqualsString(const char *str) const;

  // ensure that all data referred to by this object is persisted in the heap.
  // local data referred to by this object should be reallocated and owned.
  // this is called when the object is inserted into a HashCons table.
  virtual void Persist() {}

  // undo any operations performed by Persist(). called before deletion.
  virtual void UnPersist() {}

  // put this onto the end of the linked list pointed to by *p_end,
  // in a HashCons with its object count at *pcount.
  void HashInsert(HashObject ***ppend, size_t *pcount);

  // remove this from the linked list which contains it.
  void HashRemove();

#ifdef DEBUG

  // print the stamps for each reference held on this object.
  void PrintRefStamps();

  // get the stamp for the earliest held reference on this object.
  uint64_t MinRefStamp();

#endif // DEBUG

  // linked entry in the containing HashCons, if there is one.
  HashObject *m_next, **m_pprev;

 protected:

  // number of references held on this object.
  size_t m_refs;

  // define extra fields which are present if DEBUG is defined.
  // it is usually a really bad idea to have the size of a structure depend
  // on a #define, as if that define appears in some compilation units but
  // not others, the size of the struct may vary across units leading to
  // very bad behavior (stack and heap corruption, etc.). in this case we
  // need this extra info to debug refcount leaks and extra decrements,
  // but it also more than doubles the size of the structure. just make sure
  // that DEBUG is either defined everywhere or defined nowhere.
  // currently DEBUG is controlled by the outer Makefile, and set
  // depending on whether this is 'make debug'.

#ifdef DEBUG

  // info about a reference held on this object.
  struct ObjectReference {
    ORef v;      // the void* pointer (or NULL) which acquired this reference.
    uint64_t w;  // stamp at which the reference was acquired.
    ObjectReference() : v(NULL), w(0) {}
    ObjectReference(ORef _v, uint64_t _w) : v(_v), w(_w) {}
  };

  // next unique ID for an acquired reference. each reference
  // has a new stamp which can be used with OBJECT_REFERENCE_BREAKPOINT
  // to break when that reference is acquired. stamps are assigned in the
  // order in which references are acquired so it is critically important
  // that the execution order of the analysis remains deterministic
  // (mostly that execution order does not depend on branching is not 
  static uint64_t g_reference_stamp;

  // reference acquire stamp to call Breakpoint() at, if there is one.
  static uint64_t g_reference_breakpoint;

  // the particular sources holding references on this object,
  // and the stamps at which those references were acquired.
  // this is maintained in the order in which references were acquired.
  // if the same source holds multiple references, removing a reference
  // on that source removes the most recent one.
  Vector<ObjectReference> m_ref_sources;

  void InsertReferenceSource(ORef v)
  {
    g_reference_stamp++;
    m_ref_sources.PushBack(ObjectReference(v, g_reference_stamp));

    if (g_reference_stamp == g_reference_breakpoint)
      Breakpoint();
  }

  void RemoveReferenceSource(ORef v)
  {
    bool found = false;
    for (ssize_t rind = m_ref_sources.Size() - 1; rind >= 0; rind--) {
      if (m_ref_sources[rind].v == v) {
        for (size_t xrind = rind; xrind < m_ref_sources.Size() - 1; xrind++)
          m_ref_sources[xrind] = m_ref_sources[xrind + 1];
        m_ref_sources.PopBack();

        found = true;
        break;
      }
    }
    if (!found) {
      Breakpoint(this);
      Assert(false);
    }
  }

#endif // DEBUG

  // hash value of this object. this is filled in by the constructor
  // of the leaf subclass. the hash value must be deterministic,
  // and additionally should (but doesn't have to) depend exclusively
  // on the contents of the object, so that the hash is the same
  // across any run of any program which constructs a value equal
  // to it per Compare().
  uint32_t m_hash;

  // pointer to the end pointer of the HashCons bucket containing this object.
  HashObject ***m_ppend;

  // pointer to the object count in the HashCons containing this object.
  // when this object is deleted it will decrement this count.
  size_t *m_pcount;

 public:

  // whether HashObjects with no more incoming references should drop
  // their own references and remove themselves from the parent hash.
  // this is true unless we are scanning for leaked objects at program exit.
  static bool g_delete_unused;

  ALLOC_OVERRIDE(g_alloc_HashObject);
};

// print hash objects directly to a stream.
inline OutStream& operator << (OutStream &out, const HashObject *o)
{
  if (o != NULL)
    o->Print(out);
  else
    out << "<null>";
  return out;
}

// macro for use in Compare() functions for HashObject subclasses.
// compare two non-pointer values.
#define TryCompareValues(V0, V1)                \
  do {                                          \
    int __cmp0 = (V0);                          \
    int __cmp1 = (V1);                          \
    int __cmp_diff = __cmp1 - __cmp0;           \
    if (__cmp_diff) return __cmp_diff;          \
  } while (0)

// comparison function for two HashObjects of the same type T.
// the objects must be resident in some HashCons structure.
template <class T>
inline int CompareObjects(const T *cmp0, const T *cmp1)
{
  if (cmp0 == cmp1)
    return 0;

  // because of hash-consing the two cannot be equal at this point.

  if (cmp0 == NULL)
    return -1;
  if (cmp1 == NULL)
    return 1;

  TryCompareValues(cmp0->Hash(), cmp1->Hash());

  // colliding hash values, do a deeper comparison
  int res = T::Compare(cmp0, cmp1);
  Assert(res != 0);
  return res;
}

// macro for use in Compare() functions for HashObject subclasses.
// compare two HashObject pointers, either of which may be NULL.
#define TryCompareObjects(V0, V1, T)                    \
  do {                                                  \
    const T *__cmp0 = (V0);                             \
    const T *__cmp1 = (V1);                             \
    int __cmp_diff = CompareObjects<T>(__cmp0, __cmp1); \
    if (__cmp_diff) return __cmp_diff;                  \
  } while (0)

template <class T>
struct SortObjectsCompare
{
  static int Compare(T *v0, T *v1) {
    return CompareObjects<T>(v0, v1);
  }
};

// sort a vector of HashObjects per the Compare method on the object,
// and remove any duplicate objects. one reference must be held by v
// for each element of pdata.
template <class T>
inline void SortObjectsRmDups(Vector<T*> *pdata, ORef v)
{
  SortVector< T*, SortObjectsCompare<T> >(pdata);

  size_t shift = 0;
  for (size_t ind = 1; ind < pdata->Size(); ind++) {
    if (pdata->At(ind - 1) == pdata->At(ind)) {
      pdata->At(ind)->DecRef(v);
      shift++;
    }
    else if (shift != 0) {
      pdata->At(ind - shift) = pdata->At(ind);
    }
  }

  while (shift != 0) {
    pdata->PopBack();
    shift--;
  }
}

// is the specified vector of objects sorted per SortObjectsRmDups?
template <class T>
inline bool IsSortedObjectsRmDups(const Vector<T*> &data)
{
  if (data.Size() <= 1)
    return true;

  for (size_t ind = 0; ind < data.Size() - 1; ind++) {
    int res = CompareObjects<T>(data[ind], data[ind + 1]);
    if (res >= 0)
      return false;
  }

  return true;
}

// add references on each object in a vector.
template <class T>
inline void IncRefVector(const Vector<T*> &data, ORef v = NULL)
{
  for (size_t ind = 0; ind < data.Size(); ind++)
    data[ind]->IncRef(v);
}

// drop references on each object in a vector.
template <class T>
inline void DecRefVector(const Vector<T*> &data, ORef v = NULL)
{
  for (size_t ind = 0; ind < data.Size(); ind++)
    data[ind]->DecRef(v);
}

// useful functions for downcasting in HashObject class hierarchies.
#define DOWNCAST_TYPE(TYPEPFX, KINDPFX, NAME)           \
  inline bool Is ## NAME () {                           \
    return Kind() == KINDPFX ## NAME;                   \
  }                                                     \
  inline TYPEPFX ## NAME * If ## NAME () {              \
    if (Kind() == KINDPFX ## NAME)                      \
      return (TYPEPFX ## NAME *) this;                  \
    else return NULL;                                   \
  }                                                     \
  inline const TYPEPFX ## NAME * If ## NAME () const {  \
    if (Kind() == KINDPFX ## NAME)                      \
      return (const TYPEPFX ## NAME *) this;            \
    else return NULL;                                   \
  }                                                     \
  inline TYPEPFX ## NAME * As ## NAME () {              \
    Assert(Kind() == KINDPFX ## NAME);                  \
    return (TYPEPFX ## NAME *) this;                    \
  }                                                     \
  inline const TYPEPFX ## NAME * As ## NAME () const {  \
    Assert(Kind() == KINDPFX ## NAME);                  \
    return (const TYPEPFX ## NAME *) this;              \
  }

// normally at program exit if there are non-empty hashcons caches
// then analysis will be performed to find a root set (those with incoming
// references besides those from other leaked objects), and print their
// representation and hash values. if counts are enabled then just a count
// the number of leaked objects will be printed, and if skip is enabled
// then nothing will be printed.

extern bool g_simple_hash_cons_counts;
inline void SimpleHashConsCounts() { g_simple_hash_cons_counts = true; }

extern bool g_skip_hash_cons_counts;
inline void SkipHashConsCounts() { g_skip_hash_cons_counts = true; }

// print all HashCons objects according to the above variables.
void PrintHashCons();

// reference to some set of objects. pointers to two objects within the same
// hash cons structure are equivalent iff they are the same pointer.
// the type on which this specializes is a subclass of HashObject;
// when the last reference on a member object goes away, the object is
// deleted from the set.
template <class T>
class HashCons
{
 public:
  // create a new table with the specified minimum bucket count.
  // will dynamically resize as the object count subsequently grows/shrinks.
  HashCons<T>(size_t min_bucket_count = 719);

  // return an object from this table equivalent to o. if there is already
  // an object then that existing object will be returned and the references
  // held by the new object dropped. if there is no existing object then a
  // new one will be created by copying o and and calling Persist() on it.
  // In either case, a reference will be added to the result;
  // the caller must ensure that this reference is eventually consumed,
  // either by calling DecRef() or passing the result to another
  // HashObject constructor. the object is passed by non-const reference
  // because we don't want an explicit constructor call as the argument here;
  // calls to lookup should be wrapped in a static ::Make() function.
  T* Lookup(T &o);

  // return whether the specified object is contained in this table.
  // for debugging.
  bool IsMember(const T *o);

  // get the number of objects in this table.
  size_t Size() { return m_object_count; }

  // for finding reference leaks, drop the child refs of the objects in the
  // hash, and print out the objects with at least one reference.
  void DropAllChildRefs();
  void PrintLiveObjects(uint64_t &min_stamp);

 private:
  // resize for a new bucket count
  void Resize(size_t bucket_count);

  // check the bucket vs. object counts and resize if appropriate.
  void CheckBucketCount()
  {
    // shrink or expand the table as necessary.
    // the number of objects has to change by 2x between resizes.
    if (m_bucket_count > m_min_bucket_count &&
        m_bucket_count > m_object_count * 4)
      Resize(m_bucket_count / 2);
    else if (m_bucket_count < m_object_count)
      Resize(m_bucket_count * 2 + 1);
  }

  // print all objects in this hashcons.
  void PrintObjects();

  struct HashBucket {
    // head and tail of the list of entries in this bucket.
    HashObject *e_begin, **e_pend;

    HashBucket();
    ALLOC_OVERRIDE(g_alloc_HashCons);
  };

  // buckets in this table.
  HashBucket *m_buckets;

  // number of buckets in this table.
  size_t m_bucket_count;

  // number of objects in this table.
  size_t m_object_count;

  // minimum bucket count the table will resize to.
  size_t m_min_bucket_count;

 public:
  // linked entry in a global list of all hashcons structures.
  HashCons<HashObject> *m_hash_next;

  ALLOC_OVERRIDE(g_alloc_HashCons);
};

#include "hashcons_impl.h"

NAMESPACE_XGILL_END
