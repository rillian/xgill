
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

#include <util/assert.h>
#include <stdint.h>

NAMESPACE_XGILL_BEGIN

// every persistent transaction performed is associated with a unique
// timestamp, a 64-bit quantity which monotonically increases as the
// analysis proceeds.  timestamps indicate both the order in which
// updates occurred, and the relative real times at which they occurred.
// the high 48 bits indicate the # of seconds since the analysis started
// at which an update occurred, while the low 16 bits distinguish updates
// that occurred within each second. (if more than 2^16 updates occur
// in any one second, the current second will be bumped prematurely)

typedef uint64_t TimeStamp;
typedef uint64_t TimeSeconds;

// get the real time # of seconds since analysis start of a stamp
inline TimeSeconds TimeStampToSeconds(TimeStamp stamp)
{
  return stamp >> 16;
}

// get the first timestamp used for an update at a # of seconds into analysis
inline TimeStamp TimeSecondsToStamp(TimeSeconds seconds)
{
  return seconds << 16;
}

// initialize timestamp information
void InitializeTimeStamp();

// get the next timestamp. returns a new value each time it is called.
// this timestamp will never be zero.
TimeStamp AdvanceTimeStamp();

NAMESPACE_XGILL_END
