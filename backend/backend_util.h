
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

// backend for basic utility functions on timestamps, lists, etc.

#include "backend.h"

NAMESPACE_XGILL_BEGIN

extern TransactionBackend backend_Util;

NAMESPACE_BEGIN(Backend)

// TimeStamp functions

// get a timestamp for N real-time seconds after (positive)
// or before (negative) the current transaction.
TAction* TimeStampDeltaSeconds(Transaction *t, ssize_t seconds,
                               size_t var_result);

TAction* TimeStampLess(Transaction *t,
                       TOperand *time0, TOperand *time1,
                       size_t var_result);

TAction* TimeStampLessEqual(Transaction *t,
                            TOperand *time0, TOperand *time1,
                            size_t var_result);

TAction* TimeStampGreater(Transaction *t,
                          TOperand *time0, TOperand *time1,
                          size_t var_result);

TAction* TimeStampGreaterEqual(Transaction *t,
                               TOperand *time0, TOperand *time1,
                               size_t var_result);

// String functions

// input must be a NULL-terminated string. returns whether its length is 0.
TAction* StringIsEmpty(Transaction *t, TOperand *str,
                       size_t var_result);

// List functions

// make an empty list
TAction* ListEmpty(Transaction *t, size_t var_result);

// make a list with the specified elements
TAction* ListCreate(Transaction *t, const Vector<TOperand*> &args,
                    size_t var_result);

// push an element onto the end of an existing list
TAction* ListPush(Transaction *t, TOperand *list, TOperand *arg,
                  size_t var_result);

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
