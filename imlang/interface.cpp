
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

#include <unistd.h>

#include "filename.h"
#include "interface.h"
#include "loopsplit.h"
#include "storage.h"

#include <backend/backend_xdb.h>
#include <util/monitor.h>

// we are pulling in escape/callgraph information from memory/.
// this is kind of a forward reference and should be cleaned up.
#include <memory/storage.h>
#include <memory/serial.h>

NAMESPACE_XGILL_USING

#define GET_OBJECT(TYPE, NAME)                  \
  TYPE * new_ ##NAME = (TYPE*) NAME;            \
  if (new_ ##NAME) new_ ##NAME ->IncRef();

/////////////////////////////////////////////////////////////////////
// Utility
/////////////////////////////////////////////////////////////////////

// file which log_stream is wrapping.
static FILE* log_file = NULL;

extern "C" void XIL_SetLogFile(FILE *file)
{
  // the log stream may already have been set if we are using a manager.
  // do the assignment regardless.
  log_file = file;
  log_stream = new PrintOutStream(file);
}

FILE* XIL_GetLogFile()
{
  return log_file ? log_file : stdout;
}

extern "C" void XIL_Print(void *object)
{
  HashObject *new_object = (HashObject*) object;
  logout << flush;
  new_object->Print(logout);
  logout << endl << flush;
}

extern "C" void XIL_SetNormalizeDirectory(const char *path)
{
  const char *cwd = getcwd(NULL, 0);

  SetWorkingDirectory(cwd);
  SetBaseDirectory(path ? path : cwd);
}

// hashtable mapping filenames relative to the cwd to filenames relative
// to the normalize directory.
static HashTable<String*,String*,String> g_filename_map;

extern "C" XIL_Location XIL_MakeLocation(const char *file, int line)
{
  String *base_file = String::Make(file);

  Vector<String*> *normal_files = g_filename_map.Lookup(base_file, true);
  String *normal_file = NULL;

  if (normal_files->Empty()) {
    // first time we saw this file.
    normal_file = String::Make(NormalizeFile(file));
    normal_files->PushBack(normal_file);
  }
  else {
    normal_file = normal_files->At(0);
  }

  normal_file->IncRef();
  return (XIL_Location) Location::Make(normal_file, (uint32_t) line);
}

// CSUs and CFGs that have been constructed through this interface.
static Vector<CompositeCSU*> g_keep_csus;
static Vector<BlockCFG*> g_keep_cfgs;

// CSUs and CFGs that were constructed but will be dropped from the output
// due to errors during generation.
static Vector<CompositeCSU*> g_drop_csus;
static Vector<BlockCFG*> g_drop_cfgs;

// currently active CSU or CFG, if there is one.
static Vector<CompositeCSU*> g_active_csus;
static BlockCFG *g_active_cfg = NULL;

// ID to use for locals in g_active_cfg.
static BlockId *g_active_id = NULL;

// ID to use for locals in the function g_active_cfg is an annotation for.
static BlockId *g_annotation_id = NULL;

// whether we have seen any annotation CFGs.
static bool g_has_annotation = false;

struct AssociateKey {
  String *kind;
  void *value;

  AssociateKey() : kind(NULL), value(NULL) {}
  AssociateKey(String *_kind, void *_value) : kind(_kind), value(_value) {}

  static uint32_t Hash(uint32_t hash, const AssociateKey &key) {
    hash = Hash32(hash, key.kind->Hash());
    return Hash32(hash, (uint32_t) (size_t) key.value);
  }

  bool operator == (const AssociateKey &okey) const {
    return kind == okey.kind && value == okey.value;
  }
};

typedef HashTable<AssociateKey,void*,AssociateKey> AssociateTable;

// association hashtables.
static AssociateTable g_associate_annot;
static AssociateTable g_associate_block;
static AssociateTable g_associate_global;

static AssociateTable& GetAssociateTable(XIL_AssociateKind kind)
{
  switch (kind) {
  case XIL_AscAnnotate: return g_associate_annot;
  case XIL_AscBlock:    return g_associate_block;
  case XIL_AscGlobal:   return g_associate_global;
  default: Assert(false);
  }
}

extern "C" void** XIL_Associate(XIL_AssociateKind table,
                                const char *kind, void *value)
{
  AssociateKey key(String::Make(kind), value);
  Vector<void*> *values = GetAssociateTable(table).Lookup(key, true);

  if (values->Empty())
    values->PushBack(NULL);
  Assert(values->Size() == 1);

  return &values->At(0);
}

extern "C" void XIL_ClearAssociate(XIL_AssociateKind table)
{
  GetAssociateTable(table).Clear();
}

/////////////////////////////////////////////////////////////////////
// Active block
/////////////////////////////////////////////////////////////////////

extern "C" void XIL_SetActiveBlock(XIL_Var var, const char *annot_name,
                                   XIL_AnnotationKind annot_kind,
                                   int annot_type)
{
  Assert(!g_active_cfg);

  GET_OBJECT(Variable, var);
  new_var->IncRef();

  // this is different from the active ID for functions, since we are making
  // the initial whole CFG and not the final loop-free CFG.
  BlockId *cfg_id = NULL;

  if (annot_name) {
    g_has_annotation = true;
    String *new_name = String::Make(annot_name);

    if (annot_type) {
      Assert(new_var->Kind() == VK_Glob);
      cfg_id = BlockId::Make(B_AnnotationComp, new_var, new_name);
    }
    else if (new_var->Kind() == VK_Func) {
      cfg_id = BlockId::Make(B_AnnotationFunc, new_var, new_name);
      g_annotation_id = BlockId::Make(B_Function, new_var);
    }
    else if (new_var->Kind() == VK_Glob) {
      cfg_id = BlockId::Make(B_AnnotationInit, new_var, new_name);
    }

    cfg_id->IncRef();
    g_active_id = cfg_id;
  }
  else if (new_var->Kind() == VK_Func) {
    cfg_id = BlockId::Make(B_FunctionWhole, new_var);
    g_active_id = BlockId::Make(B_Function, new_var);
  }
  else if (new_var->Kind() == VK_Glob) {
    cfg_id = BlockId::Make(B_Initializer, new_var);
    g_active_id = BlockId::Make(B_Initializer, new_var);
  }
  else {
    Assert(false);
  }

  g_active_cfg = BlockCFG::Make(cfg_id);

  if (annot_kind)
    g_active_cfg->SetAnnotationKind((AnnotationKind) annot_kind);

  if (!g_keep_cfgs.Contains(g_active_cfg))
    g_keep_cfgs.PushBack(g_active_cfg);
}

extern "C" void XIL_ClearActiveBlock(int drop)
{
  Assert(g_active_cfg);

  if (drop) {
    if (!g_drop_cfgs.Contains(g_active_cfg))
      g_drop_cfgs.PushBack(g_active_cfg);
  }

  g_active_cfg = NULL;
  g_active_id = NULL;
  g_annotation_id = NULL;
}

/////////////////////////////////////////////////////////////////////
// Types
/////////////////////////////////////////////////////////////////////

extern "C" XIL_Type XIL_TypeError()
{
  return (XIL_Type) Type::MakeError();
}

extern "C" XIL_Type XIL_TypeVoid()
{
  return (XIL_Type) Type::MakeVoid();
}

extern "C" XIL_Type XIL_TypeInt(int width, int sign)
{
  return (XIL_Type) Type::MakeInt((size_t) width, (size_t) sign);
}

extern "C" XIL_Type XIL_TypeFloat(int width)
{
  return (XIL_Type) Type::MakeFloat((size_t) width);
}

extern "C" XIL_Type XIL_TypePointer(XIL_Type target_type, int width)
{
  GET_OBJECT(Type, target_type);
  return (XIL_Type) Type::MakePointer(new_target_type, (size_t) width);
}

extern "C" XIL_Type XIL_TypeArray(XIL_Type element_type, int count)
{
  GET_OBJECT(Type, element_type);
  return (XIL_Type) Type::MakeArray(new_element_type, (size_t) count);
}

// set of CSU names we have asked to generate.
static HashSet<String*,HashObject> g_generated_csus;

extern "C" XIL_Type XIL_TypeCSU(const char *csu_name, int *generate)
{
  String *new_csu_name = String::Make(csu_name);

  if (generate) {
    bool exists = g_generated_csus.Insert(new_csu_name);
    *generate = exists ? 0 : 1;
  }

  return (XIL_Type) Type::MakeCSU(new_csu_name);
}

extern "C"
XIL_Type XIL_TypeFunction(XIL_Type return_type, const char *this_csu, int varargs,
                          XIL_Type *arg_types, int arg_count)
{
  GET_OBJECT(Type, return_type);

  String *new_this_csu = this_csu ?  String::Make(this_csu) : NULL;
  TypeCSU *csu_type = new_this_csu ? Type::MakeCSU(new_this_csu) : NULL;

  Vector<Type*> new_arg_types;
  for (int ind = 0; ind < arg_count; ind++) {
    Type *arg_type = (Type*) arg_types[ind];
    arg_type->IncRef();
    new_arg_types.PushBack(arg_type);
  }

  return (XIL_Type)
    Type::MakeFunction(new_return_type, csu_type,
                       (bool) varargs, new_arg_types);
}

const char* XIL_GetTypeCSUName(XIL_Type csu_type)
{
  GET_OBJECT(Type, csu_type);

  if (new_csu_type->IsCSU())
    return new_csu_type->AsCSU()->GetCSUName()->Value();
  return NULL;
}

extern "C"
XIL_Field XIL_MakeField(const char *name, const char *source_name,
                        const char *csu_name, XIL_Type type, int is_func)
{
  String *new_name = String::Make(name);
  String *new_source_name = source_name ? String::Make(source_name) : NULL;
  String *new_csu_name = String::Make(csu_name);
  TypeCSU *csu_type = Type::MakeCSU(new_csu_name);

  GET_OBJECT(Type, type);

  return (XIL_Field)
    Field::Make(new_name, new_source_name, csu_type,
                new_type, (bool) is_func);
}

extern "C" void XIL_PushActiveCSU(const char *name)
{
  String *new_name = String::Make(name);
  CompositeCSU *csu = CompositeCSU::Make(new_name);

  g_active_csus.PushBack(csu);

  if (!g_keep_csus.Contains(csu))
    g_keep_csus.PushBack(csu);
}

extern "C" void XIL_PopActiveCSU(int drop)
{
  CompositeCSU *csu = g_active_csus.Back();
  g_active_csus.PopBack();

  if (drop) {
    if (!g_drop_csus.Contains(csu))
      g_drop_csus.PushBack(csu);
  }
}

extern "C" void XIL_CSUSetKind(int kind)
{
  CompositeCSU *csu = g_active_csus.Back();
  csu->SetKind((CSUKind)kind);
}

extern "C" void XIL_CSUSetWidth(int width)
{
  CompositeCSU *csu = g_active_csus.Back();
  csu->SetWidth((size_t)width);
}

extern "C" void XIL_CSUSetBeginLocation(XIL_Location begin_loc)
{
  CompositeCSU *csu = g_active_csus.Back();

  GET_OBJECT(Location, begin_loc);
  csu->SetBeginLocation(new_begin_loc);
}

extern "C" void XIL_CSUSetEndLocation(XIL_Location end_loc)
{
  CompositeCSU *csu = g_active_csus.Back();

  GET_OBJECT(Location, end_loc);
  csu->SetEndLocation(new_end_loc);
}

extern "C" void XIL_CSUAddBaseClass(const char *base_class)
{
  CompositeCSU *csu = g_active_csus.Back();

  String *new_base_class = String::Make(base_class);
  csu->AddBaseClass(new_base_class);
}

extern "C" void XIL_CSUAddDataField(XIL_Field field, int offset)
{
  CompositeCSU *csu = g_active_csus.Back();

  GET_OBJECT(Field, field);
  csu->AddField(new_field, (size_t) offset);
}

extern "C" void XIL_CSUAddFunctionField(XIL_Field field, XIL_Var function)
{
  CompositeCSU *csu = g_active_csus.Back();

  GET_OBJECT(Field, field);
  GET_OBJECT(Variable, function);
  csu->AddFunctionField(new_field, new_function);
}

/////////////////////////////////////////////////////////////////////
// Variables
/////////////////////////////////////////////////////////////////////

extern "C" XIL_Var XIL_VarGlob(const char *name, const char *source_name)
{
  String *new_name = String::Make(name);
  String *new_source_name = source_name ? String::Make(source_name) : NULL;
  return (XIL_Var)
    Variable::Make(NULL, VK_Glob, new_name, 0, new_source_name);
}

extern "C" XIL_Var XIL_VarFunc(const char *name, const char *source_name)
{
  String *new_name = String::Make(name);
  String *new_source_name = source_name ? String::Make(source_name) : NULL;
  return (XIL_Var)
    Variable::Make(NULL, VK_Func, new_name, 0, new_source_name);
}

extern "C" XIL_Var XIL_VarArg(int index, const char *name, int annot)
{
  BlockId *id = annot ? g_annotation_id : g_active_id;
  Assert(id);
  id->IncRef();

  String *new_name = name ? String::Make(name) : NULL;
  if (new_name) new_name->IncRef();

  return (XIL_Var)
    Variable::Make(id, VK_Arg, new_name, (size_t) index, new_name);
}

extern "C" XIL_Var XIL_VarLocal(const char *name,
                                const char *source_name, int annot)
{
  BlockId *id = annot ? g_annotation_id : g_active_id;
  Assert(id);
  id->IncRef();

  String *new_name = String::Make(name);
  String *new_source_name = source_name ? String::Make(source_name) : NULL;

  return (XIL_Var)
    Variable::Make(id, VK_Local, new_name, 0, new_source_name);
}

extern "C" XIL_Var XIL_VarReturn(int annot)
{
  BlockId *id = annot ? g_annotation_id : g_active_id;
  Assert(id);
  id->IncRef();

  return (XIL_Var) Variable::Make(id, VK_Return, NULL, 0, NULL);
}

extern "C" XIL_Var XIL_VarThis(int annot)
{
  BlockId *id = annot ? g_annotation_id : g_active_id;
  Assert(id);
  id->IncRef();

  return (XIL_Var) Variable::Make(id, VK_This, NULL, 0, NULL);
}

extern "C" XIL_Var XIL_VarTemp(const char *name)
{
  Assert(g_active_id);
  g_active_id->IncRef();

  String *new_name = String::Make(name);
  if (new_name) new_name->IncRef();

  return (XIL_Var)
    Variable::Make(g_active_id, VK_Temp, new_name, 0, new_name);
}

extern "C" const char* XIL_GetVarName(XIL_Var var)
{
  GET_OBJECT(Variable, var);
  if (new_var->GetName())
    return new_var->GetName()->Value();
  return NULL;
}

/////////////////////////////////////////////////////////////////////
// Expressions
/////////////////////////////////////////////////////////////////////

extern "C" XIL_Exp XIL_ExpVar(XIL_Var var)
{
  GET_OBJECT(Variable, var);
  return (XIL_Exp) Exp::MakeVar(new_var);
}

extern "C" XIL_Exp XIL_ExpDrf(XIL_Exp target)
{
  GET_OBJECT(Exp, target);
  return (XIL_Exp) Exp::MakeDrf(new_target);
}

extern "C" XIL_Exp XIL_ExpFld(XIL_Exp target, XIL_Field field)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Field, field);
  return (XIL_Exp) Exp::MakeFld(new_target, new_field);
}


