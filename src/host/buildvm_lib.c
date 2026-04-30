/*
** LuaJIT VM builder: library definition compiler.
** Copyright (C) 2005-2025 Mike Pall. See Copyright Notice in luajit.h
*/

#include "buildvm.h"
#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_bcdump.h"
#include "lj_lib.h"
#include "buildvm_libbc.h"

/* Context for library definitions. */
static uint8_t obuf[8192];
static uint8_t *optr;
static char modname[80];
static size_t modnamelen;
static char funcname[80];
static int modstate, regfunc;
static int ffid, recffid, ffasmfunc;

enum {
  REGFUNC_OK,
  REGFUNC_NOREG,
  REGFUNC_NOREGUV
};

static void libdef_name(const char *p, int kind)
{
  size_t n = strlen(p);
  if (kind != LIBINIT_STRING) {
    if (n > modnamelen && p[modnamelen] == '_' &&
	!strncmp(p, modname, modnamelen)) {
      p += modnamelen+1;
      n -= modnamelen+1;
    }
  }
  if (n > LIBINIT_MAXSTR) {
    fprintf(stderr, "Error: string too long: '%s'\n",  p);
    exit(1);
  }
  if (optr+1+n+2 > obuf+sizeof(obuf)) {  /* +2 for caller. */
    fprintf(stderr, "Error: output buffer overflow\n");
    exit(1);
  }
  *optr++ = (uint8_t)(n | kind);
  memcpy(optr, p, n);
  optr += n;
}

static void libdef_endmodule(BuildCtx *ctx)
{
  if (modstate != 0) {
    char line[80];
    const uint8_t *p;
    int n;
    if (modstate == 1)
      fprintf(ctx->fp, "  (lua_CFunction)0");
    fprintf(ctx->fp, "\n};\n");
    fprintf(ctx->fp, "static const uint8_t %s%s[] = {\n",
	    LABEL_PREFIX_LIBINIT, modname);
    line[0] = '\0';
    for (n = 0, p = obuf; p < optr; p++) {
      n += sprintf(line+n, "%d,", *p);
      if (n >= 75) {
	fprintf(ctx->fp, "%s\n", line);
	n = 0;
	line[0] = '\0';
      }
    }
    fprintf(ctx->fp, "%s%d\n};\n#endif\n\n", line, LIBINIT_END);
  }
}

static void libdef_module(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(arg);
  if (ctx->mode == BUILD_libdef) {
    libdef_endmodule(ctx);
    optr = obuf;
    *optr++ = (uint8_t)ffid;
    *optr++ = (uint8_t)ffasmfunc;
    *optr++ = 0;  /* Hash table size. */
    modstate = 1;
    fprintf(ctx->fp, "#ifdef %sMODULE_%s\n", LIBDEF_PREFIX, p);
    fprintf(ctx->fp, "#undef %sMODULE_%s\n", LIBDEF_PREFIX, p);
    fprintf(ctx->fp, "static const lua_CFunction %s%s[] = {\n",
	    LABEL_PREFIX_LIBCF, p);
  }
  modnamelen = strlen(p);
  if (modnamelen > sizeof(modname)-1) {
    fprintf(stderr, "Error: module name too long: '%s'\n", p);
    exit(1);
  }
  strcpy(modname, p);
}

static int find_ffofs(BuildCtx *ctx, const char *name)
{
  int i;
  for (i = 0; i < ctx->nglob; i++) {
    const char *gl = ctx->globnames[i];
    if (gl[0] == 'f' && gl[1] == 'f' && gl[2] == '_' && !strcmp(gl+3, name)) {
      return (int)((uint8_t *)ctx->glob[i] - ctx->code);
    }
  }
  fprintf(stderr, "Error: undefined fast function %s%s\n",
	  LABEL_PREFIX_FF, name);
  exit(1);
}

