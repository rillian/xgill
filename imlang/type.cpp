
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

#include "type.h"
#include "storage.h"

NAMESPACE_XGILL_BEGIN

// define to print the fully decorated names of program fields.
// #define FIELD_PRINT_DECORATED

/////////////////////////////////////////////////////////////////////
// Type static
/////////////////////////////////////////////////////////////////////

HashCons<Type> Type::g_table;

int Type::Compare(const Type *y0, const Type *y1)
{
  TryCompareValues(y0->Kind(), y1->Kind());

  switch (y0->Kind()) {
  case YK_Void:
  case YK_Error:
    break;
  case YK_Int:
  case YK_Float:
    TryCompareValues(y0->Width(), y1->Width());
    TryCompareValues((int)y0->IsSigned(), (int)y1->IsSigned());
    break;
  case YK_Pointer: {
    // we shouldn't ever see pointers with different widths.
    Assert(y0->Width() == y1->Width());

    const TypePointer *ny0 = (const TypePointer*)y0;
    const TypePointer *ny1 = (const TypePointer*)y1;
    TryCompareObjects(ny0->GetTargetType(), ny1->GetTargetType(), Type);
    break;
  }
  case YK_Array: { 
    const TypeArray *ny0 = (const TypeArray*)y0;
    const TypeArray *ny1 = (const TypeArray*)y1;
    TryCompareObjects(ny0->GetElementType(), ny1->GetElementType(), Type);
    TryCompareValues(ny0->GetElementCount(), ny1->GetElementCount());
    break;
  }
  case YK_CSU: {
    const TypeCSU *ny0 = (const TypeCSU*)y0;
    const TypeCSU *ny1 = (const TypeCSU*)y1;
    TryCompareObjects(ny0->GetCSUName(), ny1->GetCSUName(), String);
    break;
  }
  case YK_Function: {
    const TypeFunction *ny0 = (const TypeFunction*)y0;
    const TypeFunction *ny1 = (const TypeFunction*)y1;
    TryCompareObjects(ny0->GetReturnType(), ny1->GetReturnType(), Type);
    TryCompareObjects(ny0->GetCSUType(), ny1->GetCSUType(), Type);
    TryCompareValues((int)ny0->IsVarArgs(), (int)ny1->IsVarArgs());
    TryCompareValues(ny0->GetArgumentCount(), ny1->GetArgumentCount());
    for (size_t aind = 0; aind < ny0->GetArgumentCount(); aind++) {
      Type *arg0 = ny0->GetArgumentType(aind);
      Type *arg1 = ny1->GetArgumentType(aind);
      TryCompareObjects(arg0, arg1, Type);
    }
    break;
  }
  default:
    Assert(false);
  }

  return 0;
}

Type* Type::Copy(const Type *y)
{
  switch (y->Kind()) {
  case YK_Void:      return new TypeVoid     (*(TypeVoid*)y);
  case YK_Int:       return new TypeInt      (*(TypeInt*)y);
  case YK_Float:     return new TypeFloat    (*(TypeFloat*)y);
  case YK_Pointer:   return new TypePointer  (*(TypePointer*)y);
  case YK_Array:     return new TypeArray    (*(TypeArray*)y);
  case YK_CSU:       return new TypeCSU      (*(TypeCSU*)y);
  case YK_Function:  return new TypeFunction (*(TypeFunction*)y);
  case YK_Error:     return new TypeError    (*(TypeError*)y);
  default:
    Assert(false);
    return NULL;
  }
}