extern "C" XIL_Exp XIL_ExpRfld(XIL_Exp target, XIL_Field field)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Field, field);
  return (XIL_Exp) Exp::MakeRfld(new_target, new_field);
}

extern "C"
XIL_Exp XIL_ExpIndex(XIL_Exp target, XIL_Type element_type, XIL_Exp index)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Type, element_type);
  GET_OBJECT(Exp, index);
  return (XIL_Exp) Exp::MakeIndex(new_target, new_element_type, new_index);
}

extern "C" XIL_Exp XIL_ExpString(XIL_Type type, void *data, int data_length)
{
  GET_OBJECT(Type, type);
  DataString *new_data = DataString::Make((uint8_t*) data, data_length);
  return (XIL_Exp) Exp::MakeString(new_type->AsArray(), new_data);
}

extern "C" XIL_Exp XIL_ExpVPtr(XIL_Exp target, int vtable_index)
{
  GET_OBJECT(Exp, target);
  return (XIL_Exp) Exp::MakeVPtr(new_target, (uint32_t) vtable_index);
}

extern "C" XIL_Exp XIL_ExpInt(long value)
{
  return (XIL_Exp) Exp::MakeInt(value);
}

extern "C" XIL_Exp XIL_ExpIntStr(const char *value)
{
  return (XIL_Exp) Exp::MakeIntStr(value);
}

