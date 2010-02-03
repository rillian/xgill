
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

#include "backend_hash.h"

NAMESPACE_XGILL_BEGIN

/////////////////////////////////////////////////////////////////////
// backend data
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

class DataStringDecRef : public HashTableVisitor<DataString*,DataString*>
{
  void Visit(DataString *&str, Vector<DataString*> &str_list) {
    str->DecRef(&str_list);
    for (size_t ind = 0; ind < str_list.Size(); ind++)
      str_list[ind]->DecRef(&str_list);
  }
};

void ClearDataStringHash(DataStringHash *table)
{
  DataStringDecRef decref_DataString;
  table->VisitEach(&decref_DataString);
  table->Clear();
}

// name and contents for an in-memory hash
struct HashInfo {
  String *name;       // holds a reference
  DataStringHash *table;

  HashInfo()
    : name(NULL), table(NULL)
  {}
};

// list of all in-memory hashes
Vector<HashInfo> hashes;

void ClearHashes()
{
  for (size_t hind = 0; hind < hashes.Size(); hind++) {
    HashInfo &info = hashes[hind];

    info.name->DecRef();
    if (info.table)
      ClearDataStringHash(info.table);
    delete info.table;
  }
  hashes.Clear();
}

HashInfo& GetHash(const uint8_t *name, bool do_create = true)
{
  String *name_str = String::Make((const char*)name);
  for (size_t hind = 0; hind < hashes.Size(); hind++) {
    HashInfo &info = hashes[hind];

    if (info.name == name_str) {
      name_str->DecRef();

      // create the hash if we previously did a non-create access.
      if (do_create && info.table == NULL)
        info.table = new DataStringHash();

      return info;
    }
  }

  DataStringHash *table = NULL;
  if (do_create)
    table = new DataStringHash();

  HashInfo info;
  info.name = name_str;
  info.table = table;
  hashes.PushBack(info);

  return hashes.Back();
}

BACKEND_IMPL_END

DataStringHash* GetNamedHash(const uint8_t *name)
{
  return BACKEND_IMPL::GetHash(name, false).table;
}

/////////////////////////////////////////////////////////////////////
// backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

bool HashExists(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, hash_name, hash_length);

  DataStringHash *hash = GetHash(hash_name, false).table;

  *result = new TOperandBoolean(t, hash != NULL);
  return true;
}

bool HashClear(Transaction *t, const Vector<TOperand*> &arguments,
               TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, hash_name, hash_length);

  HashInfo &info = GetHash(hash_name, true);
  ClearDataStringHash(info.table);

  return true;
}

bool HashIsEmpty(Transaction *t, const Vector<TOperand*> &arguments,
                 TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, hash_name, hash_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  *result = new TOperandBoolean(t, hash->IsEmpty());
  return true;
}

bool HashInsertKey(Transaction *t, const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  HashInfo &info = GetHash(hash_name, true);

  DataString *keystr = DataString::Make(key_data, key_length);

  if (info.table->Lookup(keystr, false) != NULL) {
    keystr->DecRef();
    return true;
  }

  // force the insert and consume the reference on keystr.
  Vector<DataString*> *entries = info.table->Lookup(keystr, true);
  keystr->MoveRef(NULL, entries);

  return true;
}

bool HashInsertValue(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);
  BACKEND_ARG_DATA(2, value_data, value_length);

  HashInfo &info = GetHash(hash_name, true);

  DataString *keystr = DataString::Make(key_data, key_length);
  DataString *valuestr = DataString::Make(value_data, value_length);

  Vector<DataString*> *entries = info.table->Lookup(keystr, false);
  if (entries != NULL) {
    keystr->DecRef();
  }
  else {
    // force the insert and consume the reference on keystr.
    entries = info.table->Lookup(keystr, true);
    keystr->MoveRef(NULL, entries);
  }

  // check for a duplicate entry.
  for (size_t eind = 0; eind < entries->Size(); eind++) {
    if (entries->At(eind) == valuestr) {
      valuestr->DecRef();
      return true;
    }
  }

  // add the entry and consume the reference on valuestr.
  entries->PushBack(valuestr);
  valuestr->MoveRef(NULL, entries);
  return true;
}