void Type::Write(Buffer *buf, const Type *y)
{
  WriteOpenTag(buf, TAG_Type);
  WriteTagUInt32(buf, TAG_Kind, y->Kind());

  switch (y->Kind()) {
  case YK_Void:
  case YK_Error:
    break;
  case YK_Int:
    WriteTagUInt32(buf, TAG_Width, y->Width());
    if (y->IsSigned())
      WriteTagEmpty(buf, TAG_Sign);
    break;
  case YK_Float:
    WriteTagUInt32(buf, TAG_Width, y->Width());
    break;
  case YK_Pointer: {
    const TypePointer *ny = (const TypePointer*)y;
    WriteTagUInt32(buf, TAG_Width, y->Width());
    Type::Write(buf, ny->GetTargetType());
    break;
  }
  case YK_Array: {
    const TypeArray *ny = (const TypeArray*)y;
    Type::Write(buf, ny->GetElementType());
    WriteTagUInt32(buf, TAG_Count, ny->GetElementCount());
    break;
  }
  case YK_CSU: {
    const TypeCSU *ny = (const TypeCSU*)y;
    String::WriteWithTag(buf, ny->GetCSUName(), TAG_Name);
    break;
  }
  case YK_Function: {
    const TypeFunction *ny = (const TypeFunction*)y;
    Type::Write(buf, ny->GetReturnType());
    if (ny->GetCSUType()) {
      WriteOpenTag(buf, TAG_TypeFunctionCSU);
      Type::Write(buf, ny->GetCSUType());
      WriteCloseTag(buf, TAG_TypeFunctionCSU);
    }
    if (ny->IsVarArgs())
      WriteTagEmpty(buf, TAG_TypeFunctionVarArgs);
    if (ny->GetArgumentCount() > 0) {
      WriteOpenTag(buf, TAG_TypeFunctionArguments);
      for (size_t aind = 0; aind < ny->GetArgumentCount(); aind++)
        Type::Write(buf, ny->GetArgumentType(aind));
      WriteCloseTag(buf, TAG_TypeFunctionArguments);
    }
    break;
  }
  default:
    Assert(false);
  }
  WriteCloseTag(buf, TAG_Type);
}

Type* Type::Read(Buffer *buf)
{
  uint32_t kind = 0;
  uint32_t width = 0;
  uint32_t count = 0;
  bool sign = false;
  bool varargs = false;
  String *name = NULL;
  Type *target_type = NULL;
  TypeCSU *csu_type = NULL;
  Vector<Type*> argument_types;

  Try(ReadOpenTag(buf, TAG_Type));
  while (!ReadCloseTag(buf, TAG_Type)) {
    switch (PeekOpenTag(buf)) {
    case TAG_Kind: {
      Try(!kind);
      Try(ReadTagUInt32(buf, TAG_Kind, &kind));
      break;
    }
    case TAG_Width: {
      Try(ReadTagUInt32(buf, TAG_Width, &width));
      break;
    }
    case TAG_Sign: {
      Try(ReadTagEmpty(buf, TAG_Sign));
      sign = true;
      break;
    }
    case TAG_Name: {
      Try(!name);
      Try(kind == YK_CSU);
      name = String::ReadWithTag(buf, TAG_Name);
      break;
    }
    case TAG_Type: {
      Try(!target_type);
      Try(kind == YK_Pointer || kind == YK_Array || kind == YK_Function);
      target_type = Type::Read(buf);
      break;
    }
    case TAG_Count: {
      Try(kind == YK_Array);
      Try(ReadTagUInt32(buf, TAG_Count, &count));
      break;
    }
    case TAG_TypeFunctionCSU: {
      Try(!csu_type);
      Try(ReadOpenTag(buf, TAG_TypeFunctionCSU));
      csu_type = Type::Read(buf)->AsCSU();
      Try(ReadCloseTag(buf, TAG_TypeFunctionCSU));
      break;
    }
    case TAG_TypeFunctionVarArgs: {
      Try(kind == YK_Function);
      Try(ReadTagEmpty(buf, TAG_TypeFunctionVarArgs));
      varargs = true;
      break;
    }
    case TAG_TypeFunctionArguments: {
      Try(kind == YK_Function);
      Try(argument_types.Empty());
      Try(ReadOpenTag(buf, TAG_TypeFunctionArguments));
      while (!ReadCloseTag(buf, TAG_TypeFunctionArguments)) {
        Type *ntype = Type::Read(buf);
        argument_types.PushBack(ntype);
      }
      break;
    }
    default:
      Try(false);
    }
  }

  switch ((TypeKind)kind) {
  case YK_Error:
    return MakeError();
  case YK_Void:
    return MakeVoid();
  case YK_Int:
    return MakeInt(width, sign);
  case YK_Float:
    return MakeFloat(width);
  case YK_Pointer:
    Try(target_type);
    return MakePointer(target_type, width);
  case YK_Array:
    Try(target_type);
    return MakeArray(target_type, count);
  case YK_CSU:
    Try(name);
    return MakeCSU(name);
  case YK_Function:
    Try(target_type);
    return MakeFunction(target_type, csu_type, varargs, argument_types);
  default:
    Try(false);
  }
}