extern "C" XIL_Exp XIL_ExpFloat(const char *value)
{
  return (XIL_Exp) Exp::MakeFloat(value);
}

extern "C"
XIL_Exp XIL_ExpUnop(XIL_UnopKind unop, XIL_Exp op, int bits, int sign)
{
  GET_OBJECT(Exp, op);
  return (XIL_Exp) Exp::MakeUnop((UnopKind) unop, new_op,
                                 (size_t) bits, (bool) sign);
}

extern "C"
XIL_Exp XIL_ExpBinop(XIL_BinopKind binop, XIL_Exp left_op, XIL_Exp right_op,
                     XIL_Type stride_type, int bits, int sign)
{
  GET_OBJECT(Exp, left_op);
  GET_OBJECT(Exp, right_op);
  GET_OBJECT(Type, stride_type);
  return (XIL_Exp)
    Exp::MakeBinop((BinopKind) binop, new_left_op, new_right_op,
                   new_stride_type, (size_t) bits, (bool) sign);
}

extern "C" XIL_Exp XIL_ExpLoopEntry(XIL_Exp target)
{
  GET_OBJECT(Exp, target);
  return (XIL_Exp) Exp::MakeLoopEntry(new_target, NULL);
}

extern "C" XIL_Exp XIL_ExpLBound(XIL_Exp target, XIL_Type stride_type)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Type, stride_type);
  return (XIL_Exp) Exp::MakeBound(BND_Lower, new_target, new_stride_type);
}

extern "C" XIL_Exp XIL_ExpUBound(XIL_Exp target, XIL_Type stride_type)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Type, stride_type);
  return (XIL_Exp) Exp::MakeBound(BND_Upper, new_target, new_stride_type);
}

extern "C" XIL_Exp XIL_ExpZTerm(XIL_Exp target, XIL_Type stride_type)
{
  GET_OBJECT(Exp, target);
  GET_OBJECT(Type, stride_type);

  Exp *empty_exp = Exp::MakeEmpty();
  ExpInt *zero_exp = Exp::MakeInt(0);

  return (XIL_Exp) Exp::MakeTerminate(new_target, new_stride_type,
                                      empty_exp, zero_exp);
}

extern "C" int XIL_GetExpInt(XIL_Exp exp, long *value)
{
  GET_OBJECT(Exp, exp);

  if (ExpInt *nexp = new_exp->IfInt()) {
    if (nexp->GetInt(value))
      return 1;
  }

  return 0;
}

extern "C" XIL_Exp XIL_ExpAddress(XIL_Exp target)
{
  GET_OBJECT(Exp, target);

  if (ExpDrf *nnew_target = new_target->IfDrf()) {
    Exp *res = nnew_target->GetTarget();
    res->IncRef();
    return (XIL_Exp) res;
  }

  return NULL;
}

/////////////////////////////////////////////////////////////////////
// Blocks
/////////////////////////////////////////////////////////////////////

extern "C" void XIL_CFGSetBeginLocation(XIL_Location begin_loc)
{
  Assert(g_active_cfg);
  GET_OBJECT(Location, begin_loc);
  g_active_cfg->SetBeginLocation(new_begin_loc);
}

extern "C" void XIL_CFGSetEndLocation(XIL_Location end_loc)
{
  Assert(g_active_cfg);
  GET_OBJECT(Location, end_loc);
  g_active_cfg->SetEndLocation(new_end_loc);
}

extern "C" void XIL_CFGAddVar(XIL_Var var, XIL_Type type, int override)
{
  Assert(g_active_cfg);
  GET_OBJECT(Variable, var);
  GET_OBJECT(Type, type);

  if (override)
    new_var->SetType(new_type, true);

  g_active_cfg->AddVariable(new_var, new_type);
}

extern "C" XIL_PPoint XIL_CFGAddPoint(XIL_Location loc)
{
  Assert(g_active_cfg);
  GET_OBJECT(Location, loc);

  return (XIL_PPoint) g_active_cfg->AddPoint(new_loc);
}

extern "C" XIL_Location XIL_CFGGetPointLocation(XIL_PPoint point)
{
  Assert(g_active_cfg);

  Location *loc = g_active_cfg->GetPointLocation((PPoint) point);
  loc->IncRef();

  return (XIL_Location) loc;
}

extern "C" void XIL_CFGSetPointLocation(XIL_PPoint point, XIL_Location loc)
{
  Assert(g_active_cfg);
  GET_OBJECT(Location, loc);

  g_active_cfg->SetPointLocation((PPoint) point, new_loc);
}

extern "C"
void XIL_CFGSetEntryPoint(XIL_PPoint point)
{
  Assert(g_active_cfg);
  g_active_cfg->SetEntryPoint((PPoint) point);
}

extern "C"
void XIL_CFGSetExitPoint(XIL_PPoint point)
{
  Assert(g_active_cfg);
  g_active_cfg->SetExitPoint((PPoint) point);
}

