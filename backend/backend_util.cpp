
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

#include "backend_util.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

bool TimeStampDeltaBefore(Transaction *t,
                          const Vector<TOperand*> &arguments,
                          TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_TIMESTAMP(0, delta);

  TimeStamp stamp = t->GetTimeStamp();

  TimeStamp res = 0;
  if (stamp >= delta)
    res = stamp - delta;

  *result = new TOperandTimeStamp(t, res);
  return true;
}

bool TimeStampDeltaAfter(Transaction *t,
                         const Vector<TOperand*> &arguments,
                         TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_TIMESTAMP(0, delta);

  TimeStamp stamp = t->GetTimeStamp();
  TimeStamp res = stamp + delta;

  *result = new TOperandTimeStamp(t, res);
  return true;
}

bool TimeStampLess(Transaction *t,
                   const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_TIMESTAMP(0, time0);
  BACKEND_ARG_TIMESTAMP(1, time1);

  *result = new TOperandBoolean(t, time0 < time1);
  return true;
}

bool TimeStampLessEqual(Transaction *t,
                        const Vector<TOperand*> &arguments,
                        TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_TIMESTAMP(0, time0);
  BACKEND_ARG_TIMESTAMP(1, time1);

  *result = new TOperandBoolean(t, time0 <= time1);
  return true;
}

bool StringIsEmpty(Transaction *t,
                   const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, str, str_length);

  *result = new TOperandBoolean(t, str_length == 1);
  return true;
}

bool ListCreate(Transaction *t,
                const Vector<TOperand*> &arguments,
                TOperand **result)
{
  TOperandList *res = new TOperandList(t);

  for (size_t aind = 0; aind < arguments.Size(); aind++)
    res->PushOperand(arguments[aind]);

  *result = res;
  return true;
}

bool ListPush(Transaction *t,
              const Vector<TOperand*> &arguments,
              TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_LIST(0, list);

  // make a copy of the argument list, do not modify it directly.
  TOperandList *res = new TOperandList(t);
  for (size_t lind = 0; lind < list->GetCount(); lind++)
    res->PushOperand(list->GetOperand(lind));
  res->PushOperand(arguments[1]);

  *result = res;
  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend
/////////////////////////////////////////////////////////////////////

static void start_Util()
{
  BACKEND_REGISTER(TimeStampDeltaBefore);
  BACKEND_REGISTER(TimeStampDeltaAfter);
  BACKEND_REGISTER(TimeStampLess);
  BACKEND_REGISTER(TimeStampLessEqual);
  BACKEND_REGISTER(StringIsEmpty);
  BACKEND_REGISTER(ListCreate);
  BACKEND_REGISTER(ListPush);
}

TransactionBackend backend_Util(start_Util, NULL);

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* TimeStampDeltaSeconds(Transaction *t,
                               ssize_t seconds,
                               size_t var_result)
{
  if (seconds < 0) {
    TimeStamp delta = TimeSecondsToStamp(-seconds);

    BACKEND_CALL(TimeStampDeltaBefore, var_result);
    call->PushArgument(new TOperandTimeStamp(t, delta));
    return call;
  }
  else {
    TimeStamp delta = TimeSecondsToStamp(seconds);

    BACKEND_CALL(TimeStampDeltaAfter, var_result);
    call->PushArgument(new TOperandTimeStamp(t, delta));
    return call;
  }
}

TAction* TimeStampLess(Transaction *t,
                       TOperand *time0, TOperand *time1,
                       size_t var_result)
{
  BACKEND_CALL(TimeStampLess, var_result);
  call->PushArgument(time0);
  call->PushArgument(time1);
  return call;
}

TAction* TimeStampLessEqual(Transaction *t,
                            TOperand *time0, TOperand *time1,
                            size_t var_result)
{
  BACKEND_CALL(TimeStampLessEqual, var_result);
  call->PushArgument(time0);
  call->PushArgument(time1);
  return call;
}

TAction* TimeStampGreater(Transaction *t,
                          TOperand *time0, TOperand *time1,
                          size_t var_result)
{
  return TimeStampLess(t, time1, time0, var_result);
}

TAction* TimeStampGreaterEqual(Transaction *t,
                               TOperand *time0, TOperand *time1,
                               size_t var_result)
{
  return TimeStampLessEqual(t, time1, time0, var_result);
}

TAction* StringIsEmpty(Transaction *t, TOperand *str,
                       size_t var_result)
{
  BACKEND_CALL(StringIsEmpty, var_result);
  call->PushArgument(str);
  return call;
}

TAction* ListEmpty(Transaction *t, size_t var_result)
{
  BACKEND_CALL(ListCreate, var_result);
  return call;
}

TAction* ListCreate(Transaction *t, const Vector<TOperand*> &args,
                    size_t var_result)
{
  BACKEND_CALL(ListCreate, var_result);
  for (size_t aind = 0; aind < args.Size(); aind++)
    call->PushArgument(args[aind]);
  return call;
}

TAction* ListPush(Transaction *t, TOperand *list, TOperand *arg,
                  size_t var_result)
{
  BACKEND_CALL(ListPush, var_result);
  call->PushArgument(list);
  call->PushArgument(arg);
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