TypeError* Type::MakeError() {
  TypeError xy;
  return (TypeError*) g_table.Lookup(xy);
}

TypeVoid* Type::MakeVoid() {
  TypeVoid xy;
  return (TypeVoid*) g_table.Lookup(xy);
}

TypeInt* Type::MakeInt(size_t width, bool sign) {
  TypeInt xy(width, sign);
  return (TypeInt*) g_table.Lookup(xy);
}

TypeFloat* Type::MakeFloat(size_t width) {
  TypeFloat xy(width);
  return (TypeFloat*) g_table.Lookup(xy);
}

TypePointer* Type::MakePointer(Type *target_type, size_t width) {
  TypePointer xy(target_type, width);
  return (TypePointer*) g_table.Lookup(xy);
}

TypeArray* Type::MakeArray(Type *element_type, size_t element_count) {
  TypeArray xy(element_type, element_count);
  return (TypeArray*) g_table.Lookup(xy);
}

TypeCSU* Type::MakeCSU(String *csu_name) {
  TypeCSU xy(csu_name);
  return (TypeCSU*) g_table.Lookup(xy);
}

TypeFunction* Type::MakeFunction(Type *return_type, TypeCSU *csu_type,
                                 bool varargs,
                                 const Vector<Type*> &arguments) {
  TypeFunction xy(return_type, csu_type, varargs, arguments);
  return (TypeFunction*) g_table.Lookup(xy);
}

/////////////////////////////////////////////////////////////////////
// TypeVoid
/////////////////////////////////////////////////////////////////////

TypeVoid::TypeVoid()
  : Type(YK_Void)
{}

size_t TypeVoid::Width() const
{
  // treat void as having a width of one, for bounds etc.
  return 1;
}

void TypeVoid::Print(OutStream &out) const
{
  out << "void";
}

/////////////////////////////////////////////////////////////////////
// TypeInt
/////////////////////////////////////////////////////////////////////

TypeInt::TypeInt(size_t width, bool sign)
  : Type(YK_Int), m_width(width), m_sign(sign)
{
  m_hash = Hash32(m_hash, m_width * 2 + (m_sign ? 1 : 0));
}

size_t TypeInt::Width() const
{
  return m_width;
}

bool TypeInt::IsSigned() const
{
  return m_sign;
}

void TypeInt::Print(OutStream &out) const
{
  out << (m_sign ? "int" : "uint") << (long) (m_width * 8);
}

/////////////////////////////////////////////////////////////////////
// TypeFloat
/////////////////////////////////////////////////////////////////////

TypeFloat::TypeFloat(size_t width)
  : Type(YK_Float), m_width(width)
{
  m_hash = Hash32(m_hash, m_width);
}

size_t TypeFloat::Width() const
{
  return m_width;
}

bool TypeFloat::IsSigned() const
{
  return true;
}

void TypeFloat::Print(OutStream &out) const
{
  out << "float" << (long) (m_width * 8);
}

/////////////////////////////////////////////////////////////////////
// TypePointer
/////////////////////////////////////////////////////////////////////

TypePointer::TypePointer(Type *target_type, size_t width)
  : Type(YK_Pointer), m_target_type(target_type), m_width(width)
{
  Assert(m_target_type);
  m_hash = Hash32(m_hash, m_target_type->Hash());
  m_hash = Hash32(m_hash, m_width);
}

size_t TypePointer::Width() const
{
  return m_width;
}

void TypePointer::Print(OutStream &out) const
{
  m_target_type->Print(out);
  out << "*";
}

void TypePointer::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_target_type->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// TypeArray
/////////////////////////////////////////////////////////////////////

TypeArray::TypeArray(Type *element_type, size_t element_count)
  : Type(YK_Array),
    m_element_type(element_type), m_element_count(element_count)
{
  Assert(m_element_type);
  m_hash = Hash32(m_hash, m_element_count);
  m_hash = Hash32(m_hash, m_element_type->Hash());
}

size_t TypeArray::Width() const
{
  return m_element_count * m_element_type->Width();
}

