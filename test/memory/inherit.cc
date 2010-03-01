
// handling of multiple inheritance.

class A
{
public:
  virtual int a1() { return 0; }
  virtual int a2() = 0;
};

class B
{
public:
  virtual int b1() { return 2; }
  virtual int b2() { return 3; }
  // virtual int a1() { return 10000; }
};

class C : public A, public B
{
public:
  int a2() { return 4; }
  int b2() { return 5; }
  virtual int c1() { return 6; }
};

void callA(A *a)
{
  a->a1();
  a->a2();
}

void callB(B *b)
{
  b->b1();
  b->b2();
}

void callC(C *c)
{
  c->a1();
  c->a2();
  c->b1();
  c->b2();
  c->c1();
  callA(c);
  callB(c);
}

void start()
{
  B b;
  C c;
  callB(&b);
  callC(&c);
}
