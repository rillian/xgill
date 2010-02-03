
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

#include "backend.h"

NAMESPACE_XGILL_BEGIN

extern TransactionBackend backend_Graph;

// the graph backend is used to sort hashes which represent graphs
// (i.e. callgraphs). these sorts allow analyses to process functions in
// a bottom up fashion while a) being deterministic even if many cores are
// performing the analysis, and b) being robust against small changes in
// the input graph.

// to this end, the produced sorts do not keep a per-node ordering but
// separate the nodes into discrete stages. nodes within each stage are sorted
// by their name.
// stage 0: nodes with no outgoing edges
// stage 1: nodes whose outgoing edges are all in stage 0
// stage 2: nodes whose outgoing edges are all in stage 0 or 1
// ...
// stage last: all remaining nodes. also includes nodes from a second 'unknown'
//             hash whose outgoing edges are not known (i.e. indirect calls).

// access to the graph functions for sorting/storing a hash.
BACKEND_IMPL_BEGIN

void Backend_GraphSortHash(const uint8_t *hash_name, const uint8_t *hash_unk,
                           const uint8_t *db_name,
                           const uint8_t *sort_name, size_t stage_count);

BACKEND_IMPL_END

NAMESPACE_BEGIN(Backend)

// topo sort the graph specified by the entries in hash_name,
// storing the result in sort_name. entries in hash_name represent keys
// in the database db_name; the resulting sort will have exactly one entry
// for each key in db_name. stores the resulting sort to disk.
TAction* GraphSortHash(Transaction *t,
                       const char *hash_name, const char *hash_unk,
                       const char *db_name,
                       const char *sort_name, size_t stage_count);

// load a sort from disk, does not overwrite any existing sort.
// returns the number of intermediate stages which were read in.
// valid stage values for GraphPopSort are [0, stage_count].
TAction* GraphLoadSort(Transaction *t,
                       const char *sort_name, size_t var_result);

// get a list with all entries in the specified stage of the sort.
TAction* GraphSortKeys(Transaction *t,
                       const char *sort_name, TOperand *stage,
                       size_t var_result);

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
