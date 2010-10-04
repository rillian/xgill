
#include "../sixgill.h"

// annotation issues on templates.

template <typename TEMPLATE>
struct str
{
  typedef TEMPLATE ThisType;
  static int SizeOfType() { return sizeof(TEMPLATE); }

  //invariant(field == SizeOfType())
  //invariant(field == sizeof(TEMPLATE))
  invariant(field == sizeof(ThisType))
  int field;
};

int foo()
{
  str<double> s;
  return s.SizeOfType();
}