bool HashInsertCheck(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);
  BACKEND_ARG_DATA(2, value_data, value_length);

  HashInfo &info = GetHash(hash_name, true);

  DataString *keystr = DataString::Make(key_data, key_length);
  DataString *valuestr = DataString::Make(value_data, value_length);

  Vector<DataString*> *entries = info.table->Lookup(keystr, false);
  if (entries != NULL) {
    keystr->DecRef();
  }
  else {
    // force the insert and consume the reference on keystr.
    entries = info.table->Lookup(keystr, true);
    keystr->MoveRef(NULL, entries);
  }

  // check for a duplicate entry, and get its index.
  bool found_exists = false;
  for (size_t eind = 0; eind < entries->Size(); eind++) {
    if (entries->At(eind) == valuestr) {
      found_exists = true;
      break;
    }
  }

  if (!found_exists) {
    // add the entry and consume the reference on valuestr.
    entries->PushBack(valuestr);
    valuestr->MoveRef(NULL, entries);
  }
  else {
    valuestr->DecRef();
  }

  TOperandList *list_val = new TOperandList(t);
  list_val->PushOperand(new TOperandBoolean(t, found_exists));

  // add all the entries to the list result.
  for (size_t eind = 0; eind < entries->Size(); eind++) {
    DataString *ds = entries->At(eind);
    TOperand *val = new TOperandString(t, ds->Value(), ds->ValueLength());
    list_val->PushOperand(val);
  }

  *result = list_val;
  return true;
}

bool HashChooseKey(Transaction *t, const Vector<TOperand*> &arguments,
                   TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  BACKEND_ARG_STRING(0, hash_name, hash_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  if (hash->IsEmpty()) {
    *result = new TOperandString(t, "");
    return true;
  }

  DataString *key = hash->ChooseKey();
  size_t key_length = key->ValueLength();

  // copy the key's data in case it is deleted later by the transaction.
  Buffer *buf = new Buffer(key_length);
  buf->Append(key->Value(), key_length);
  t->AddBuffer(buf);

  *result = new TOperandString(t, buf->base, key_length);
  return true;
}

bool HashIsMember(Transaction *t, const Vector<TOperand*> &arguments,
                  TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  DataString *keystr = DataString::Make(key_data, key_length);
  Vector<DataString*> *entries = hash->Lookup(keystr, false);
  keystr->DecRef();

  *result = new TOperandBoolean(t, entries != NULL);
  return true;
}

bool HashLookup(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  DataString *keystr = DataString::Make(key_data, key_length);
  Vector<DataString*> *entries = hash->Lookup(keystr, false);
  keystr->DecRef();

  TOperandList *list = new TOperandList(t);

  if (entries != NULL) {
    for (size_t eind = 0; eind < entries->Size(); eind++) {
      DataString *value = entries->At(eind);
      size_t value_length = value->ValueLength();

      Buffer *buf = new Buffer(value_length);
      buf->Append(value->Value(), value_length);
      t->AddBuffer(buf);

      list->PushOperand(new TOperandString(t, buf->base, value_length));
    }
  }

  *result = list;
  return true;
}

bool HashLookupSingle(Transaction *t, const Vector<TOperand*> &arguments,
                      TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  DataString *keystr = DataString::Make(key_data, key_length);
  Vector<DataString*> *entries = hash->Lookup(keystr, false);
  keystr->DecRef();

  if (entries == NULL || entries->Size() != 1) {
    logout << "ERROR: Key must have a single associated value." << endl;
    return false;
  }

  DataString *value = entries->At(0);
  size_t value_length = value->ValueLength();

  Buffer *buf = new Buffer(value_length);
  buf->Append(value->Value(), value_length);
  t->AddBuffer(buf);

  *result = new TOperandString(t, buf->base, value_length);
  return true;
}

bool HashRemove(Transaction *t, const Vector<TOperand*> &arguments,
                TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);
  BACKEND_ARG_STRING(1, key_data, key_length);

  DataStringHash *hash = GetHash(hash_name, true).table;

  DataString *keystr = DataString::Make(key_data, key_length);
  Vector<DataString*> *entries = hash->Lookup(keystr, false);

  if (entries != NULL) {
    // drop the references for the table entry
    keystr->DecRef();
    for (size_t eind = 0; eind < entries->Size(); eind++)
      entries->At(eind)->DecRef(entries);

    // remove the entry itself
    hash->Remove(keystr);
  }

  // drop the reference from our original lookup
  keystr->DecRef(entries);
  return true;
}

class DataStringGetKeys : public HashTableVisitor<DataString*,DataString*>
{
public:
  Transaction *t;
  TOperandList *list;