extern "C"
void XIL_CFGAddLoopHead(XIL_PPoint point, XIL_Location end_loc)
{
  Assert(g_active_cfg);
  if (!point) return;

  Location *new_end_loc = end_loc ? (Location*) end_loc : NULL;
  g_active_cfg->AddLoopHead((PPoint) point, new_end_loc);
}

extern "C"
void XIL_CFGEdgeSkip(XIL_PPoint source, XIL_PPoint target)
{
  Assert(g_active_cfg);
  if (!source) return;

  PEdge *edge = PEdge::MakeSkip((PPoint) source, (PPoint) target);
  g_active_cfg->AddEdge(edge);
}

extern "C"
void XIL_CFGEdgeAssume(XIL_PPoint source, XIL_PPoint target,
                       XIL_Exp condition, int nonzero)
{
  Assert(g_active_cfg);
  if (!source) return;

  GET_OBJECT(Exp, condition);

  PEdge *edge = PEdge::MakeAssume((PPoint) source, (PPoint) target,
                                  new_condition, (bool) nonzero);
  g_active_cfg->AddEdge(edge);
}

extern "C"
void XIL_CFGEdgeAssign(XIL_PPoint source, XIL_PPoint target,
                       XIL_Type type, XIL_Exp left_side, XIL_Exp right_side)
{
  Assert(g_active_cfg);
  if (!source) return;

  GET_OBJECT(Type, type);
  GET_OBJECT(Exp, left_side);
  GET_OBJECT(Exp, right_side);

  PEdge *edge = PEdge::MakeAssign((PPoint) source, (PPoint) target,
                                  new_type, new_left_side, new_right_side);
  g_active_cfg->AddEdge(edge);
}

extern "C" XIL_Exp XIL_CFGInstanceFunction(XIL_Exp instance, XIL_Exp func)
{
  GET_OBJECT(Exp, instance);
  GET_OBJECT(Exp, func);

  // if there is an instance object the function should be relative to it.
  // this isn't the case for the function we are passed though, fix it up.
  if (new_instance)
    new_func = ExpReplaceExp(new_func, new_instance, Exp::MakeEmpty());

  if (new_func->IsVar() || new_func->IsRelative())
    return (XIL_Exp) new_func;
  return NULL;
}

extern "C"
void XIL_CFGEdgeCall(XIL_PPoint source, XIL_PPoint target, XIL_Type func_type,
                     XIL_Exp return_assign, XIL_Exp instance, XIL_Exp func,
                     XIL_Exp *args, int arg_count)
{
  Assert(g_active_cfg);
  if (!source) return;

  GET_OBJECT(Type, func_type);
  GET_OBJECT(Exp, return_assign);
  GET_OBJECT(Exp, instance);
  GET_OBJECT(Exp, func);

  Vector<Exp*> new_args;
  for (int ind = 0; ind < arg_count; ind++) {
    Exp *arg = (Exp*) args[ind];
    arg->IncRef();
    new_args.PushBack(arg);
  }

  PEdge *edge = PEdge::MakeCall((PPoint) source, (PPoint) target,
                                new_func_type->AsFunction(),
                                new_return_assign, new_instance, new_func, new_args);
  g_active_cfg->AddEdge(edge);
}

extern "C"
void XIL_CFGEdgeAssembly(XIL_PPoint source, XIL_PPoint target)
{
  Assert(g_active_cfg);
  if (!source) return;

  PEdge *edge = PEdge::MakeAssembly((PPoint) source, (PPoint) target);
  g_active_cfg->AddEdge(edge);
}

extern "C"
void XIL_CFGEdgeAnnotation(XIL_PPoint source, XIL_PPoint target,
                           const char *annot_name)
{
  Assert(g_active_cfg);
  if (!source) return;

  Variable *func_var = g_active_id->BaseVar();
  Assert(func_var->Kind() == VK_Func);

  func_var->IncRef();
  String *new_name = String::Make(annot_name);
  BlockId *annot = BlockId::Make(B_AnnotationFunc, func_var, new_name);

  PEdge *edge = PEdge::MakeAnnotation((PPoint) source, (PPoint) target, annot);
  g_active_cfg->AddEdge(edge);
}

/////////////////////////////////////////////////////////////////////
// Backend data
/////////////////////////////////////////////////////////////////////

// for backend functions.
NAMESPACE_XGILL_BEGIN

// databases accessed online by the block backend.
static Xdb *csu_xdb = NULL;
static Xdb *body_xdb = NULL;
static Xdb *init_xdb = NULL;

// sets of all annotations that have been processed.
static HashTable<String*,BlockCFG*,String> g_backend_annot_func;
static HashTable<String*,BlockCFG*,String> g_backend_annot_init;
static HashTable<String*,BlockCFG*,String> g_backend_annot_comp;

// sets of escape/callgraph information which the block backend has received.
static HashTable<String*,EscapeEdgeSet*,String> g_backend_escape_forward;
static HashTable<String*,EscapeEdgeSet*,String> g_backend_escape_backward;
static HashTable<String*,EscapeAccessSet*,String> g_backend_escape_accesses;
static HashSet<CallEdgeSet*,HashObject> g_backend_callers;
static HashSet<CallEdgeSet*,HashObject> g_backend_callees;

// quickly check whether escape information has been seen.
// these do not hold references.
static HashSet<EscapeEdgeSet*,HashObject> g_backend_seen_escape_edges;
static HashSet<EscapeAccessSet*,HashObject> g_backend_seen_escape_accesses;

// whether to merge with existing data when clearing the backend hashes.
// this is set only if we had to flush the backend data before finishing
// due to high memory usage.
static bool g_backend_merge = false;

// visitor to write out generated annotations to the databases.

class WriteAnnotationVisitor : public HashTableVisitor<String*,BlockCFG*>
{
public:
  const char *db_name;
  WriteAnnotationVisitor(const char *_db_name) : db_name(_db_name) {}

  void Visit(String *&key, Vector<BlockCFG*> &cfg_list)
  {
    if (cfg_list.Empty()) {
      key->DecRef(&cfg_list);
      return;
    }

    Xdb *xdb = GetDatabase(db_name, true);
    static Buffer scratch_buf;

    // lookup and write any old entries first. this is only useful to do when
    // there isn't a manager running.
    XdbFindUncompressed(xdb, key, &scratch_buf);

    Vector<BlockCFG*> old_cfg_list;
    Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
    BlockCFG::ReadList(&read_buf, &old_cfg_list);
    scratch_buf.Reset();

    for (size_t ind = 0; ind < old_cfg_list.Size(); ind++) {
      BlockCFG *cfg = old_cfg_list[ind];

      // watch for duplicate CFGs.
      if (cfg_list.Contains(cfg)) {
        cfg->DecRef();
      }
      else {
        cfg->MoveRef(NULL, &cfg_list);
        cfg_list.PushBack(cfg);
      }
    }

    // write the old and new entries out.
    BlockCFG::WriteList(&scratch_buf, cfg_list);

    XdbReplaceCompress(xdb, key, &scratch_buf);
    scratch_buf.Reset();

    key->DecRef(&cfg_list);
    for (size_t ind = 0; ind < cfg_list.Size(); ind++)
      cfg_list[ind]->DecRef(&cfg_list);
  }
};

// visitors to write all generated escape/callgraph info to the databases.

