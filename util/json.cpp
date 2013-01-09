
// Sixgill: Static assertion checker for C/C++ programs.
// Copyright (C) 2013  Mozilla Corporation
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

#include "buffer.h"

#include <imlang/block.h>
#include <imlang/bit.h>
#include <memory/serial.h>

NAMESPACE_XGILL_BEGIN

static const uint32_t TAG_LIMIT = 4000;
static const char *g_tag_names[TAG_LIMIT];

static inline const char *TagName(tag_t outer, tag_t inner)
{
  Assert(inner < TAG_LIMIT);
  if (g_tag_names[inner])
    return g_tag_names[inner];

  switch (inner) {
  case TAG_String:
    switch (outer) {
    case TAG_BlockId:
      return "Loop";
    case TAG_Exp:
      return "String";
    default:
      logout << "*** ERROR *** Unknown name for inner string tag: " << TagName(0, outer) << endl;
      Assert(false);
    }
  case TAG_Int32:
  case TAG_UInt32:
  case TAG_UInt64:
    switch (outer) {
    case TAG_Location:
      return "Line";
    case TAG_Exp:
      return "Number";
    default:
      logout << "*** ERROR *** Unknown name for inner integer tag: " << TagName(0, outer) << endl;
      Assert(false);
    }
  default:
    logout << "*** ERROR *** Unknown unnamed tag: " << inner << endl;
    Assert(false);
  }
}

