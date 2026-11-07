/* Link shim for clang-cl/lld cross builds: things MSVC provides as intrinsics or CRT startup objects. */
#include <stddef.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
u8  __inbyte (u16 p){ u8 v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
u16 __inword (u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
u32 __indword(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
void __outbyte (u16 p,u8 v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
void __outword (u16 p,u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
void __outdword(u16 p,u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
/* Static cookie: acceptable for bring-up builds only. Release builds should seed it in DriverEntry. */
u64 __security_cookie = 0x00002B992DDFA232ULL;