void TypeArray::Print(OutStream &out) const
{
  m_element_type->Print(out);
  out << "[" << (long)m_element_count << "]";
}

void TypeArray::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_element_type->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// TypeCSU
/////////////////////////////////////////////////////////////////////

TypeCSU::TypeCSU(String *csu_name)
  : Type(YK_CSU), m_csu_name(csu_name)
{
  Assert(m_csu_name);
  m_hash = Hash32(m_hash, m_csu_name->Hash());
}

size_t TypeCSU::Width() const
{
  CompositeCSU *csu = CompositeCSUCache.Lookup(m_csu_name);

  size_t width = 0;
  if (csu)
    width = csu->GetWidth();

  CompositeCSUCache.Release(m_csu_name);
  return width;
}

void TypeCSU::Print(OutStream &out) const
{
  out << m_csu_name->Value();
}

void TypeCSU::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_csu_name->DecMoveRef(ov, nv);
}

/////////////////////////////////////////////////////////////////////
// TypeFunction
/////////////////////////////////////////////////////////////////////

TypeFunction::TypeFunction(Type *return_type, TypeCSU *csu_type, bool varargs,
                           const Vector<Type*> &argument_types)
  : Type(YK_Function),
    m_return_type(return_type), m_csu_type(csu_type), m_varargs(varargs),
    m_argument_types(argument_types.Data()),
    m_argument_count(argument_types.Size())
{
  Assert(m_return_type);
  AssertArray(m_argument_types, m_argument_count);

  if (m_varargs)
    m_hash++;
  m_hash = Hash32(m_hash, m_return_type->Hash());
  if (m_csu_type)
    m_hash = Hash32(m_hash, m_csu_type->Hash());
  for (size_t aind = 0; aind < m_argument_count; aind++)
    m_hash = Hash32(m_hash, m_argument_types[aind]->Hash());
}

size_t TypeFunction::Width() const
{
  // treat all functions as having a one byte width.
  return 1;
}

void TypeFunction::Print(OutStream &out) const
{
  out << "(" << m_return_type;
  if (m_csu_type)
    out << "," << m_csu_type;
  out << ")(";
  for (size_t aind = 0; aind < m_argument_count; aind++) {
    if (aind != 0)
      out << ",";
    out << m_argument_types[aind];
  }
  if (m_varargs)
    out << ",...";
  out << ")";
}

void TypeFunction::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_return_type->DecMoveRef(ov, nv);
  if (m_csu_type)
    m_csu_type->DecMoveRef(ov, nv);
  for (size_t aind = 0; aind < m_argument_count; aind++)
    m_argument_types[aind]->DecMoveRef(ov, nv);
}

void TypeFunction::Persist()
{
  if (m_argument_count > 0) {
    Type **new_argument_types = new Type*[m_argument_count];
    memcpy(new_argument_types, m_argument_types,
           m_argument_count * sizeof(Type*));
    m_argument_types = new_argument_types;
  }
  else {
    m_argument_types = NULL;
  }
}

void TypeFunction::UnPersist()
{
  if (m_argument_types)
    delete[] m_argument_types;
}

/////////////////////////////////////////////////////////////////////
// TypeError
/////////////////////////////////////////////////////////////////////

TypeError::TypeError()
  : Type(YK_Error)
{}

void TypeError::Print(OutStream &out) const
{
  out << "<error>";
}

/////////////////////////////////////////////////////////////////////
// CompositeCSU static
/////////////////////////////////////////////////////////////////////

HashCons<CompositeCSU> CompositeCSU::g_table;

int CompositeCSU::Compare(const CompositeCSU *csu0, const CompositeCSU *csu1)
{
  // we assume the names of different CSU's are unique, so just checking
  // the name is enough.
  TryCompareObjects(csu0->GetName(), csu1->GetName(), String);
  return 0;
}

CompositeCSU* CompositeCSU::Copy(const CompositeCSU *csu)
{
  return new CompositeCSU(*csu);
}

