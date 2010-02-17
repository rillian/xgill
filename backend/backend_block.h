
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

// backend for frontend functionality. this does two main things:
// - manages and writes the block data structures during the frontend parse.
// - keeps track of the worklist during backend analysis.

#include "backend.h"

NAMESPACE_XGILL_BEGIN

// files to mark as changed if we are doing an incremental build.
extern ConfigOption option_incremental_files;

// hash for adding items to process in the next stage, see functions below.
#define BLOCK_WORKLIST_NEXT "worklist_next"

NAMESPACE_BEGIN(Backend)

// construction functions.

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

// return whether an annotation CFG has already been processed.
TAction* BlockQueryAnnot(Transaction *t, const char *db_name,
                         const char *var_name, const char *annot_name,
                         size_t var_result);

// write out an annotation CFG stored in annot_data.
TAction* BlockWriteAnnot(Transaction *t, TOperand *annot_data);

// flush all escape/callgraph data that has been written to the databases.
// this is also performed when the backend finishes.
TAction* BlockFlush(Transaction *t);

// file source functions.

// return whether the source/preprocessed contents of a file have already
// been stored in this run.
TAction* BlockQueryFile(Transaction *t, const char *file, size_t var_result);

// write out the source/preprocessed contents of a previously queried file.
// this keeps track of files which have changed since a previous run for
// incremental analysis.
TAction* BlockWriteFile(Transaction *t, const char *file,
                        TOperand *source_data, TOperand *preproc_data);

// worklist functions.

// setup the callgraph worklist and return the count of stages.
TAction* BlockLoadWorklist(Transaction *t, size_t var_result);

// setup the callgraph worklist for a specific list of functions.
TAction* BlockSeedWorklist(Transaction *t, TOperandList *functions);

// get the current stage of the worklist. if this is in [0,stage_count]
// then functions are being popped from the callgraph sort and being processed
// for the first time. for stages after stage_count, functions added to the
// BLOCK_WORKLIST_NEXT hash during the previous stage will be processed.
TAction* BlockCurrentStage(Transaction *t, size_t var_result);

// two barriers are used to manage the transition from one stage to the next.
//
// - the process barrier is a count of the number of workers that have
// processed at least one function but have not finished with processing
// all functions in the stage yet. no worker should write out the results
// of the stage until the process barrier is clear.
//
// - the write barrier is a count of the number of workers that have
// finished with processing the stage but have not finished writing out their
// results yet. the next stage can't start until the write barrier is clear.

// pop a function from the worklist and store its name in var_result.
// the string will be empty if the worklist for the current stage is drained.
// if have_barrier_process is false and a function was popped then the process
// barrier will be incremented. if the worklist is empty and both barriers
// are clear, advances the stage.
TAction* BlockPopWorklist(Transaction *t, bool have_barrier_process,
                          size_t var_result);

// returns whether the process/write barriers are non-zero.
TAction* BlockHaveBarrierProcess(Transaction *t, size_t var_result);
TAction* BlockHaveBarrierWrite(Transaction *t, size_t var_result);

// shifts a count on the process barrier to a count on the write barrier.
TAction* BlockShiftBarrierProcess(Transaction *t);

// drops a count on the write barrier.
TAction* BlockDropBarrierWrite(Transaction *t);

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
