
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

#include "backend_compound.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)
NAMESPACE_BEGIN(Compound)

// HashCreateXdbKeys ($hashname, $dbname)
//   $existvar = HashExists($hashname)
//   if !$existvar
//     HashClear($hashname)
//     $keyslist = XdbAllKeys($dbname)
//     foreach $key in $keyslist
//       HashInsertKey($hashname, $key)

TAction* HashCreateXdbKeys(Transaction *t,
                           const char *hash_name,
                           const char *db_name)
{
  size_t existvar = t->MakeVariable();
  size_t keylistvar = t->MakeVariable();
  size_t keyvar = t->MakeVariable();

  TOperand *existarg = new TOperandVariable(t, existvar);
  TOperand *keylistarg = new TOperandVariable(t, keylistvar);
  TOperand *keyarg = new TOperandVariable(t, keyvar);

  TActionIterate *key_iter = new TActionIterate(t, keyvar, keylistarg);
  key_iter->PushAction(HashInsertKey(t, hash_name, keyarg));

  TActionTest *nex_test = new TActionTest(t, existarg, false);
  nex_test->PushAction(HashClear(t, hash_name));
  nex_test->PushAction(XdbAllKeys(t, db_name, keylistvar));
  nex_test->PushAction(key_iter);

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashExists(t, hash_name, existvar));
  sequence->PushAction(nex_test);

  return sequence;
}

// HashPopXdbKey ($hashname, $dbname)
//   $return_key = HashChooseKey($hashname)
//   HashRemove($hashname, $return_key)
//   $return_value = XdbLookup($dbname, $return_key)

TAction* HashPopXdbKey(Transaction *t,
                       const char *hash_name,
                       const char *db_name,
                       size_t key_result,
                       size_t value_result)
{
  TOperand *key_arg = new TOperandVariable(t, key_result);

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashChooseKey(t, hash_name, key_result));
  sequence->PushAction(HashRemove(t, hash_name, key_arg));
  sequence->PushAction(XdbLookup(t, db_name, key_arg, value_result));

  return sequence;
}

// HashPopXdbKeyWithSort ($sortname, $hashname, $dbname)
//   $return_key = GraphGetMaxSort($sortname)
//   GraphRemoveMaxSort($sortname)
//   HashRemove($hashname, $return_key)
//   $return_empty = StringIsEmpty($return_key)
//   if $return_empty
//     $return_key = HashChooseKey($hashname)
//     HashRemove($hashname, $return_key)
//   $return_value = XdbLookup($dbname, $return_key)

TAction* HashPopXdbKeyWithSort(Transaction *t,
                               const char *sort_name,
                               const char *hash_name,
                               const char *db_name,
                               size_t key_result,
                               size_t value_result)
{
  TOperand *key_arg = new TOperandVariable(t, key_result);

  size_t empty_var = t->MakeVariable();
  TOperand *empty_arg = new TOperandVariable(t, empty_var);

  TActionTest *empty_test = new TActionTest(t, empty_arg, true);
  empty_test->PushAction(HashChooseKey(t, hash_name, key_result));
  empty_test->PushAction(HashRemove(t, hash_name, key_arg));

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(GraphGetMaxSort(t, sort_name, key_result));
  sequence->PushAction(GraphRemoveMaxSort(t, sort_name));
  sequence->PushAction(HashRemove(t, hash_name, key_arg));
  sequence->PushAction(StringIsEmpty(t, key_arg, empty_var));
  sequence->PushAction(empty_test);
  sequence->PushAction(XdbLookup(t, db_name, key_arg, value_result));

  return sequence;
}

// XdbReplaceConditional ($dbname, $key, $value, $rstamp, $succeed_action)
//   $timevar := XdbTimeStamp($dbname, $key)
//   $cmpvar := TimeStampLessEqual($timevar, $rstamp)
//   if ($cmpvar)
//     XdbReplace($dbname, $key, $value)
//     $succeed_action
//   if (!$cmpvar)
//     $return_new_value := XdbLookup($dbname, $key)

TAction* XdbReplaceConditional(Transaction *t,
                               const char *db_name,
                               TOperand *key,
                               TOperand *value,
                               TimeStamp rstamp,
                               TAction *succeed_action,
                               size_t new_value_result)
{
  size_t timevar = t->MakeVariable();
  size_t cmpvar = t->MakeVariable();

  TOperand *timearg = new TOperandVariable(t, timevar);
  TOperand *cmparg = new TOperandVariable(t, cmpvar);
  TOperand *rstamparg = new TOperandTimeStamp(t, rstamp);

  TActionTest *le_test = new TActionTest(t, cmparg, true);
  le_test->PushAction(XdbReplace(t, db_name, key, value));
  if (succeed_action != NULL)
    le_test->PushAction(succeed_action);

  TActionTest *gt_test = new TActionTest(t, cmparg, false);
  gt_test->PushAction(XdbLookup(t, db_name, key, new_value_result));

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(XdbTimeStamp(t, db_name, key, timevar));
  sequence->PushAction(TimeStampLessEqual(t, timearg, rstamparg, cmpvar));
  sequence->PushAction(le_test);
  sequence->PushAction(gt_test);

  return sequence;
}