void CompositeCSU::Write(Buffer *buf, const CompositeCSU *csu)
{
  Assert(csu->m_begin_location);
  Assert(csu->m_end_location);

  WriteOpenTag(buf, TAG_CompositeCSU);

  String::WriteWithTag(buf, csu->GetName(), TAG_Name);
  WriteTagUInt32(buf, TAG_Kind, csu->Kind());

  Location::Write(buf, csu->m_begin_location);
  Location::Write(buf, csu->m_end_location);

  WriteTagUInt32(buf, TAG_Width, csu->GetWidth());

  for (size_t ind = 0; ind < csu->GetBaseClassCount(); ind++) {
    WriteOpenTag(buf, TAG_CSUBaseClass);
    String::Write(buf, csu->GetBaseClass(ind));
    WriteCloseTag(buf, TAG_CSUBaseClass);
  }

  for (size_t ind = 0; ind < csu->GetFieldCount(); ind++) {
    const DataField &df = csu->GetField(ind);
    WriteOpenTag(buf, TAG_DataField);
    Field::Write(buf, df.field);
    WriteTagUInt32(buf, TAG_Offset, df.offset);
    WriteCloseTag(buf, TAG_DataField);
  }

  for (size_t ind = 0; ind < csu->GetFunctionFieldCount(); ind++) {
    const FunctionField &ff = csu->GetFunctionField(ind);
    WriteOpenTag(buf, TAG_FunctionField);
    Field::Write(buf, ff.field);
    if (ff.function)
      Variable::Write(buf, ff.function);
    WriteCloseTag(buf, TAG_FunctionField);
  }

  WriteCloseTag(buf, TAG_CompositeCSU);
}

CompositeCSU* CompositeCSU::Read(Buffer *buf)
{
  CompositeCSU *res = NULL;
  bool drop_info = false;

  Try(ReadOpenTag(buf, TAG_CompositeCSU));
  while (!ReadCloseTag(buf, TAG_CompositeCSU)) {
    switch (PeekOpenTag(buf)) {
    case TAG_Name: {
      Try(!res);
      String *name = String::ReadWithTag(buf, TAG_Name);
      res = Make(name);

      // throw away all the data we read if the CSU is already filled in.
      if (res->m_kind)
        drop_info = true;
      break;
    }
    case TAG_Kind: {
      Try(res);
      uint32_t kind;
      Try(ReadTagUInt32(buf, TAG_Kind, &kind));

      if (!drop_info)
        res->SetKind((CSUKind)kind);
      break;
    }
    case TAG_Width: {
      Try(res);
      uint32_t width;
      Try(ReadTagUInt32(buf, TAG_Width, &width));

      if (!drop_info)
        res->SetWidth(width);
      break;
    }
    case TAG_Location: {
      Try(res);
      Location *loc = Location::Read(buf);

      if (drop_info)
        loc->DecRef();
      else if (!res->m_begin_location)
        res->SetBeginLocation(loc);
      else
        res->SetEndLocation(loc);
      break;
    }
    case TAG_CSUBaseClass: {
      Try(res);
      Try(ReadOpenTag(buf, TAG_CSUBaseClass));
      String *base_class = String::Read(buf);
      Try(ReadCloseTag(buf, TAG_CSUBaseClass));

      if (drop_info)
        base_class->DecRef();
      else
        res->AddBaseClass(base_class);
      break;
    }
    case TAG_DataField: {
      Try(res);
      Try(ReadOpenTag(buf, TAG_DataField));
      uint32_t offset;
      Field *field = Field::Read(buf);
      Try(ReadTagUInt32(buf, TAG_Offset, &offset));
      Try(ReadCloseTag(buf, TAG_DataField));

      if (drop_info)
        field->DecRef();
      else
        res->AddField(field, offset);
      break;
    }
    case TAG_FunctionField: {
      Try(res);
      Try(ReadOpenTag(buf, TAG_FunctionField));
      Field *field = Field::Read(buf);
      Variable *function = NULL;
      if (PeekOpenTag(buf) == TAG_Variable)
        function = Variable::Read(buf);
      Try(ReadCloseTag(buf, TAG_FunctionField));
 
      if (drop_info) {
        field->DecRef();
        if (function)
          function->DecRef();
      }
      else {
        res->AddFunctionField(field, function);
      }
      break;
    }
    default:
      Try(false);
    }
  }

  Try(res);
  return res;
}

/////////////////////////////////////////////////////////////////////
// CompositeCSU
/////////////////////////////////////////////////////////////////////