// read an escape edge set from buf and combine it with any in memory data.
EscapeEdgeSet* CombineEscapeEdge(Buffer *buf)
{
  Trace *source = NULL;
  bool forward = false;
  Vector<EscapeEdge> edges;
  EscapeEdgeSet::ReadMerge(buf, &source, &forward, &edges);

  EscapeEdgeSet *eset = EscapeEdgeSet::Make(source, forward, false);

  for (size_t ind = 0; ind < edges.Size(); ind++)
    eset->AddEdge(edges[ind]);
  return eset;
}

class WriteEscapeEdgeSetVisitor
  : public HashTableVisitor<String*,EscapeEdgeSet*>
{
public:
  bool forward;
  WriteEscapeEdgeSetVisitor(bool _forward) : forward(_forward) {}

  void Visit(String *&key, Vector<EscapeEdgeSet*> &eset_list)
  {
    Xdb *xdb = forward ?
        GetDatabase(ESCAPE_EDGE_FORWARD_DATABASE, true)
      : GetDatabase(ESCAPE_EDGE_BACKWARD_DATABASE, true);

    static Buffer scratch_buf;

    if (g_backend_merge) {
      if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
        Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
        while (read_buf.pos != read_buf.base + read_buf.size) {
          EscapeEdgeSet *eset = CombineEscapeEdge(&read_buf);
          if (!eset_list.Contains(eset)) {
            eset->IncRef(&eset_list);
            eset_list.PushBack(eset);
          }
          eset->DecRef();
        }
      }
    }

    for (size_t ind = 0; ind < eset_list.Size(); ind++) {
      EscapeEdgeSet *eset = eset_list[ind];
      EscapeEdgeSet::Write(&scratch_buf, eset);
      eset->DecRef(&eset_list);
    }

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    key->DecRef(&eset_list);
  }
};

// read an escape access set from buf and combine it with any in memory data.
EscapeAccessSet* CombineEscapeAccess(Buffer *buf)
{
  Trace *value = NULL;
  Vector<EscapeAccess> accesses;
  EscapeAccessSet::ReadMerge(buf, &value, &accesses);

  EscapeAccessSet *aset = EscapeAccessSet::Make(value, false);

  for (size_t ind = 0; ind < accesses.Size(); ind++)
    aset->AddAccess(accesses[ind]);
  return aset;
}

class WriteEscapeAccessSetVisitor
  : public HashTableVisitor<String*,EscapeAccessSet*>
{
public:
  void Visit(String *&key, Vector<EscapeAccessSet*> &aset_list)
  {
    Xdb *xdb = GetDatabase(ESCAPE_ACCESS_DATABASE, true);

    static Buffer scratch_buf;

    if (g_backend_merge) {
      if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
        Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
        while (read_buf.pos != read_buf.base + read_buf.size) {
          EscapeAccessSet *aset = CombineEscapeAccess(&read_buf);
          if (!aset_list.Contains(aset)) {
            aset->IncRef(&aset_list);
            aset_list.PushBack(aset);
          }
          aset->DecRef();
        }
      }
    }

    for (size_t ind = 0; ind < aset_list.Size(); ind++) {
      EscapeAccessSet *aset = aset_list[ind];
      EscapeAccessSet::Write(&scratch_buf, aset);
      aset->DecRef(&aset_list);
    }

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    key->DecRef(&aset_list);
  }
};

// read a call edge set from buf and combine it with any in memory data.
CallEdgeSet* CombineCallEdge(Buffer *buf)
{
  Variable *function = NULL;
  bool callers = false;
  Vector<CallEdge> edges;
  CallEdgeSet::ReadMerge(buf, &function, &callers, &edges);

  CallEdgeSet *cset = CallEdgeSet::Make(function, callers, false);

  for (size_t ind = 0; ind < edges.Size(); ind++)
    cset->AddEdge(edges[ind]);
  return cset;
}

class WriteCallEdgeSetVisitor : public HashSetVisitor<CallEdgeSet*>
{
public:
  bool callers;
  WriteCallEdgeSetVisitor(bool _callers) : callers(_callers) {}

  void Visit(CallEdgeSet *&cset)
  {
    Xdb *xdb = callers ?
        GetDatabase(CALLER_DATABASE, true)
      : GetDatabase(CALLEE_DATABASE, true);
    String *key = cset->GetFunction()->GetName();

    static Buffer scratch_buf;

    if (g_backend_merge) {
      if (XdbFindUncompressed(xdb, key, &scratch_buf)) {
        Buffer read_buf(scratch_buf.base, scratch_buf.pos - scratch_buf.base);
        CallEdgeSet *new_cset = CombineCallEdge(&read_buf);
        Assert(new_cset == cset);
        new_cset->DecRef();
      }
    }

    CallEdgeSet::Write(&scratch_buf, cset);

    XdbReplaceCompress(xdb, key, &scratch_buf);

    scratch_buf.Reset();
    cset->DecRef();
  }
};

// flush all escape/callgraph caches to disk.
void FlushEscapeBackend()
{
  WriteEscapeEdgeSetVisitor visitor_eset_forward(true);
  g_backend_escape_forward.VisitEach(&visitor_eset_forward);
  g_backend_escape_forward.Clear();

  WriteEscapeEdgeSetVisitor visitor_eset_backward(false);
  g_backend_escape_backward.VisitEach(&visitor_eset_backward);
  g_backend_escape_backward.Clear();

  WriteEscapeAccessSetVisitor visitor_aset;
  g_backend_escape_accesses.VisitEach(&visitor_aset);
  g_backend_escape_accesses.Clear();

  WriteCallEdgeSetVisitor visitor_cset_caller(true);
  g_backend_callers.VisitEach(&visitor_cset_caller);
  g_backend_callers.Clear();

  WriteCallEdgeSetVisitor visitor_cset_callee(false);
  g_backend_callees.VisitEach(&visitor_cset_callee);
  g_backend_callees.Clear();

  g_backend_seen_escape_edges.Clear();
  g_backend_seen_escape_accesses.Clear();
}

/////////////////////////////////////////////////////////////////////
// Backend implementations
/////////////////////////////////////////////////////////////////////

BACKEND_IMPL_BEGIN

// determine whether an annotation CFG has been processed. takes three
// arguments: the database name, variable name and annotation name.
bool BlockQueryAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(3);
  BACKEND_ARG_STRING(0, db_name, db_length);
  BACKEND_ARG_STRING(1, var_name, var_length);
  BACKEND_ARG_STRING(2, annot_name, annot_length);

  String *new_var_name = String::Make((const char*) var_name);

  Vector<BlockCFG*> *cfg_list = NULL;
  if (!strcmp((const char*) db_name, BODY_ANNOT_DATABASE))
    cfg_list = g_backend_annot_func.Lookup(new_var_name);
  else if (!strcmp((const char*) db_name, INIT_ANNOT_DATABASE))
    cfg_list = g_backend_annot_init.Lookup(new_var_name);
  else if (!strcmp((const char*) db_name, COMP_ANNOT_DATABASE))
    cfg_list = g_backend_annot_comp.Lookup(new_var_name);
  else
    Assert(false);

  new_var_name->DecRef();

  bool found = false;
  for (size_t ind = 0; cfg_list && ind < cfg_list->Size(); ind++) {
    const char *exist_name = cfg_list->At(ind)->GetId()->Loop()->Value();
    if (!strcmp((const char*) annot_name, exist_name))
      found = true;
  }

  *result = new TOperandBoolean(t, found);
  return true;
}