static void libdef_func(BuildCtx *ctx, char *p, int arg)
{
  if (arg != LIBINIT_CF)
    ffasmfunc++;
  if (ctx->mode == BUILD_libdef) {
    if (modstate == 0) {
      fprintf(stderr, "Error: no module for function definition %s\n", p);
      exit(1);
    }
    if (regfunc == REGFUNC_NOREG) {
      if (optr+1 > obuf+sizeof(obuf)) {
	fprintf(stderr, "Error: output buffer overflow\n");
	exit(1);
      }
      *optr++ = LIBINIT_FFID;
    } else {
      if (arg != LIBINIT_ASM_) {
	if (modstate != 1) fprintf(ctx->fp, ",\n");
	modstate = 2;
	fprintf(ctx->fp, "  %s%s", arg ? LABEL_PREFIX_FFH : LABEL_PREFIX_CF, p);
      }
      if (regfunc != REGFUNC_NOREGUV) obuf[2]++;  /* Bump hash table size. */
      libdef_name(regfunc == REGFUNC_NOREGUV ? "" : p, arg);
    }
  } else if (ctx->mode == BUILD_ffdef) {
    fprintf(ctx->fp, "FFDEF(%s)\n", p);
  } else if (ctx->mode == BUILD_recdef) {
    if (strlen(p) > sizeof(funcname)-1) {
      fprintf(stderr, "Error: function name too long: '%s'\n", p);
      exit(1);
    }
    strcpy(funcname, p);
  } else if (ctx->mode == BUILD_vmdef) {
    int i;
    for (i = 1; p[i] && modname[i-1]; i++)
      if (p[i] == '_') p[i] = '.';
    fprintf(ctx->fp, "\"%s\",\n", p);
  } else if (ctx->mode == BUILD_bcdef) {
    if (arg != LIBINIT_CF)
      fprintf(ctx->fp, ",\n%d", find_ffofs(ctx, p));
  }
  ffid++;
  regfunc = REGFUNC_OK;
}

static uint8_t *libdef_uleb128(uint8_t *p, uint32_t *vv)
{
  uint32_t v = *p++;
  if (v >= 0x80) {
    int sh = 0; v &= 0x7f;
    do { v |= ((*p & 0x7f) << (sh += 7)); } while (*p++ >= 0x80);
  }
  *vv = v;
  return p;
}

/*
** Convert one 32-bit (legacy) bytecode dump segment into the new 64-bit
** layout used by the runtime, writing the result starting at `dst` and
** returning the total number of bytes written.
**
** Source layout (per instruction, 4 bytes, little-endian):
**   byte0 = OP, byte1 = A, byte2 = C, byte3 = B    (ABC form)
**   byte0 = OP, byte1 = A, byte2 = D_lo, byte3 = D_hi  (AD form)
**
** Destination layout (per instruction, 8 bytes, little-endian):
**   bytes 0..1 : OP (8b) + pad (8b)
**   bytes 2..3 : A   (16b)
**   bytes 4..5 : C / D_lo (16b)
**   bytes 6..7 : B / D_hi (16b)
**
** For AD form (KSHORT/JMP/...), the legacy 16-bit D is stored as a
** signed 16-bit value relative to BCBIAS_J=0x8000. The new layout uses
** 32-bit D relative to BCBIAS_J=0x80000000. To preserve semantics:
**   D_new = (int32_t)(int16_t)D_old + 0x80000000 - 0x8000
** i.e. unbias against the old bias and re-bias against the new bias.
*/

/* Build a local "is AD-form" table from BCDEF, using the convention that
** AD-form opcodes are exactly those whose B-field operand mode (mb) is
** `___` (i.e. unused). This matches lj_bc.h's BCMnone in that slot.
**
** For AD-form opcodes we *also* need to know whether the D field is a
** jump offset, because only then is the on-disk value biased by
** BCBIAS_J and must be re-biased when widening from 16 to 32 bits. The
** companion table libdef_isjump records that.
*/
#define LIBDEF_BCMODE_AD____   1   /* mb == ___ -> AD form */
#define LIBDEF_BCMODE_AD_dst   0
#define LIBDEF_BCMODE_AD_base  0
#define LIBDEF_BCMODE_AD_var   0
#define LIBDEF_BCMODE_AD_rbase 0
#define LIBDEF_BCMODE_AD_uv    0
#define LIBDEF_BCMODE_AD_lit   0
#define LIBDEF_BCMODE_AD_lits  0
#define LIBDEF_BCMODE_AD_pri   0
#define LIBDEF_BCMODE_AD_num   0
#define LIBDEF_BCMODE_AD_str   0
#define LIBDEF_BCMODE_AD_tab   0
#define LIBDEF_BCMODE_AD_func  0
#define LIBDEF_BCMODE_AD_jump  0
#define LIBDEF_BCMODE_AD_cdata 0

