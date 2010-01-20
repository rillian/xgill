
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

#include "block.h"

NAMESPACE_XGILL_BEGIN

// trim away points and edges from cfg for which either there is no path from
// the entry point, or no path to the exit point. if flatten_skips is set,
// flattens away any Skip() edges from the final CFG. consumes a reference
// on new_id. TODO: this currently trims away paths that loop infinitely. fix?
void TrimUnreachable(BlockCFG *cfg, bool flatten_skips);

// reorganize the points in CFG so that their order forms a topological sort.
// the CFG should already have had unreachable points trimmed and skip
// edges followed.
void TopoSortCFG(BlockCFG *cfg);

// covert a possibly loop containing CFG for some function into a list
// of equivalent loop-free CFGs for the function and its various loops.
// also removes Skip edges from the result CFGs.
void SplitLoops(BlockCFG *cfg, Vector<BlockCFG*> *resultCFGs);

// counter associated with a loop block identifier.
struct LoopCounter {
  // holds a reference.
  BlockId *loop;

  // line for the head of this loop.
  uint32_t line;

  // count of this loop's position within its function.
  size_t counter;

  LoopCounter() : loop(NULL), line(0), counter(0) {}

  // comparison method to sort counters by line.
  static int Compare(const LoopCounter &c0, const LoopCounter &c1)
  {
    if (c0.line < c1.line) return -1;
    if (c0.line > c1.line) return 1;
    return 0;
  }
};

// compute an ordering for all the loops in function and store
// in counter_list. loops are ordered by their line numbers relative to one
// another, so that moving the function around in its file or
// inserting/removing non-loop code in the function will not affect
// the counters. if there are multiple loops defined on the same line
// (usually from a macro expansion) then they will have counter zero.
void GetLoopCounters(Variable *function, Vector<LoopCounter> *loop_list);

NAMESPACE_XGILL_END
