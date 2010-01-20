
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

// access to the graph functions for sorting a hash and storing the sort,
// for use by the block backend.
BACKEND_IMPL_BEGIN

void Backend_GraphTopoSortHash(const uint8_t *hash_name,
                               const uint8_t *sort_name);
void Backend_GraphStoreSort(const uint8_t *sort_name,
                            const uint8_t *file_name);

BACKEND_IMPL_END

NAMESPACE_BEGIN(Backend)

TAction* GraphSortExists(Transaction *t,
                         const char *sort_name,
                         size_t var_result);

// topo sort the graph specified by the entries in hash_name,
// storing the result in sort_name. the smallest entries in
// the sort are those towards the roots.
TAction* GraphTopoSortHash(Transaction *t,
                           const char *hash_name,
                           const char *sort_name);

// store the specified sort in the specified file.
TAction* GraphStoreSort(Transaction *t,
                        const char *sort_name,
                        const char *file_name);

// load a sort from the specified file.
TAction* GraphLoadSort(Transaction *t,
                       const char *sort_name,
                       const char *file_name);

// reverse the order of the specified sort.
TAction* GraphReverseSort(Transaction *t,
                          const char *sort_name);

// return the maximum element in the specified sort.
TAction* GraphGetMaxSort(Transaction *t,
                         const char *sort_name,
                         size_t var_result);

// remove the maximum element in the specified sort.
TAction* GraphRemoveMaxSort(Transaction *t,
                            const char *sort_name);

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
