
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

// general purpose compound actions composed from other backend functions.

#include "backend_util.h"
#include "backend_xdb.h"
#include "backend_hash.h"
#include "backend_graph.h"

NAMESPACE_XGILL_BEGIN

NAMESPACE_BEGIN(Backend)
NAMESPACE_BEGIN(Compound)

// HashCreateXdbKeys creates a hash if it does not exist whose values
// are all the keys in a specified database. this is normally used
// at startup for initializing worklist hashes.

TAction* HashCreateXdbKeys(Transaction *t,
                           const char *hash_name,
                           const char *db_name);

// HashPopXdbKey removes an arbitrary element from a hash,
// and returns that element along with its contents
// in the specified database. this is normally used for removing items
// from worklists whose keys are keys in a database, i.e. those
// initialized with HashCreateXdbKeys. HashPopXdbKeyWithSort is
// similar, taking an extra sort whose maximum element will be picked
// if it is non-empty.

TAction* HashPopXdbKey(Transaction *t,
                       const char *hash_name,
                       const char *db_name,
                       size_t key_result,
                       size_t value_result);

TAction* HashPopXdbKeyWithSort(Transaction *t,
                               const char *sort_name,
                               const char *hash_name,
                               const char *db_name,
                               size_t key_result,
                               size_t value_result);

// XdbReplaceConditional is useful for updating databases for which
// entries might be written in multiple places, but contains few
// hot entries written in lots of places. replace the specified key
// with the specified value unless it has changed since $rstamp,
// in which case the new value of that entry should be returned
// (after which the client will redo the merge and resubmit).

TAction* XdbReplaceConditional(Transaction *t,
                               const char *db_name,
                               TOperand *key,
                               TOperand *value,
                               TimeStamp rstamp,
                               TAction *succeed_action,
                               size_t new_value_result);

// XdbReplaceTry should be used for database entries where
// there is only a single client which could write to the entry
// (e.g. the summary for a function can be written only by
// analyzing that function), but dependencies may cause the
// entry to be reanalyzed. do the replace only if the entry
// has not changed since $rstamp. otherwise discard the change.

TAction* XdbReplaceTry(Transaction *t,
                       const char *db_name,
                       TOperand *key,
                       TOperand *value,
                       TimeStamp rstamp,
                       TAction *succeed_action,
                       size_t *pcmp_var = NULL);

// dependencies are cases where when an entry in a bdb database
// is modified then particular items should go back onto a worklist
// for reanalysis.  A hash $depname is used to track the dependencies
// for a database, and a second hash $workname is a set of worklist items.
// XdbLookupDependency is used on database reads that should introduce
// a dependency, and UpdateDependency is used as the $succeed_action
// for database updates that can have dependencies.

TAction* XdbLookupDependency(Transaction *t,
                             const char *db_name,
                             TOperand *key,
                             const char *depname,
                             TOperand *workval,
                             size_t value_result);

TAction* UpdateDependency(Transaction *t,
                          const char *depname,
                          TOperand *key,
                          const char *workname);

// simpler utilitarian compound actions

// run action if the specified hash is empty.
TAction* HashRunIfEmpty(Transaction *t,
                        const char *hash_name,
                        TAction *action);

// clear the database if the specified hash does not exist.
TAction* XdbClearIfNotHash(Transaction *t,
                           const char *db_name,
                           const char *hash_name);

NAMESPACE_END(Compound)
NAMESPACE_END(Backend)

// looks up the specified key in the specified database, returning true
// if the key exists, false otherwise. if the key exists, stores in buf
// the uncompressed contents of the entry.
bool DoLookupTransaction(const char *db_name,
                         const char *key_name,
                         Buffer *buf);

NAMESPACE_XGILL_END
