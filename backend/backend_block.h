
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

// backend for writing out block data structures during the frontend parse.

#include "backend.h"

NAMESPACE_XGILL_BEGIN

// files to mark as changed if we are doing an incremental build.
extern ConfigOption option_incremental_files;

NAMESPACE_BEGIN(Backend)

// determine which in a list of CSUs and/or blocks needs to be processed.
// query_data is a compressed series of Names and/or BlockIds,
// and result receives the subset of that list which needs to be processed.
TAction* BlockQueryList(Transaction *t, TOperand *query_data,
                        size_t var_result);

// add the results of processing a set of CSUs and/or blocks. list is
// a compressed series of CompositeCSUs, TAG_Uint32 followed by BlockCFGs,
// EscapeEdgeSets, EscapeAccessSets, and CallEdgeSets.
// these will be merged with any previous data.
TAction* BlockWriteList(Transaction *t, TOperand *write_data);

// determine whether an annotation CFG has already been processed.
TAction* BlockQueryAnnot(Transaction *t, const char *db_name,
                         const char *var_name, const char *annot_name,
                         size_t var_result);

// write out an annotation CFG stored in annot_data.
TAction* BlockWriteAnnot(Transaction *t, TOperand *annot_data);

// flush all data that has been written to the databases. this is also
// performed when the backend finishes.
TAction* BlockFlush(Transaction *t);

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