CompositeCSU::CompositeCSU(String *name)
  : m_kind(CSU_Invalid), m_name(name), m_width(0),
    m_begin_location(NULL), m_end_location(NULL),
    m_base_classes(NULL), m_data_fields(NULL), m_function_fields(NULL)
{
  Assert(m_name);
  m_hash = m_name->Hash();
}

void CompositeCSU::SetKind(CSUKind kind)
{
  // make sure that multiple assignments of the kind are consistent.
  Assert(m_kind == CSU_Invalid || m_kind == kind);

  m_kind = kind;
}

void CompositeCSU::SetWidth(size_t width)
{
  // make sure that multiple assignments of the width are consistent.
  Assert(m_width == 0 || m_width == width);

  m_width = width;
}

void CompositeCSU::SetBeginLocation(Location *loc)
{
  if (m_begin_location)
    m_begin_location->DecRef(this);
  loc->MoveRef(NULL, this);
  m_begin_location = loc;
}

void CompositeCSU::SetEndLocation(Location *loc)
{
  if (m_end_location)
    m_end_location->DecRef(this);
  loc->MoveRef(NULL, this);
  m_end_location = loc;
}

void CompositeCSU::AddBaseClass(String *base_class)
{
  if (m_base_classes == NULL)
    m_base_classes = new Vector<String*>();

  base_class->MoveRef(NULL, this);
  m_base_classes->PushBack(base_class);
}

void CompositeCSU::AddField(Field *field, size_t offset)
{
  if (m_data_fields == NULL)
    m_data_fields = new Vector<DataField>();

  field->MoveRef(NULL, this);
  m_data_fields->PushBack(DataField(field, offset));
}

void CompositeCSU::AddFunctionField(Field *field, Variable *function)
{
  if (m_function_fields == NULL)
    m_function_fields = new Vector<FunctionField>();

  field->MoveRef(NULL, this);
  if (function)
    function->MoveRef(NULL, this);
  m_function_fields->PushBack(FunctionField(field, function));
}

void CompositeCSU::Print(OutStream &out) const
{
  switch (m_kind) {
  case CSU_Class: out << "class "; break;
  case CSU_Struct: out << "struct "; break;
  case CSU_Union: out << "union "; break;
  default: Assert(false);
  }

  out << m_name << endl;

  out << "  begin_location: " << m_begin_location << endl;
  out << "  end_location: " << m_end_location << endl;

  out << "  width: " << m_width << endl;

  for (size_t ind = 0; ind < GetBaseClassCount(); ind++)
    out << "  base: " << GetBaseClass(ind) << endl;

  for (size_t ind = 0; ind < GetFieldCount(); ind++) {
    const DataField &df = GetField(ind);
    out << "  field: " << df.offset << " "
        << df.field->GetName()->Value() << " " << df.field->GetType() << endl;
  }

  for (size_t ind = 0; ind < GetFunctionFieldCount(); ind++) {
    const FunctionField &ff = GetFunctionField(ind);
    out << "  function: " << ff.field;
    if (ff.function)
      out << " " << ff.function;
    out << endl;
  }
}

void CompositeCSU::DecMoveChildRefs(ORef ov, ORef nv)
{
  // similar case to BlockCFG, all references except the name are dropped
  // by UnPersist.
  m_name->DecMoveRef(ov, nv);

  // again, same case as BlockCFG, UnPersist is idempotent and can be
  // called repeatedly.
  if (ov == this) {
    Assert(nv == NULL);
    UnPersist();
  }
}

void CompositeCSU::Persist()
{
  // only the kind and name have been set up at this point.
}

void CompositeCSU::UnPersist()
{
  if (m_begin_location) {
    m_begin_location->DecRef(this);
    m_begin_location = NULL;
  }

  if (m_end_location) {
    m_end_location->DecRef(this);
    m_end_location = NULL;
  }

  if (m_base_classes) {
    DecRefVector<String>(*m_base_classes, this);
    delete m_base_classes;
    m_base_classes = NULL;
  }

  if (m_data_fields) {
    for (size_t ind = 0; ind < m_data_fields->Size(); ind++)
      m_data_fields->At(ind).field->DecRef(this);
    delete m_data_fields;
    m_data_fields = NULL;
  }

  if (m_function_fields) {
    for (size_t ind = 0; ind < m_function_fields->Size(); ind++) {
      const FunctionField &ff = m_function_fields->At(ind);
      ff.field->DecRef(this);
      if (ff.function)
        ff.function->DecRef(this);
    }
    delete m_function_fields;
    m_function_fields = NULL;
  }
}

