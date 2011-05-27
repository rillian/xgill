
struct JSContext;
struct JSObject { int f; };

void js_GC() {}

namespace js {

struct AutoObjectRooter
{
  JSObject *&obj;
  AutoObjectRooter(JSContext *cx, JSObject *&obj) : obj(obj) {}
};

struct AutoRootedObject
{
  JSObject *obj;
  AutoObjectRooter root;
  AutoRootedObject(JSContext *cx, JSObject *value) : obj(value), root(cx, obj) {}

  operator JSObject * () { return obj; }
  JSObject * operator ->() { return obj; }
};

void bad(JSObject *obj)
{
  js_GC();
  obj->f = 0;
}

void good(JSContext *cx, JSObject *obj)
{
  AutoObjectRooter root(cx, obj);
  js_GC();
  obj->f = 0;
}

void split(JSContext *cx, JSObject *obj)
{
  AutoRootedObject obj2(cx, obj);
  js_GC();

  obj->f = 0;
  obj2->f = 0;
}

void other_split(JSContext *cx, JSObject *obj)
{
  AutoRootedObject obj2(cx, obj);
  js_GC();

  bad(obj2);
}

}