  void Visit(DataString *&str, Vector<DataString*> &str_list)
  {
    size_t length = str->ValueLength();
    Buffer *buf = new Buffer(length);
    t->AddBuffer(buf);
    buf->Append(str->Value(), length);
    list->PushOperand(new TOperandString(t, buf->pos, length));
  }
};

bool HashAllKeys(Transaction *t, const Vector<TOperand*> &arguments,
                 TOperand **result)
{
  BACKEND_ARG_COUNT(2);
  BACKEND_ARG_STRING(0, hash_name, hash_length);

  TOperandList *list = new TOperandList(t);

  if (DataStringHash *hash = GetNamedHash(hash_name)) {
    DataStringGetKeys visitor;
    visitor.t = t;
    visitor.list = list;
    hash->VisitEach(&visitor);
  }

  *result = list;
  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// backend
/////////////////////////////////////////////////////////////////////

static void start_Hash()
{
  BACKEND_REGISTER(HashExists);
  BACKEND_REGISTER(HashClear);
  BACKEND_REGISTER(HashIsEmpty);
  BACKEND_REGISTER(HashInsertKey);
  BACKEND_REGISTER(HashInsertValue);
  BACKEND_REGISTER(HashInsertCheck);
  BACKEND_REGISTER(HashChooseKey);
  BACKEND_REGISTER(HashIsMember);
  BACKEND_REGISTER(HashLookup);
  BACKEND_REGISTER(HashLookupSingle);
  BACKEND_REGISTER(HashRemove);
  BACKEND_REGISTER(HashAllKeys);
}

static void finish_Hash()
{
  BACKEND_IMPL::ClearHashes();
}

TransactionBackend backend_Hash(start_Hash, finish_Hash);

/////////////////////////////////////////////////////////////////////
// backend wrappers
/////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(Backend)

TAction* HashExists(Transaction *t,
                    const char *hash_name,
                    size_t var_result)
{
  BACKEND_CALL(HashExists, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  return call;
}

TAction* HashClear(Transaction *t,
                   const char *hash_name)
{
  BACKEND_CALL(HashClear, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  return call;
}

TAction* HashIsEmpty(Transaction *t,
                     const char *hash_name,
                     size_t var_result)
{
  BACKEND_CALL(HashIsEmpty, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  return call;
}

TAction* HashInsertKey(Transaction *t,
                       const char *hash_name,
                       TOperand *key)
{
  BACKEND_CALL(HashInsertKey, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  return call;
}

TAction* HashInsertValue(Transaction *t,
                         const char *hash_name,
                         TOperand *key,
                         TOperand *value)
{
  BACKEND_CALL(HashInsertValue, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  call->PushArgument(value);
  return call;
}

TAction* HashInsertCheck(Transaction *t,
                         const char *hash_name,
                         TOperand *key,
                         TOperand *value,
                         size_t list_result)
{
  BACKEND_CALL(HashInsertCheck, list_result);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  call->PushArgument(value);
  return call;
}

TAction* HashChooseKey(Transaction *t,
                       const char *hash_name,
                       size_t var_result)
{
  BACKEND_CALL(HashChooseKey, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  return call;
}

TAction* HashIsMember(Transaction *t,
                      const char *hash_name,
                      TOperand *key,
                      size_t var_result)
{
  BACKEND_CALL(HashIsMember, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  return call;
}

TAction* HashLookup(Transaction *t,
                    const char *hash_name,
                    TOperand *key,
                    size_t var_result)
{
  BACKEND_CALL(HashLookup, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  return call; 
}

TAction* HashLookupSingle(Transaction *t,
                          const char *hash_name,
                          TOperand *key,
                          size_t var_result)
{
  BACKEND_CALL(HashLookupSingle, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  return call;
}

TAction* HashRemove(Transaction *t,
                    const char *hash_name,
                    TOperand *key)
{
  BACKEND_CALL(HashRemove, 0);
  call->PushArgument(new TOperandString(t, hash_name));
  call->PushArgument(key);
  return call;
}

TAction* HashAllKeys(Transaction *t,
                     const char *hash_name,
                     size_t var_result)
{
  BACKEND_CALL(HashAllKeys, var_result);
  call->PushArgument(new TOperandString(t, hash_name));
  return call;
}

NAMESPACE_END(Backend)

NAMESPACE_XGILL_END