// write an annotation CFG to the appropriate hashtable.
bool BlockWriteAnnot(Transaction *t, const Vector<TOperand*> &arguments,
                     TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  BlockCFG *annot_cfg = BlockCFG::Read(&read_buf);
  BlockId *id = annot_cfg->GetId();
  String *var_name = id->Function();
  data_buf.Reset();

  Vector<BlockCFG*> *cfg_list = NULL;

  switch (id->Kind()) {
  case B_AnnotationFunc:
    cfg_list = g_backend_annot_func.Lookup(var_name, true); break;
  case B_AnnotationInit:
    cfg_list = g_backend_annot_init.Lookup(var_name, true); break;
  case B_AnnotationComp:
    cfg_list = g_backend_annot_comp.Lookup(var_name, true); break;
  default: Assert(false);
  }

  if (cfg_list->Empty()) {
    // first time we saw this key.
    var_name->IncRef(cfg_list);
  }

  annot_cfg->MoveRef(NULL, cfg_list);
  cfg_list->PushBack(annot_cfg);

  return true;
}

// determine which in a list of CSUs and/or blocks needs to be processed.
// takes one argument, a compressed series of Names and/or BlockIds,
// and result receives the subset of that list which needs to be processed.
bool BlockQueryList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  // open up the databases if this is our first time here.
  static bool have_query = false;
  if (!have_query) {
    have_query = true;
    csu_xdb = GetDatabase(COMP_DATABASE, true);
    body_xdb = GetDatabase(BODY_DATABASE, true);
    init_xdb = GetDatabase(INIT_DATABASE, true);
  }

  static Buffer result_buf;

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_Name: {
      String *name = String::ReadWithTag(&read_buf, TAG_Name);
      const char *name_val = name->Value();
      Buffer key_buf((const uint8_t*) name_val, strlen(name_val) + 1);

      if (!csu_xdb->HasKey(&key_buf))
        String::WriteWithTag(&result_buf, name, TAG_Name);

      name->DecRef();
      break;
    }

    case TAG_BlockId: {
      BlockId *id = BlockId::Read(&read_buf);
      const char *function = id->Function()->Value();
      Buffer key_buf((const uint8_t*) function, strlen(function) + 1);

      Xdb *xdb = NULL;
      if (id->Kind() == B_FunctionWhole)
        xdb = body_xdb;
      else if (id->Kind() == B_Initializer)
        xdb = init_xdb;
      else
        Assert(false);

      if (!xdb->HasKey(&key_buf))
        BlockId::Write(&result_buf, id);

      id->DecRef();
      break;
    }

    default:
      Assert(false);
    }
  }

  data_buf.Reset();

  if (result_buf.pos == result_buf.base) {
    // none of the elements in the list need to be processed.
    *result = new TOperandString(t, NULL, 0);
    return true;
  }

  Buffer *compress_buf = new Buffer();
  t->AddBuffer(compress_buf);

  CompressBufferInUse(&result_buf, compress_buf);
  result_buf.Reset();

  *result = new TOperandString(t, compress_buf->base,
                               compress_buf->pos - compress_buf->base);
  return true;
}

// add the results of processing a set of CSUs and/or blocks. list is
// a compressed series of CompositeCSUs, TAG_Uint32 followed by BlockCFGs,
// EscapeEdgeSets, EscapeAccessSets, and CallEdgeSets.
bool BlockWriteList(Transaction *t, const Vector<TOperand*> &arguments,
                    TOperand **result)
{
  BACKEND_ARG_COUNT(1);
  Assert(arguments[0]->Kind() == TO_String);
  TOperandString *list = (TOperandString*) arguments[0];

  // should not be able to get here without having done a query first.
  Assert(csu_xdb && body_xdb && init_xdb);

  static Buffer data_buf;
  TOperandString::Uncompress(list, &data_buf);
  Buffer read_buf(data_buf.base, data_buf.pos - data_buf.base);

  static Buffer write_buf;

  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {

    case TAG_CompositeCSU: {
      CompositeCSU *csu = CompositeCSU::Read(&read_buf);
      CompositeCSU::Write(&write_buf, csu);

      XdbReplaceCompress(csu_xdb, csu->GetName(), &write_buf);

      csu->DecRef();
      write_buf.Reset();
      break;
    }

    case TAG_UInt32: {
      uint32_t count = 0;
      Try(ReadUInt32(&read_buf, &count));
      Assert(count);

      // the count indicates the number of CFGs for the next function/global.
      Vector<BlockCFG*> function_cfgs;

      for (size_t ind = 0; ind < count; ind++) {
        BlockCFG *cfg = BlockCFG::Read(&read_buf);
        BlockCFG::Write(&write_buf, cfg);
        function_cfgs.PushBack(cfg);
      }

      BlockId *id = function_cfgs[0]->GetId();

      Xdb *xdb = NULL;
      if (id->Kind() == B_Function || id->Kind() == B_Loop)
        xdb = body_xdb;
      else if (id->Kind() == B_Initializer)
        xdb = init_xdb;
      else
        Assert(false);

      XdbReplaceCompress(xdb, id->Function(), &write_buf);

      for (size_t ind = 0; ind < function_cfgs.Size(); ind++)
        function_cfgs[ind]->DecRef();
      write_buf.Reset();
      break;
    }

    // these append to the existing set if there is one.

    case TAG_EscapeEdgeSet: {
      EscapeEdgeSet *eset = CombineEscapeEdge(&read_buf);

      if (g_backend_seen_escape_edges.Insert(eset)) {
        eset->DecRef();
        break;
      }

      String *key = GetTraceKey(eset->GetSource());

      Vector<EscapeEdgeSet*> *eset_list = (eset->IsForward()) ?
          g_backend_escape_forward.Lookup(key, true)
        : g_backend_escape_backward.Lookup(key, true);

      if (eset_list->Empty())
        key->IncRef(eset_list);
      eset->MoveRef(NULL, eset_list);
      eset_list->PushBack(eset);

      key->DecRef();
      break;
    }

    case TAG_EscapeAccessSet: {
      EscapeAccessSet *aset = CombineEscapeAccess(&read_buf);

      if (g_backend_seen_escape_accesses.Insert(aset)) {
        aset->DecRef();
        break;
      }

      String *key = GetTraceKey(aset->GetValue());

      Vector<EscapeAccessSet*> *aset_list =
        g_backend_escape_accesses.Lookup(key, true);

      if (aset_list->Empty())
        key->IncRef(aset_list);
      aset->MoveRef(NULL, aset_list);
      aset_list->PushBack(aset);

      key->DecRef();
      break;
    }

    case TAG_CallEdgeSet: {
      CallEdgeSet *cset = CombineCallEdge(&read_buf);

      bool exists = (cset->IsCallers()) ?
          g_backend_callers.Insert(cset)
        : g_backend_callees.Insert(cset);

      if (exists)
        cset->DecRef();
      break;
    }

    default:
      Assert(false);
    }
  }

  if (IsHighVmUsage()) {
    logout << "WARNING: High memory usage, flushing caches..." << endl;
    FlushEscapeBackend();

    // on subsequent flushes we will have to merge with the data on disk.
    g_backend_merge = true;
  }

  data_buf.Reset();
  return true;
}

BACKEND_IMPL_END

