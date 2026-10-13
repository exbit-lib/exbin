// Minimal compiler-rt piece (no wasm builtins lib available): 128-bit multiply.
typedef unsigned long long u64; typedef long long i64;
typedef struct { u64 lo; i64 hi; } i128s;
i128s __multi3(u64 alo, i64 ahi, u64 blo, i64 bhi) {
  u64 a0=alo&0xffffffffu, a1=alo>>32, b0=blo&0xffffffffu, b1=blo>>32;
  u64 p00=a0*b0, p01=a0*b1, p10=a1*b0, p11=a1*b1;
  u64 mid=(p00>>32)+(p01&0xffffffffu)+(p10&0xffffffffu);
  u64 lo=(p00&0xffffffffu)|(mid<<32);
  u64 hi=p11+(p01>>32)+(p10>>32)+(mid>>32)+alo*(u64)bhi+(u64)ahi*blo;
  i128s r={lo,(i64)hi}; return r;
}
