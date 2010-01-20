
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

// interface for the backends which execute transaction functions.

#include "transaction.h"
#include "operand.h"
#include "action.h"

NAMESPACE_XGILL_BEGIN

class TransactionBackend;

// interface for a function which can run during a transaction.
// functions return true on success, false and print an error otherwise.
typedef bool (*TFunction)(Transaction *t, const Vector<TOperand*> &arguments,
                          TOperand **result);

// function for starting up or finishing a backend.
typedef void (*TStartFunction)();
typedef void (*TFinishFunction)();

// the transaction backend defines the various functions which
// can be invoked by a transaction.
class TransactionBackend
{
 public:
  // setup all builtin backends for calling functions. this must only
  // be called once.
  static void StartBackend();

  // finish the backends, persisting data to disk if necessary. this must
  // only be called once.
  static void FinishBackend();

  // run a function in some backend on the specified arguments,
  // returning a result in *result if there is one.
  // return true on success, false and print an error otherwise.
  static bool RunFunction(Transaction *t, const char *name,
                          const Vector<TOperand*> &arguments,
                          TOperand **result);

  // register a function which can be called for this backend.
  // registration should be performed by the start method, and the name
  // should be unique across all backends.
  static void RegisterFunction(const char *name, TFunction function);

 public:
  TransactionBackend(TStartFunction start, TFinishFunction finish)
    : m_start(start), m_finish(finish)
  {}

 private:
  // start and finish functions for this backend. finish may be NULL.
  TStartFunction m_start;
  TFinishFunction m_finish;
};

/////////////////////////////////////////////////////////////////////
// backend macros
/////////////////////////////////////////////////////////////////////

// namespace to put backend implementation functions and data in.

#define BACKEND_IMPL        Backend_IMPL
#define BACKEND_IMPL_BEGIN  namespace Backend_IMPL {
#define BACKEND_IMPL_END    }

// register a function NAME whose implementation is at BACKEND_IMPL::NAME.
#define BACKEND_REGISTER(NAME)                                          \
  do {                                                                  \
    TransactionBackend::RegisterFunction(#NAME, Backend_IMPL::NAME);    \
  } while (0)

// make a call to function NAME, storing the result (if any) in RESULT.
#define BACKEND_CALL(NAME, RESULT)                                      \
  TActionCall *call = new TActionCall(t, RESULT, #NAME)

// the following are helpers for writing backend implementation functions.

// check that the number of arguments is exactly NUM.
#define BACKEND_ARG_COUNT(NUM)                                  \
  do {                                                          \
    if (arguments.Size() != NUM) {                              \
      logout << "ERROR: Expected " #NUM " arguments." << endl;  \
      return false;                                             \
    }                                                           \
  } while (0)

// get a NULL-terminated string from argument POS and store it in NAME/LEN.
// LEN includes the NULL-terminator, i.e. it is strlen(NAME) + 1.
#define BACKEND_ARG_STRING(POS, NAME, LEN)                              \
  const uint8_t *NAME = NULL;                                           \
  size_t LEN = 0;                                                       \
  do {                                                                  \
    if (arguments[POS]->Kind() != TO_String) {                          \
      logout << "ERROR: Argument " #POS " must be a string." << endl;   \
      return false;                                                     \
    }                                                                   \
    TOperandString *arg = (TOperandString*)arguments[POS];              \
    NAME = arg->GetData();                                              \
    LEN = arg->GetDataLength();                                         \
    if (!ValidString(NAME, LEN)) {                                      \
      logout << "ERROR: Argument " #POS " must be NULL-terminated: ";   \
      PrintString(logout, NAME, LEN);                                   \
      logout << endl;                                                   \
      return false;                                                     \
    }                                                                   \
  } while (0)

// get an unformatted string from argument POS and store in DATA/LEN.
#define BACKEND_ARG_DATA(POS, DATA, LEN)                                \
  const uint8_t *DATA = NULL;                                           \
  size_t LEN = 0;                                                       \
  do {                                                                  \
    if (arguments[POS]->Kind() != TO_String) {                          \
      logout << "ERROR: Argument " #POS " must be a string." << endl;   \
      return false;                                                     \
    }                                                                   \
    TOperandString *arg = (TOperandString*)arguments[POS];              \
    DATA = arg->GetData();                                              \
    LEN = arg->GetDataLength();                                         \
    if (LEN == 0) {                                                     \
      logout << "ERROR: Argument " #POS " must not be empty." << endl;  \
      return false;                                                     \
    }                                                                   \
  } while (0)

// get a timestamp from argument POS and store it in TIME.
#define BACKEND_ARG_TIMESTAMP(POS, TIME)                                 \
  TimeStamp TIME = 0;                                                    \
  do {                                                                   \
    if (arguments[POS]->Kind() != TO_TimeStamp) {                        \
      logout << "ERROR: Argument " #POS " must be a timestamp." << endl; \
      return false;                                                      \
    }                                                                    \
    TOperandTimeStamp *arg = (TOperandTimeStamp*)arguments[POS];         \
    TIME = arg->GetStamp();                                              \
  } while (0)

// get a list from argument POS and store in LIST.
#define BACKEND_ARG_LIST(POS, LIST)                                     \
  TOperandList *LIST = NULL;                                            \
  do {                                                                  \
    if (arguments[POS]->Kind() != TO_List) {                            \
      logout << "ERROR: Argument " #POS " must be a list." << endl;     \
      return false;                                                     \
    }                                                                   \
    LIST = (TOperandList*)arguments[POS];                               \
  } while (0)

NAMESPACE_XGILL_END