static const uint8_t libdef_isad[] = {
#define BCISAD(name, ma, mb, mc, mt)  LIBDEF_BCMODE_AD_##mb,
  BCDEF(BCISAD)
#undef BCISAD
  0
};

/* Whether the D field (mc when mb==___) is a jump offset. Only true for
** opcodes whose mc == jump. We re-use the same infrastructure: define
** _jump = 1 and everything else = 0 for the C-field slot.
*/
#define LIBDEF_BCMC_JUMP____   0
#define LIBDEF_BCMC_JUMP_dst   0
#define LIBDEF_BCMC_JUMP_base  0
#define LIBDEF_BCMC_JUMP_var   0
#define LIBDEF_BCMC_JUMP_rbase 0
#define LIBDEF_BCMC_JUMP_uv    0
#define LIBDEF_BCMC_JUMP_lit   0
#define LIBDEF_BCMC_JUMP_lits  0
#define LIBDEF_BCMC_JUMP_pri   0
#define LIBDEF_BCMC_JUMP_num   0
#define LIBDEF_BCMC_JUMP_str   0
#define LIBDEF_BCMC_JUMP_tab   0
#define LIBDEF_BCMC_JUMP_func  0
#define LIBDEF_BCMC_JUMP_jump  1
#define LIBDEF_BCMC_JUMP_cdata 0

static const uint8_t libdef_isjump[] = {
#define BCISJ(name, ma, mb, mc, mt)  LIBDEF_BCMC_JUMP_##mc,
  BCDEF(BCISJ)
#undef BCISJ
  0
};

static MSize libdef_expand_dump(uint8_t *dst, const uint8_t *src, MSize srclen)
{
  const uint8_t *send = src + srclen;
  uint8_t *d = dst;
  uint32_t flags, numparams, framesize, sizeuv;
  uint32_t sizekgc, sizekn, sizebc, sizedbg = 0;
  const uint8_t *bc_end;
  MSize i;

  /* Copy 4-byte prototype header verbatim. */
  flags     = *src++;  *d++ = (uint8_t)flags;
  numparams = *src++;  *d++ = (uint8_t)numparams;
  framesize = *src++;  *d++ = (uint8_t)framesize;
  sizeuv    = *src++;  *d++ = (uint8_t)sizeuv;

  /* Copy ULEB128 fields: sizekgc, sizekn, sizebc (and optionally sizedbg). */
#define COPY_ULEB(out_var)                                  \
  do {                                                      \
    uint32_t _v = 0; int _sh = 0;                           \
    uint8_t _b;                                             \
    do { _b = *src++; *d++ = _b;                            \
         _v |= (uint32_t)(_b & 0x7f) << _sh; _sh += 7;      \
    } while (_b & 0x80);                                    \
    (out_var) = _v;                                         \
  } while (0)

  COPY_ULEB(sizekgc);
  COPY_ULEB(sizekn);
  COPY_ULEB(sizebc);
  /* Library function dumps are always written with BCDUMP_F_STRIP
  ** (see lj_lib.c::lib_read_lfunc), so no debug-info fields follow.
  */
#undef COPY_ULEB

  (void)numparams; (void)framesize; (void)sizeuv;
  (void)sizekgc; (void)sizekn; (void)sizedbg;

  /* Expand sizebc instructions from 4 bytes to 8 bytes. */
  bc_end = src + (size_t)sizebc * 4;
  for (i = 0; i < sizebc; i++, src += 4) {
    uint8_t op = src[0];
    uint8_t a  = src[1];
    uint8_t c  = src[2];
    uint8_t b  = src[3];
    int is_ad   = (op < (uint8_t)BC__MAX) && libdef_isad[op];
    int is_jump = (op < (uint8_t)BC__MAX) && libdef_isjump[op];
    /* DUALNUM type fixup, mirroring legacy libdef_fixupbc. */
    if (!LJ_DUALNUM && op == BC_ISTYPE && c == (uint8_t)(~LJ_TNUMX+1)) {
      op = BC_ISNUM; c++;
    }
    /* Write OP + pad. */
    d[0] = op;
    d[1] = 0;
    /* Write A (low byte; high byte zeroed). */
    d[2] = a;
    d[3] = 0;
    if (is_ad) {
      uint16_t d16 = (uint16_t)(c | ((uint16_t)b << 8));
      uint32_t d32;
      if (is_jump) {
	/* Re-bias the jump offset:
	**   real offset = (uint16_t)D_old - 0x8000   (signed result)
	**   D_new       = real offset + 0x80000000   (re-biased)
	** d16 is treated as unsigned 16-bit (the legacy on-disk encoding).
	*/
	int32_t off = (int32_t)d16 - 0x8000;
	d32 = (uint32_t)(off + (int32_t)BCBIAS_J);
      } else {
	/* Plain unsigned 16-bit D (var/lit/pri/str/tab/...). Zero-extend. */
	d32 = (uint32_t)d16;
      }
      d[4] = (uint8_t)(d32        & 0xff);
      d[5] = (uint8_t)((d32 >> 8)  & 0xff);
      d[6] = (uint8_t)((d32 >> 16) & 0xff);
      d[7] = (uint8_t)((d32 >> 24) & 0xff);
    } else {
      d[4] = c;  d[5] = 0;
      d[6] = b;  d[7] = 0;
    }
    d += 8;
  }
  src = bc_end;

  /* Copy the remainder verbatim (upvalues, kgc, knum, debug info). */
  while (src < send) *d++ = *src++;

  return (MSize)(d - dst);
}