/////////////////////////////////////////////////////////////////////
// Backend
/////////////////////////////////////////////////////////////////////

static void start_Block()
{
  BACKEND_REGISTER(BlockQueryAnnot);
  BACKEND_REGISTER(BlockWriteAnnot);
  BACKEND_REGISTER(BlockQueryList);
  BACKEND_REGISTER(BlockWriteList);
}

static void finish_Block()
{
  // write out any annotations we found.

  WriteAnnotationVisitor func_visitor(BODY_ANNOT_DATABASE);
  g_backend_annot_func.VisitEach(&func_visitor);

  WriteAnnotationVisitor init_visitor(INIT_ANNOT_DATABASE);
  g_backend_annot_init.VisitEach(&init_visitor);

  WriteAnnotationVisitor comp_visitor(COMP_ANNOT_DATABASE);
  g_backend_annot_comp.VisitEach(&comp_visitor);

  if (g_backend_escape_forward.IsEmpty() &&
      g_backend_escape_backward.IsEmpty() &&
      g_backend_escape_accesses.IsEmpty() &&
      g_backend_callers.IsEmpty() &&
      g_backend_callees.IsEmpty() &&
      !g_backend_merge)
    return;

  // sort and write out the callgraph hash.
  BACKEND_IMPL::Backend_GraphTopoSortHash((const uint8_t*) CALLGRAPH_HASH,
                                          (const uint8_t*) CALLGRAPH_SORT);
  BACKEND_IMPL::Backend_GraphStoreSort((const uint8_t*) CALLGRAPH_SORT,
                                       (const uint8_t*) BODY_SORT_FILE);

  // flush any remaining escape/callgraph changes.
  FlushEscapeBackend();
}

TransactionBackend backend_Block(start_Block, finish_Block);

NAMESPACE_XGILL_END

/////////////////////////////////////////////////////////////////////
// Databases
/////////////////////////////////////////////////////////////////////

extern "C" void XIL_SetupGenerate(const char *remote_address)
{
  // always consider generation to be restarts, since we can only look
  // at one translation unit at a time. the script controlling the build
  // must terminate the manager manually.
  AnalysisPrepare(remote_address, true);
  SkipHashConsCounts();
}

extern "C" void XIL_PrintGenerated()
{
  logout << "Generated Objects:" << endl << endl;

  for (size_t ind = 0; ind < g_keep_csus.Size(); ind++) {
    CompositeCSU *csu = g_keep_csus[ind];

    if (g_drop_csus.Contains(csu))
      continue;

    csu->Print(logout);
    logout << endl;
  }

  for (size_t ind = 0; ind < g_keep_cfgs.Size(); ind++) {
    BlockCFG *cfg = g_keep_cfgs[ind];

    if (g_drop_cfgs.Contains(cfg))
      continue;

    cfg->Print(logout);
    logout << endl;
  }
}

// buffer for writing out query or processed data.
static Buffer g_data_buf;

// soft limit on the amount of data we will write in one transaction when
// querying whether to process data or writing out the processed data.
// this limit is checked before compressing, so the amount of transmitted
// data will be considerably less.
#define TRANSACTION_DATA_LIMIT 512 * 1024

// whether the amount of data in g_data_buf exceeds the data limit.
#define TRANSACTION_DATA_EXCEEDED                               \
  (g_data_buf.pos - g_data_buf.base > TRANSACTION_DATA_LIMIT)

// lists for constructing batch queries on which CSUs/blocks to process.
static Vector<CompositeCSU*> g_query_csus;
static Vector<BlockCFG*> g_query_blocks;

// list of the CSUs and blocks we will actually be processing.
static Vector<CompositeCSU*> g_write_csus;
static Vector<BlockCFG*> g_write_blocks;

// lists with escape/callgraph information from the blocks we are processing.
static Vector<EscapeEdgeSet*> g_escape_edges;
static Vector<EscapeAccessSet*> g_escape_accesses;
static Vector<CallEdgeSet*> g_call_edges;

// data_buf contains the identifiers for all current query CSUs/blocks.
// submit a transaction to see which of these should be processed,
// adding entries to the write lists as necessary.
static void ProcessQueryList(Transaction *t)
{
  size_t result_var = t->MakeVariable(true);

  TOperand *list_op = TOperandString::Compress(t, &g_data_buf);

  BACKEND_CALL(BlockQueryList, result_var);
  call->PushArgument(list_op);
  t->PushAction(call);

  SubmitTransaction(t);
  g_data_buf.Reset();

  TOperandString *result_op = t->LookupString(result_var);

  if (result_op->GetDataLength() == 0) {
    // none of the queried CSUs/blocks needs to be processed.
    t->Clear();
    return;
  }

  TOperandString::Uncompress(result_op, &g_data_buf);

  Vector<String*> found_csus;
  Vector<BlockId*> found_blocks;

  Buffer read_buf(g_data_buf.base, g_data_buf.pos - g_data_buf.base);
  while (read_buf.pos != read_buf.base + read_buf.size) {
    switch (PeekOpenTag(&read_buf)) {
    case TAG_Name:
      found_csus.PushBack(String::ReadWithTag(&read_buf, TAG_Name));
      break;
    case TAG_BlockId:
      found_blocks.PushBack(BlockId::Read(&read_buf));
      break;
    default:
      Assert(false);
    }
  }

  t->Clear();

  for (size_t ind = 0; ind < g_query_csus.Size(); ind++) {
    CompositeCSU *csu = g_query_csus[ind];
    if (found_csus.Contains(csu->GetName()))
      g_write_csus.PushBack(csu);
  }

  for (size_t ind = 0; ind < g_query_blocks.Size(); ind++) {
    BlockCFG *cfg = g_query_blocks[ind];
    if (found_blocks.Contains(cfg->GetId()))
      g_write_blocks.PushBack(cfg);
  }

  g_query_csus.Clear();
  g_query_blocks.Clear();
  g_data_buf.Reset();
}

// write out the contents of data_buf to t as the results of processing
// some number of CSUs and/or blocks.
static void ProcessWriteList(Transaction *t)
{
  TOperand *list_op = TOperandString::Compress(t, &g_data_buf);

  BACKEND_CALL(BlockWriteList, 0);
  call->PushArgument(list_op);
  t->PushAction(call);

  SubmitTransaction(t);
  t->Clear();

  g_data_buf.Reset();
}

