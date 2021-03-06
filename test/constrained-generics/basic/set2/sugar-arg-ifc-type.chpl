/*
This is basic-arg-query.chpl
with the CG formal 'cgArg' is given the interface name
as if it were a type.
*/

interface IFC(T) {
  proc reqFun(cgFormal: T, formal2: int);
  proc dfltImpl(cgFormal: T, formal2: int) {
    writeln("in IFC.dfltImpl()");
  }
}

proc cgFun(cgArg: IFC, arg2: int) {
  writeln("in cgFun()");
  reqFun(cgArg, arg2);
  dfltImpl(cgArg, arg2);
}

proc reqFun(arg1: real, arg2: int) {
  writeln("in reqFun/real*int", (arg1, arg2));
}

implements IFC(real); // relies on the above reqFun(real)

cgFun(2.3, 4);