static void FillTagNames()
{
#define ADD_TAG(NAME)				\
  Assert(TAG_ ## NAME < TAG_LIMIT);		\
  g_tag_names[TAG_ ## NAME] = #NAME

  ADD_TAG(Bit);
  ADD_TAG(Type);
  ADD_TAG(TypeFunctionVarArgs);
  ADD_TAG(TypeFunctionArguments);
  ADD_TAG(TypeFunctionCSU);
  ADD_TAG(CompositeCSU);
  ADD_TAG(CSUBaseClass);
  ADD_TAG(Command);
  ADD_TAG(Field);
  ADD_TAG(FieldCSU);
  ADD_TAG(FieldInstanceFunction);
  ADD_TAG(DataField);
  ADD_TAG(FunctionField);
  ADD_TAG(Variable);
  ADD_TAG(Exp);
  ADD_TAG(ExpUnsigned);
  ADD_TAG(Trace);
  ADD_TAG(BlockId);
  ADD_TAG(BlockPPoint);
  ADD_TAG(Version);
  ADD_TAG(DefineVariable);
  ADD_TAG(PPoint);
  ADD_TAG(LoopHead);
  ADD_TAG(LoopIsomorphic);
  ADD_TAG(PointAnnotation);
  ADD_TAG(PEdge);
  ADD_TAG(PEdgeAssumeNonZero);
  ADD_TAG(PEdgeCallArguments);
  ADD_TAG(PEdgeCallInstance);
  ADD_TAG(BlockCFG);
  ADD_TAG(CallEdgeSet);
  ADD_TAG(CallEdge);
  ADD_TAG(EscapeEdgeSet);
  ADD_TAG(EscapeEdge);
  ADD_TAG(EscapeEdgeMoveCaller);
  ADD_TAG(EscapeEdgeMoveCallee);
  ADD_TAG(EscapeAccessSet);
  ADD_TAG(EscapeAccess);
  ADD_TAG(BlockMemory);
  ADD_TAG(MemoryKindSimplify);
  ADD_TAG(MemoryKindAlias);
  ADD_TAG(MemoryKindClobber);
  ADD_TAG(MemoryGCEntry);
  ADD_TAG(MemoryGuardEntry);
  ADD_TAG(MemoryAssumeEntry);
  ADD_TAG(MemoryReturnEntry);
  ADD_TAG(MemoryTargetEntry);
  ADD_TAG(MemoryAssignEntry);
  ADD_TAG(MemoryArgumentEntry);
  ADD_TAG(MemoryClobberEntry);
  ADD_TAG(BlockModset);
  ADD_TAG(ModsetEntry);
  ADD_TAG(ModsetAssign);
  ADD_TAG(ModsetCanGC);
  ADD_TAG(BlockSummary);
  ADD_TAG(SummaryAssert);
  ADD_TAG(SummaryAssume);
  ADD_TAG(Kind);
  ADD_TAG(OpCode);
  ADD_TAG(Width);
  ADD_TAG(Offset);
  ADD_TAG(Count);
  ADD_TAG(Index);
  ADD_TAG(Sign);
  ADD_TAG(Name);
  ADD_TAG(True);
  ADD_TAG(False);
  ADD_TAG(Hash);
  ADD_TAG(CacheString);
  ADD_TAG(Location);

#undef ADD_TAG
}

static inline bool IsPrimitiveTag(tag_t tag)
{
  switch (tag) {
  case TAG_Kind:
  case TAG_Name:
  case TAG_Version:
  case TAG_Command:
  case TAG_Width:
  case TAG_Sign:
  case TAG_Index:
  case TAG_OpCode:
  case TAG_PEdgeAssumeNonZero:
  case TAG_Count:
    return true;
  default:
    return false;
  }
}

static inline bool CanHaveMultipleInnerTags(tag_t outer, tag_t inner)
{
  switch (outer) {
  case TAG_Variable:
  case TAG_Field:
    return inner == TAG_Name;
  case TAG_BlockCFG:
    switch (inner) {
    case TAG_Location:
    case TAG_DefineVariable:
    case TAG_PPoint:
    case TAG_PEdge:
    case TAG_Index:
    case TAG_LoopIsomorphic:
      return true;
    default:
      return false;
    }
  case TAG_TypeFunctionArguments:
    return inner == TAG_Type;
  case TAG_PEdgeCallArguments:
    return inner == TAG_Exp;
  case TAG_PEdge:
    switch (inner) {
    case TAG_Index:
    case TAG_Exp:
      return true;
    default:
      return false;
    }
  case TAG_Exp:
    return inner == TAG_Exp;
  default:
    return false;
  }
}

static inline bool IgnoreRepeatedTag(tag_t outer, tag_t inner)
{
  switch (outer) {
  case TAG_Variable:
    return inner == TAG_Name;
  case TAG_Exp:
    return inner == TAG_Exp;
  default:
    return false;
  }
}

static inline const char *ChangeInt(uint32_t val, tag_t outer, tag_t inner)
{
#define SWITCH_CASE(NAME, VAL)			\
  case VAL: return # NAME;

#define SWITCH(MACRO)				\
  switch (val) {				\
    MACRO(SWITCH_CASE)				\
  default:					\
    logout << "Unexpected value";		\
  }

  // Hack for distinguishing unops/binops when printing.
  static uint32_t last_exp_kind = 0;

  switch (inner) {
  case TAG_Kind:
    switch (outer) {
    case TAG_Variable:
      SWITCH(ITERATE_VARIABLE_KINDS)
    case TAG_Exp:
      last_exp_kind = val;
      SWITCH(ITERATE_EXP_KINDS)
    case TAG_Type:
      SWITCH(ITERATE_TYPE_KINDS)
    case TAG_BlockId:
      SWITCH(ITERATE_BLOCK_KINDS)
    case TAG_PEdge:
      SWITCH(ITERATE_EDGE_KINDS)
    default:
      logout << "*** ERROR *** No readable kind for " << TagName(0, outer) << endl;
      Assert(false);
    }
  case TAG_OpCode:
    switch (outer) {
    case TAG_Exp:
      switch (last_exp_kind) {
      case EK_Unop:
	SWITCH(XIL_ITERATE_UNOP)
      case EK_Binop:
	SWITCH(XIL_ITERATE_BINOP)
      default:
	Assert(false);
      }
    default:
      logout << "*** ERROR *** No readable opcode for " << TagName(0, outer) << endl;
      Assert(false);
    }
  default:
    return NULL;
  }

#undef SWITCH_CASE
#undef SWITCH
}

static bool PrintJSONTag(Buffer *buf, int pad_spaces, tag_t outer = 0, tag_t inner = 0)
{
  int32_t val;
  uint32_t uval;
  uint64_t luval;
  tag_t tag;

  const uint8_t *str_base = NULL;
  size_t str_len = 0;

  if (ReadString(buf, &str_base, &str_len)) {
    logout << "\"";
    if (!str_base[str_len - 1])
      str_len--;
    PrintString(logout, str_base, str_len);
    logout << "\"";
  }
  else if (ReadInt32(buf, &val)) {
    logout << val;
  }
  else if (ReadUInt32(buf, &uval)) {
    if (const char *str = ChangeInt(uval, outer, inner))
      logout << "\"" << str << "\"";
    else
      logout << uval;
  }
  else if (ReadUInt64(buf, &luval)) {
    logout << luval;
  }
  else if ((tag = PeekOpenTag(buf))) {
    if (IsPrimitiveTag(tag)) {
      ReadOpenTag(buf, tag);
      if (PeekOpenTag(buf)) {
	if (!PrintJSONTag(buf, 0, outer, tag))
	  return false;
      } else {
	logout << "true";
      }
      ReadCloseTag(buf, tag);
      return true;
    }

    if (tag == TAG_CacheString) {
      String *str = String::ReadCache(buf);
      logout << "\"";
      PrintString(logout, (const uint8_t*) str->Value(), strlen(str->Value()));
      logout << "\"";
      return true;
    }

    logout << "{" << endl;

    Vector<tag_t> inner_seen;
    tag_t inner_tag;

    ReadOpenTag(buf, tag);
    while (!ReadCloseTag(buf, tag)) {
      if ((inner_tag = PeekOpenTag(buf))) {
	if (!inner_seen.Empty())
	  logout << "," << endl;

	PrintPadding(pad_spaces);

	if (inner_seen.Contains(inner_tag) && !IgnoreRepeatedTag(tag, inner_tag)) {
	  logout << "*** ERROR *** Duplicate inner tag: "
		 << TagName(0, tag) << " " << TagName(tag, inner_tag) << endl;
	  Assert(!inner_seen.Contains(inner_tag));
	}
	inner_seen.PushBack(inner_tag);
	logout << "\"" << TagName(tag, inner_tag) << "\": ";

	if (CanHaveMultipleInnerTags(tag, inner_tag)) {
	  logout << "[" << endl;
	  PrintPadding(pad_spaces + 2);
	  if (!PrintJSONTag(buf, pad_spaces + 2, tag))
	    return false;
	  while (PeekOpenTag(buf) == inner_tag) {
	    logout << "," << endl;
	    PrintPadding(pad_spaces + 2);
	    if (!PrintJSONTag(buf, pad_spaces + 2, tag))
	      return false;
	  }
	  logout << endl;
	  PrintPadding(pad_spaces + 1);
	  logout << "]";
	} else {
	  if (!PrintJSONTag(buf, pad_spaces + 1, tag))
	    return false;
	}
      } else {
	if (!PrintJSONTag(buf, pad_spaces + 1, tag))
	  return false;
      }
    }

    logout << endl;
    PrintPadding(pad_spaces);
    logout << "}";
  }
  else {
    return false;
  }
  return true;
}

void PrintJSONBuffer(Buffer *buf)
{
  if (!g_tag_names[TAG_Kind])
    FillTagNames();

  Buffer newbuf(buf->base, buf->size);

  logout << "[";
  while (newbuf.pos != newbuf.base + newbuf.size) {
    if (newbuf.pos != newbuf.base)
      logout << ",";
    if (!PrintJSONTag(&newbuf, 0)) {
      logout << "ERROR: Buffer parse failed" << endl;
      return;
    }
  }
  logout << "]";
}

NAMESPACE_XGILL_END
