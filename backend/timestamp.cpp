
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

#include "timestamp.h"
#include <util/assert.h>
#include <time.h>

NAMESPACE_XGILL_BEGIN

// implementation is based on time() function, which gives a 32-bit
// seconds value so we are only using 48 bits of the TimeStamp type.
// this still gives us enough seconds for 

static TimeStamp current_stamp = 0;
static time_t current_second = 0;

void InitializeTimeStamp()
{
  current_stamp = 1;
  current_second = time(NULL);
}

TimeStamp AdvanceTimeStamp()
{
  // make sure we called InitializeTimeStamp()
  Assert(current_stamp != 0);

  time_t second = time(NULL);

  // this assertion might trip sometime in 2038
  Assert(second >= current_second);

  if (second > current_second) {
    TimeSeconds base_seconds = TimeStampToSeconds(current_stamp);
    TimeSeconds next_seconds = base_seconds + (second - current_second);
    current_stamp = TimeSecondsToStamp(next_seconds);
    current_second = second;
  }
  else {
    current_stamp++;
  }

  return current_stamp;
}

NAMESPACE_XGILL_END