// XdbReplaceTry($dbname, $key, $value, $rstamp, $succeed_action)
//   $timevar := XdbTimeStamp($dbname, $key)
//   $cmpvar := TimeStampLessEqual($timevar, $rstamp)
//   if ($cmpvar)
//     XdbReplace($dbname, $key, $value)
//     $succeed_action

TAction* XdbReplaceTry(Transaction *t,
                       const char *db_name,
                       TOperand *key,
                       TOperand *value,
                       TimeStamp rstamp,
                       TAction *succeed_action,
                       size_t *pcmp_var)
{
  size_t timevar = t->MakeVariable();
  size_t cmpvar = t->MakeVariable();

  if (pcmp_var)
    *pcmp_var = cmpvar;

  TOperand *timearg = new TOperandVariable(t, timevar);
  TOperand *cmparg = new TOperandVariable(t, cmpvar);
  TOperand *rstamparg = new TOperandTimeStamp(t, rstamp);

  TActionTest *le_test = new TActionTest(t, cmparg, true);
  le_test->PushAction(XdbReplace(t, db_name, key, value));
  if (succeed_action != NULL)
    le_test->PushAction(succeed_action);

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(XdbTimeStamp(t, db_name, key, timevar));
  sequence->PushAction(TimeStampLessEqual(t, timearg, rstamparg, cmpvar));
  sequence->PushAction(le_test);

  return sequence;
}

// XdbLookupDependency ($dbname, $key, $depname, $workval)
//   HashInsert($depname, $key, $workval)
//   $return_value := XdbLookup($dbname, $key)

TAction* XdbLookupDependency(Transaction *t,
                             const char *dbname,
                             TOperand *key,
                             const char *dep_name,
                             TOperand *workval,
                             size_t value_result)
{
  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashInsertValue(t, dep_name, key, workval));
  sequence->PushAction(XdbLookup(t, dbname, key, value_result));

  return sequence;
}

// UpdateDependency ($depname, $key, $workname)
//   $depvals := HashLookup($depname, $key)
//   foreach $dep in $depvals
//     HashInsertKey($workname, $dep)

TAction* UpdateDependency(Transaction *t,
                          const char *dep_name,
                          TOperand *key,
                          const char *work_name)
{
  size_t deplistvar = t->MakeVariable();
  size_t depvar = t->MakeVariable();

  TOperand *deplistarg = new TOperandVariable(t, deplistvar);
  TOperand *deparg = new TOperandVariable(t, depvar);

  TActionIterate *dep_iter = new TActionIterate(t, depvar, deplistarg);
  dep_iter->PushAction(HashInsertKey(t, work_name, deparg));

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashLookup(t, dep_name, key, deplistvar));
  sequence->PushAction(dep_iter);

  return sequence;
}

// HashRunIfEmpty ($hashname, $action)
//  $var = HashIsEmpty($hashname)
//  if $var
//    $action

TAction* HashRunIfEmpty(Transaction *t,
                        const char *hashname,
                        TAction *action)
{
  size_t emptyvar = t->MakeVariable();
  TOperand *emptyarg = new TOperandVariable(t, emptyvar);

  TActionTest *empty_test = new TActionTest(t, emptyarg, true);
  empty_test->PushAction(action);

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashIsEmpty(t, hashname, emptyvar));
  sequence->PushAction(empty_test);

  return sequence;
}

// XdbClearIfNotHash ($dbname, $hashname)
//  $var = HashExists($hashname)
//  if $var
//    XdbClear($dbname)

TAction* XdbClearIfNotHash(Transaction *t,
                           const char *dbname,
                           const char *hashname)
{
  size_t existvar = t->MakeVariable();
  TOperand *existarg = new TOperandVariable(t, existvar);

  TActionTest *nexist_test = new TActionTest(t, existarg, false);
  nexist_test->PushAction(XdbClear(t, dbname));

  TActionSequence *sequence = new TActionSequence(t);
  sequence->PushAction(HashExists(t, hashname, existvar));
  sequence->PushAction(nexist_test);

  return sequence;
}

NAMESPACE_END(Compound)
NAMESPACE_END(Backend)

/////////////////////////////////////////////////////////////////////
// Lookup transactions
/////////////////////////////////////////////////////////////////////

bool DoLookupTransaction(const char *db_name,
                         const char *key_name,
                         Buffer *buf)
{
  Transaction *t = new Transaction();

  size_t data_res = t->MakeVariable(true);
  TOperand *key_arg = new TOperandString(t, key_name);
  t->PushAction(Backend::XdbLookup(t, db_name, key_arg, data_res));

  SubmitTransaction(t);

  TOperandString *data_value = t->LookupString(data_res);

  if (data_value->GetDataLength() == 0) {
    delete t;
    return false;
  }
  else {
    TOperandString::Uncompress(data_value, buf);

    delete t;
    return true;
  }
}

NAMESPACE_XGILL_END