static void libdef_lua(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(arg);
  if (ctx->mode == BUILD_libdef) {
    int i;
    for (i = 0; libbc_map[i].name != NULL; i++) {
      if (!strcmp(libbc_map[i].name, p)) {
	int ofs = libbc_map[i].ofs;
	int len = libbc_map[i+1].ofs - ofs;
	MSize newlen;
	obuf[2]++;  /* Bump hash table size. */
	*optr++ = LIBINIT_LUA;
	libdef_name(p, 0);
	newlen = libdef_expand_dump(optr, libbc_code + ofs, (MSize)len);
	optr += newlen;
	return;
      }
    }
    fprintf(stderr, "Error: missing libbc definition for %s\n", p);
    exit(1);
  }
}

static uint32_t find_rec(char *name)
{
  char *p = (char *)obuf;
  uint32_t n;
  for (n = 2; *p; n++) {
    if (strcmp(p, name) == 0)
      return n;
    p += strlen(p)+1;
  }
  if (p+strlen(name)+1 >= (char *)obuf+sizeof(obuf)) {
    fprintf(stderr, "Error: output buffer overflow\n");
    exit(1);
  }
  strcpy(p, name);
  return n;
}

static void libdef_rec(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(arg);
  if (ctx->mode == BUILD_recdef) {
    char *q;
    uint32_t n;
    for (; recffid+1 < ffid; recffid++)
      fprintf(ctx->fp, ",\n0");
    recffid = ffid;
    if (*p == '.') p = funcname;
    q = strchr(p, ' ');
    if (q) *q++ = '\0';
    n = find_rec(p);
    if (q)
      fprintf(ctx->fp, ",\n0x%02x00+(%s)", n, q);
    else
      fprintf(ctx->fp, ",\n0x%02x00", n);
  }
}

static void memcpy_endian(void *dst, void *src, size_t n)
{
  union { uint8_t b; uint32_t u; } host_endian;
  host_endian.u = 1;
  if (host_endian.b == LJ_ENDIAN_SELECT(1, 0)) {
    memcpy(dst, src, n);
  } else {
    size_t i;
    for (i = 0; i < n; i++)
      ((uint8_t *)dst)[i] = ((uint8_t *)src)[n-i-1];
  }
}