/////////////////////////////////////////////////////////////////////
// Field static
/////////////////////////////////////////////////////////////////////

HashCons<Field> Field::g_table;

int Field::Compare(const Field *f0, const Field *f1)
{
  TryCompareObjects(f0->GetCSUType(), f1->GetCSUType(), Type);
  TryCompareObjects(f0->GetName(), f1->GetName(), String);

  // in general, the name/index/CSU data should be enough to distinguish
  // different fields. however, in some frontend error cases we can get
  // confused when making fields and we want to check the other components
  // of the field.

  TryCompareObjects(f0->GetSourceName(), f1->GetSourceName(), String);
  TryCompareObjects(f0->GetType(), f1->GetType(), Type);

  TryCompareValues((int)f0->IsInstanceFunction(),
                   (int)f1->IsInstanceFunction());
  return 0;
}

Field* Field::Copy(const Field *f)
{
  return new Field(*f);
}

void Field::Write(Buffer *buf, const Field *f)
{
  WriteOpenTag(buf, TAG_Field);

  String::WriteWithTag(buf, f->GetName(), TAG_Name);
  if (f->GetSourceName())
    String::WriteWithTag(buf, f->GetSourceName(), TAG_Name);

  Type::Write(buf, f->GetType());

  WriteOpenTag(buf, TAG_FieldCSU);
  Type::Write(buf, f->GetCSUType());
  WriteCloseTag(buf, TAG_FieldCSU);

  if (f->IsInstanceFunction())
    WriteTagEmpty(buf, TAG_FieldInstanceFunction);

  WriteCloseTag(buf, TAG_Field);
}

Field* Field::Read(Buffer *buf)
{
  String *name = NULL;
  String *source_name = NULL;
  Type *csu_type = NULL;
  Type *type = NULL;
  bool is_function = false;

  Try(ReadOpenTag(buf, TAG_Field));
  while (!ReadCloseTag(buf, TAG_Field)) {
    switch (PeekOpenTag(buf)) {
    case TAG_Name: {
      if (name) {
        Try(!source_name);
        source_name = String::ReadWithTag(buf, TAG_Name);
      }
      else {
        name = String::ReadWithTag(buf, TAG_Name);
      }
      break;
    }
    case TAG_Type: {
      Try(!type);
      type = Type::Read(buf);
      break;
    }
    case TAG_FieldCSU: {
      Try(!csu_type);
      Try(ReadOpenTag(buf, TAG_FieldCSU));
      csu_type = Type::Read(buf);
      Try(ReadCloseTag(buf, TAG_FieldCSU));
      break;
    }
    case TAG_FieldInstanceFunction: {
      Try(ReadTagEmpty(buf, TAG_FieldInstanceFunction));
      is_function = true;
      break;
    }
    default:
      Try(false);
    }
  }

  return Make(name, source_name, csu_type->AsCSU(), type, is_function);
}

/////////////////////////////////////////////////////////////////////
// Field
/////////////////////////////////////////////////////////////////////

Field::Field(String *name, String *source_name, TypeCSU *csu_type,
             Type *type, bool is_function)
  : m_name(name), m_source_name(source_name),
    m_csu_type(csu_type), m_type(type), m_is_function(is_function)
{
  Assert(m_name);
  Assert(m_csu_type);
  Assert(m_type);

  m_hash = Hash32(m_csu_type->Hash(), m_name->Hash());
}

void Field::Print(OutStream &out) const
{
#ifdef FIELD_PRINT_DECORATED
  out << m_name->Value();
#else
  if (m_source_name)
    out << m_source_name->Value();
  else
    out << m_name->Value();
#endif
}

void Field::DecMoveChildRefs(ORef ov, ORef nv)
{
  m_name->DecMoveRef(ov, nv);
  if (m_source_name)
    m_source_name->DecMoveRef(ov, nv);

  m_csu_type->DecMoveRef(ov, nv);
  m_type->DecMoveRef(ov, nv);
}

NAMESPACE_XGILL_END
