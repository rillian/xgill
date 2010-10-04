
#include "../sixgill.h"

// annotation issues on templates.

template <typename TEMPLATE>
struct str
{
  static int SizeOfType() { return sizeof(TEMPLATE); }

  //invariant(field == SizeOfType())
  invariant(field == sizeof(TEMPLATE))
  int field;
};

int foo()
{
  str<double> s;
  return s.SizeOfType();
}