static void libdef_push(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(arg);
  if (ctx->mode == BUILD_libdef) {
    int len = (int)strlen(p);
    if (*p == '"') {
      if (len > 1 && p[len-1] == '"') {
	p[len-1] = '\0';
	libdef_name(p+1, LIBINIT_STRING);
	return;
      }
    } else if (*p >= '0' && *p <= '9') {
      char *ep;
      double d = strtod(p, &ep);
      if (*ep == '\0') {
	if (optr+1+sizeof(double) > obuf+sizeof(obuf)) {
	  fprintf(stderr, "Error: output buffer overflow\n");
	  exit(1);
	}
	*optr++ = LIBINIT_NUMBER;
	memcpy_endian(optr, &d, sizeof(double));
	optr += sizeof(double);
	return;
      }
    } else if (!strcmp(p, "lastcl")) {
      if (optr+1 > obuf+sizeof(obuf)) {
	fprintf(stderr, "Error: output buffer overflow\n");
	exit(1);
      }
      *optr++ = LIBINIT_LASTCL;
      return;
    } else if (len > 4 && !strncmp(p, "top-", 4)) {
      if (optr+2 > obuf+sizeof(obuf)) {
	fprintf(stderr, "Error: output buffer overflow\n");
	exit(1);
      }
      *optr++ = LIBINIT_COPY;
      *optr++ = (uint8_t)atoi(p+4);
      return;
    }
    fprintf(stderr, "Error: bad value for %sPUSH(%s)\n", LIBDEF_PREFIX, p);
    exit(1);
  }
}

static void libdef_set(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(arg);
  if (ctx->mode == BUILD_libdef) {
    if (p[0] == '!' && p[1] == '\0') p[0] = '\0';  /* Set env. */
    libdef_name(p, LIBINIT_STRING);
    *optr++ = LIBINIT_SET;
    obuf[2]++;  /* Bump hash table size. */
  }
}

static void libdef_regfunc(BuildCtx *ctx, char *p, int arg)
{
  UNUSED(ctx); UNUSED(p);
  regfunc = arg;
}

typedef void (*LibDefFunc)(BuildCtx *ctx, char *p, int arg);

typedef struct LibDefHandler {
  const char *suffix;
  const char *stop;
  const LibDefFunc func;
  const int arg;
} LibDefHandler;

static const LibDefHandler libdef_handlers[] = {
  { "MODULE_",	" \t\r\n",	libdef_module,		0 },
  { "CF(",	")",		libdef_func,		LIBINIT_CF },
  { "ASM(",	")",		libdef_func,		LIBINIT_ASM },
  { "ASM_(",	")",		libdef_func,		LIBINIT_ASM_ },
  { "LUA(",	")",		libdef_lua,		0 },
  { "REC(",	")",		libdef_rec,		0 },
  { "PUSH(",	")",		libdef_push,		0 },
  { "SET(",	")",		libdef_set,		0 },
  { "NOREGUV",	NULL,		libdef_regfunc,		REGFUNC_NOREGUV },
  { "NOREG",	NULL,		libdef_regfunc,		REGFUNC_NOREG },
  { NULL,	NULL,		(LibDefFunc)0,		0 }
};

