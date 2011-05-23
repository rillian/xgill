
struct JSObject { int f; };
void js_GC();

void callGC(bool when) {
  if (when)
    js_GC();
}

struct Heap {
  JSObject *obj;
};

void good1(Heap *h)
{
  callGC(true);
  h->obj->f = 0;
}

void good2(Heap *h)
{
  JSObject *obj = h->obj;
  callGC(false);
  obj->f = 0;
}

void bad(Heap *h)
{
  JSObject *obj = h->obj;
  callGC(true);
  obj->f = 0;
}