extern "C" void XIL_WriteGenerated()
{
  Transaction *t = new Transaction();

  if (g_has_annotation) {
    // if we are making an annotation CFG there should be only one CFG
    // and no CSUs.
    Assert(g_keep_csus.Empty());
    Assert(g_keep_cfgs.Size() <= 1);

    if (!g_keep_cfgs.Empty() && g_drop_cfgs.Empty()) {
      BlockCFG *cfg = g_keep_cfgs[0];

      // do loop splitting (well, check for loops and eliminate skips).
      Vector<BlockCFG*> split_cfgs;
      SplitLoops(cfg, &split_cfgs);

      // if there were actually loops, only write out the non-loop CFG.
      BlockCFG::Write(&g_data_buf, split_cfgs.Back());

      TOperand *data = TOperandString::Compress(t, &g_data_buf);
      g_data_buf.Reset();

      BACKEND_CALL(BlockWriteAnnot, 0);
      call->PushArgument(data);
      t->PushAction(call);

      SubmitTransaction(t);
    }

    delete t;
    AnalysisCleanup();
    return;
  }

  for (size_t ind = 0; ind < g_keep_csus.Size(); ind++) {
    CompositeCSU *csu = g_keep_csus[ind];

    if (g_drop_csus.Contains(csu))
      continue;

    g_query_csus.PushBack(csu);
    String::WriteWithTag(&g_data_buf, csu->GetName(), TAG_Name);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessQueryList(t);
  }

  for (size_t ind = 0; ind < g_keep_cfgs.Size(); ind++) {
    BlockCFG *cfg = g_keep_cfgs[ind];

    if (g_drop_cfgs.Contains(cfg))
      continue;

    g_query_blocks.PushBack(cfg);
    BlockId::Write(&g_data_buf, cfg->GetId());

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessQueryList(t);
  }

  if (g_data_buf.pos != g_data_buf.base)
    ProcessQueryList(t);
  Assert(g_data_buf.pos == g_data_buf.base);

  for (size_t ind = 0; ind < g_write_csus.Size(); ind++) {
    CompositeCSU::Write(&g_data_buf, g_write_csus[ind]);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessWriteList(t);
  }

  // use our global lists to remember all escape/callgraph info generated.
  SetStaticMergeCaches(&g_escape_edges, &g_escape_accesses, &g_call_edges);

  // the CSU database is empty but all the CSUs the escape analysis will
  // need are still in memory.
  EscapeUseLocalCSUs();

  for (size_t ind = 0; ind < g_write_blocks.Size(); ind++) {
    BlockCFG *cfg = g_write_blocks[ind];

    // do loop splitting.
    Vector<BlockCFG*> split_cfgs;
    SplitLoops(cfg, &split_cfgs);

    // remember the direct callees of this function.
    Vector<Variable*> callees;

    // do callgraph and escape processing.
    for (size_t cind = 0; cind < split_cfgs.Size(); cind++) {
      EscapeProcessCFG(split_cfgs[cind]);
      CallgraphProcessCFG(split_cfgs[cind], &callees);
    }

    // add the direct call edges to the callgraph hash. only do this for
    // functions; ignore calls from global variable initializers.
    if (cfg->GetId()->Kind() == B_FunctionWhole) {
      TOperand *key_arg =
        new TOperandString(t, cfg->GetId()->Function()->Value());
      for (size_t cind = 0; cind < callees.Size(); cind++) {
        const char *name = callees[cind]->GetName()->Value();
        TOperand *callee_arg = new TOperandString(t, name);

        t->PushAction(
          Backend::HashInsertValue(t, CALLGRAPH_HASH, key_arg, callee_arg));
      }
    }

    Assert(split_cfgs.Size());
    WriteUInt32(&g_data_buf, split_cfgs.Size());

    for (size_t cind = 0; cind < split_cfgs.Size(); cind++)
      BlockCFG::Write(&g_data_buf, split_cfgs[cind]);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessWriteList(t);
  }

  for (size_t ind = 0; ind < g_escape_edges.Size(); ind++) {
    EscapeEdgeSet *eset = g_escape_edges[ind];
    EscapeEdgeSet::Write(&g_data_buf, eset);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessWriteList(t);
  }

  for (size_t ind = 0; ind < g_escape_accesses.Size(); ind++) {
    EscapeAccessSet *aset = g_escape_accesses[ind];
    EscapeAccessSet::Write(&g_data_buf, aset);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessWriteList(t);
  }

  for (size_t ind = 0; ind < g_call_edges.Size(); ind++) {
    CallEdgeSet *cset = g_call_edges[ind];
    CallEdgeSet::Write(&g_data_buf, cset);

    if (TRANSACTION_DATA_EXCEEDED)
      ProcessWriteList(t);
  }
  
  if (g_data_buf.pos != g_data_buf.base)
    ProcessWriteList(t);
  Assert(g_data_buf.pos == g_data_buf.base);

  delete t;
  AnalysisCleanup();
}

extern "C"
int XIL_HasAnnotation(XIL_Var var, const char *annot_name, int annot_type)
{
  GET_OBJECT(Variable, var);

  const char *db_name = NULL;
  if (annot_type)
    db_name = COMP_ANNOT_DATABASE;
  else if (new_var->Kind() == VK_Func)
    db_name = BODY_ANNOT_DATABASE;
  else if (new_var->Kind() == VK_Glob)
    db_name = INIT_ANNOT_DATABASE;
  else
    Assert(false);

  Transaction *t = new Transaction();
  size_t result = t->MakeVariable(true);

  BACKEND_CALL(BlockQueryAnnot, result);
  call->PushArgument(new TOperandString(t, db_name));
  call->PushArgument(new TOperandString(t, new_var->GetName()->Value()));
  call->PushArgument(new TOperandString(t, annot_name));
  t->PushAction(call);

  SubmitTransaction(t);

  TOperandBoolean *data = t->LookupBoolean(result);
  int has_data = data->IsTrue();

  delete t;
  return has_data;
}

extern "C"
void XIL_AddAnnotationMsg(XIL_Var var, const char *annot_name, int annot_type,
                          XIL_Location loc, const char *annot_message)
{
  GET_OBJECT(Variable, var);
  GET_OBJECT(Location, loc);
  Assert(!g_has_annotation);

  BlockId *cfg_id = NULL;
  String *new_name = String::Make(annot_name);

  if (annot_type) {
    Assert(new_var->Kind() == VK_Glob);
    cfg_id = BlockId::Make(B_AnnotationComp, new_var, new_name);
  }
  else if (new_var->Kind() == VK_Func) {
    cfg_id = BlockId::Make(B_AnnotationFunc, new_var, new_name);
  }
  else if (new_var->Kind() == VK_Glob) {
    cfg_id = BlockId::Make(B_AnnotationInit, new_var, new_name);
  }

  BlockCFG *cfg = BlockCFG::Make(cfg_id);

  // make a single local variable '__error__'.
  cfg_id->IncRef();
  String *error_name = String::Make("__error__");
  Variable *error_var = Variable::Make(cfg_id, VK_Local, error_name, 0, NULL);
  cfg->AddVariable(error_var, Type::MakeError());

  new_loc->IncRef();
  new_loc->IncRef();
  cfg->SetBeginLocation(new_loc);
  cfg->SetEndLocation(new_loc);

  new_loc->IncRef();
  new_loc->IncRef();
  PPoint entry_point = cfg->AddPoint(new_loc);
  PPoint exit_point = cfg->AddPoint(new_loc);

  cfg->SetEntryPoint(entry_point);
  cfg->SetExitPoint(exit_point);

  Exp *error_exp = Exp::MakeVar(error_var);

  String *new_message = String::Make(annot_message);
  Exp *message_exp = Exp::MakeString(new_message);

  PEdge *edge = PEdge::MakeAssign(entry_point, exit_point, Type::MakeError(),
                                  error_exp, message_exp);
  cfg->AddEdge(edge);

  Transaction *t = new Transaction();

  Assert(g_data_buf.pos == g_data_buf.base);
  BlockCFG::Write(&g_data_buf, cfg);

  TOperand *data = TOperandString::Compress(t, &g_data_buf);
  g_data_buf.Reset();

  BACKEND_CALL(BlockWriteAnnot, 0);
  call->PushArgument(data);
  t->PushAction(call);

  SubmitTransaction(t);
  delete t;
}