/* Emit C source code for library function definitions. */
void emit_lib(BuildCtx *ctx)
{
  const char *fname;

  if (ctx->mode == BUILD_ffdef || ctx->mode == BUILD_libdef ||
      ctx->mode == BUILD_recdef)
    fprintf(ctx->fp, "/* This is a generated file. DO NOT EDIT! */\n\n");
  else if (ctx->mode == BUILD_vmdef)
    fprintf(ctx->fp, "ffnames = {\n[0]=\"Lua\",\n\"C\",\n");
  if (ctx->mode == BUILD_recdef)
    fprintf(ctx->fp, "static const uint16_t recff_idmap[] = {\n0,\n0x0100");
  recffid = ffid = FF_C+1;
  ffasmfunc = 0;

  while ((fname = *ctx->args++)) {
    char buf[256];  /* We don't care about analyzing lines longer than that. */
    FILE *fp;
    if (fname[0] == '-' && fname[1] == '\0') {
      fp = stdin;
    } else {
      fp = fopen(fname, "r");
      if (!fp) {
	fprintf(stderr, "Error: cannot open input file '%s': %s\n",
		fname, strerror(errno));
	exit(1);
      }
    }
    modstate = 0;
    regfunc = REGFUNC_OK;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
      char *p;
      /* Simplistic pre-processor. Only handles top-level #if/#endif. */
      if (buf[0] == '#' && buf[1] == 'i' && buf[2] == 'f') {
	int ok = 1;
	size_t len = strlen(buf);
	if (buf[len-1] == '\n') {
	  buf[len-1] = 0;
	  if (buf[len-2] == '\r') {
	    buf[len-2] = 0;
	  }
	}
	if (!strcmp(buf, "#if LJ_52"))
	  ok = LJ_52;
	else if (!strcmp(buf, "#if LJ_HASJIT"))
	  ok = LJ_HASJIT;
	else if (!strcmp(buf, "#if LJ_HASFFI"))
	  ok = LJ_HASFFI;
	else if (!strcmp(buf, "#if LJ_HASBUFFER"))
	  ok = LJ_HASBUFFER;
	if (!ok) {
	  int lvl = 1;
	  while (fgets(buf, sizeof(buf), fp) != NULL) {
	    if (buf[0] == '#' && buf[1] == 'e' && buf[2] == 'n') {
	      if (--lvl == 0) break;
	    } else if (buf[0] == '#' && buf[1] == 'i' && buf[2] == 'f') {
	      lvl++;
	    }
	  }
	  continue;
	}
      }
      for (p = buf; (p = strstr(p, LIBDEF_PREFIX)) != NULL; ) {
	const LibDefHandler *ldh;
	p += sizeof(LIBDEF_PREFIX)-1;
	for (ldh = libdef_handlers; ldh->suffix != NULL; ldh++) {
	  size_t n, len = strlen(ldh->suffix);
	  if (!strncmp(p, ldh->suffix, len)) {
	    p += len;
	    n = ldh->stop ? strcspn(p, ldh->stop) : 0;
	    if (!p[n]) break;
	    p[n] = '\0';
	    ldh->func(ctx, p, ldh->arg);
	    p += n+1;
	    break;
	  }
	}
	if (ldh->suffix == NULL) {
	  buf[strlen(buf)-1] = '\0';
	  fprintf(stderr, "Error: unknown library definition tag %s%s\n",
		  LIBDEF_PREFIX, p);
	  exit(1);
	}
      }
    }
    fclose(fp);
    if (ctx->mode == BUILD_libdef) {
      libdef_endmodule(ctx);
    }
  }

  if (ctx->mode == BUILD_ffdef) {
    fprintf(ctx->fp, "\n#undef FFDEF\n\n");
    fprintf(ctx->fp,
      "#ifndef FF_NUM_ASMFUNC\n#define FF_NUM_ASMFUNC %d\n#endif\n\n",
      ffasmfunc);
  } else if (ctx->mode == BUILD_vmdef) {
    fprintf(ctx->fp, "},\n\n");
  } else if (ctx->mode == BUILD_bcdef) {
    int i;
    fprintf(ctx->fp, "\n};\n\n");
    fprintf(ctx->fp, "LJ_DATADEF const uint16_t lj_bc_mode[] = {\n");
    fprintf(ctx->fp, "BCDEF(BCMODE)\n");
    for (i = ffasmfunc-1; i > 0; i--)
      fprintf(ctx->fp, "BCMODE_FF,\n");
    fprintf(ctx->fp, "BCMODE_FF\n};\n\n");
  } else if (ctx->mode == BUILD_recdef) {
    char *p = (char *)obuf;
    fprintf(ctx->fp, "\n};\n\n");
    fprintf(ctx->fp, "static const RecordFunc recff_func[] = {\n"
	    "recff_nyi,\n"
	    "recff_c");
    while (*p) {
      fprintf(ctx->fp, ",\nrecff_%s", p);
      p += strlen(p)+1;
    }
    fprintf(ctx->fp, "\n};\n\n");
  }
}

