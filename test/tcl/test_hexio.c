/*
** 2007 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of dbsql interfaces.  This code
** implements TCL commands for reading and writing the binary
** database files and displaying the content of those files as
** hexadecimal.  We could, in theory, use the built-in "binary"
** command of TCL to do a lot of this, but there are some issues
** with historical versions of the "binary" command.  So it seems
** easier and safer to build our own mechanism.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>


/*
** Convert binary to hex.  The input buf[] contains N bytes of
** binary data.  buf[] is 2*n+1 bytes long.  Overwrite buf[]
** with a hexadecimal representation of its original binary input.
*/
void __test_bin_to_hex(unsigned char *buf, int N){
  const unsigned char hex[] = "0123456789ABCDEF";
  int i, j;
  unsigned char c;
  i = N*2;
  buf[i--] = 0;
  for(j=N-1; j>=0; j--){
    c = buf[j];
    buf[i--] = hex[c&0xf];
    buf[i--] = hex[c>>4];
  }
  assert( i==-1 );
}

/*
** Convert hex to binary.  The input in[] contains N bytes of
** hexadecimal.  Convert this into binary and write outs[] with
** the binary data.  Spaces in the original input are ignored.
** Return the number of bytes of binary rendered.
*/
int __test_hex_to_bin(const unsigned char *in, int N, unsigned char *outs){
  const unsigned char maps[] = {
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     1, 2, 3, 4, 5, 6, 7, 8,  9,10, 0, 0, 0, 0, 0, 0,
     0,11,12,13,14,15,16, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0,11,12,13,14,15,16, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
  };
  int i, j;
  int hi=1;
  unsigned char c;

  for(i=j=0; i<N; i++){
    c = maps[in[i]];
    if( c==0 ) continue;
    if( hi ){
      outs[j] = (c-1)<<4;
      hi = 0;
    }else{
      outs[j++] |= c-1;
      hi = 1;
    }
  }
  return j;
}


/*
** Usage:   hexio_read  FILENAME  OFFSET  AMT
**
** Read AMT bytes from file FILENAME beginning at OFFSET from the
** beginning of the file.  Convert that information to hexadecimal
** and return the resulting HEX string.
*/
static int DBSQL_TCLAPI hexio_read(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int offset;
  int amt, got;
  const char *file;
  unsigned char *buf;
  FILE *in;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME OFFSET AMT");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &offset) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[3], &amt) ) return TCL_ERROR;
  file = Tcl_GetString(objv[1]);
  buf = dbsql_malloc( amt*2+1 );
  if( buf==0 ){
    return TCL_ERROR;
  }
  in = fopen(file, "rb");
  if( in==0 ){
    in = fopen(file, "r");
  }
  if( in==0 ){
    Tcl_AppendResult(interp, "cannot open input file ", file, NULL);
    return TCL_ERROR;
  }
  fseek(in, offset, SEEK_SET);
  got = (int)fread(buf, 1, amt, in);
  fclose(in);
  if( got<0 ){
    got = 0;
  }
  __test_bin_to_hex(buf, got);
  Tcl_AppendResult(interp, buf, NULL);
  dbsql_free(buf);
  return TCL_OK;
}


/*
** Usage:   hexio_write  FILENAME  OFFSET  DATA
**
** Write DATA into file FILENAME beginning at OFFSET from the
** beginning of the file.  DATA is expressed in hexadecimal.
*/
static int DBSQL_TCLAPI hexio_write(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int offset;
  TCL_SIZE n_in;
  int n_out, written;
  const char *file;
  const unsigned char *in;
  unsigned char *outs;
  FILE *out;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME OFFSET HEXDATA");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &offset) ) return TCL_ERROR;
  file = Tcl_GetString(objv[1]);
  in = (const unsigned char *)Tcl_GetStringFromObj(objv[3], &n_in);
  outs = dbsql_malloc64( 1 + n_in/2 );
  if( outs==0 ){
    return TCL_ERROR;
  }
  n_out = __test_hex_to_bin(in, (int)n_in, outs);
  out = fopen(file, "r+b");
  if( out==0 ){
    out = fopen(file, "r+");
  }
  if( out==0 ){
    Tcl_AppendResult(interp, "cannot open output file ", file, NULL);
    return TCL_ERROR;
  }
  fseek(out, offset, SEEK_SET);
  written = (int)fwrite(outs, 1, n_out, out);
  dbsql_free(outs);
  fclose(out);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(written));
  return TCL_OK;
}

/*
** USAGE:   hexio_get_int [-littleendian] HEXDATA
**
** Interpret the HEXDATA argument as a big-endian integer.  Return
** the value of that integer.  HEXDATA can contain between 2 and 8
** hexadecimal digits.
*/
static int DBSQL_TCLAPI hexio_get_int(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int val;
  TCL_SIZE n_in;
  int n_out;
  const unsigned char *in;
  unsigned char *outs;
  unsigned char nums[4];
  int little = 0;

  if( objc==3 ){
    TCL_SIZE n;
    char *z = Tcl_GetStringFromObj(objv[1], &n);
    if( n>=2 && n<=13 && memcmp(z, "-littleendian", n)==0 ){
      little = 1;
    }
  }
  if( (objc-little)!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "[-littleendian] HEXDATA");
    return TCL_ERROR;
  }
  in = (const unsigned char *)Tcl_GetStringFromObj(objv[1+little], &n_in);
  outs = dbsql_malloc64( 1 + n_in/2 );
  if( outs==0 ){
    return TCL_ERROR;
  }
  n_out = __test_hex_to_bin(in, (int)n_in, outs);
  if( n_out>=4 ){
    memcpy(nums, outs, 4);
  }else{
    memset(nums, 0, sizeof(nums));
    memcpy(&nums[4-n_out], outs, n_out);
  }
  dbsql_free(outs);
  if( little ){
    val = (int)((u_int32_t)nums[3]<<24) | (nums[2]<<16) | (nums[1]<<8) | nums[0];
  }else{
    val = (int)((u_int32_t)nums[0]<<24) | (nums[1]<<16) | (nums[2]<<8) | nums[3];
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(val));
  return TCL_OK;
}


/*
** USAGE:   hexio_render_int16   INTEGER
**
** Render INTEGER has a 16-bit big-endian integer in hexadecimal.
*/
static int DBSQL_TCLAPI hexio_render_int16(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int val;
  unsigned char nums[10];

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "INTEGER");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &val) ) return TCL_ERROR;
  nums[0] = val>>8;
  nums[1] = val;
  __test_bin_to_hex(nums, 2);
  Tcl_SetObjResult(interp, Tcl_NewStringObj((char*)nums, 4));
  return TCL_OK;
}


/*
** USAGE:   hexio_render_int32   INTEGER
**
** Render INTEGER has a 32-bit big-endian integer in hexadecimal.
*/
static int DBSQL_TCLAPI hexio_render_int32(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int val;
  unsigned char nums[10];

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "INTEGER");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &val) ) return TCL_ERROR;
  nums[0] = val>>24;
  nums[1] = val>>16;
  nums[2] = val>>8;
  nums[3] = val;
  __test_bin_to_hex(nums, 4);
  Tcl_SetObjResult(interp, Tcl_NewStringObj((char*)nums, 8));
  return TCL_OK;
}

/*
** USAGE:  utf8_to_utf8  HEX
**
** The argument is a UTF8 string represented in hexadecimal.
** The UTF8 might not be well-formed.  Run this string through
** __utf8to8() convert it back to hex and return the result.
*/
static int DBSQL_TCLAPI utf8_to_utf8(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifdef DBSQL_DEBUG
  TCL_SIZE n;
  int n_out;
  const unsigned char *orig;
  unsigned char *z;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HEX");
    return TCL_ERROR;
  }
  orig = (unsigned char *)Tcl_GetStringFromObj(objv[1], &n);
  z = dbsql_malloc64( n+4 );
  n = __test_hex_to_bin(orig, (int)n, z);
  z[n] = 0;
  n_out = __utf8_to8(z);
  __test_bin_to_hex(z,n_out);
  Tcl_AppendResult(interp, (char*)z, NULL);
  dbsql_free(z);
  return TCL_OK;
#else
  Tcl_AppendResult(interp, 
      "[utf8_to_utf8] unavailable - DBSQL_DEBUG not defined", NULL
  );
  return TCL_ERROR;
#endif
}

static int __get_fts3_varint(const char *p, int64_t *v){
  const unsigned char *q = (const unsigned char *) p;
  u_int64_t x = 0, y = 1;
  while( (*q & 0x80) == 0x80 ){
    x += y * (*q++ & 0x7f);
    y <<= 7;
  }
  x += y * (*q++);
  *v = (int64_t) x;
  return (int) (q - (unsigned char *)p);
}

static int __put_fts3_varint(char *p, int64_t v){
  unsigned char *q = (unsigned char *) p;
  u_int64_t vu = v;
  do{
    *q++ = (unsigned char) ((vu & 0x7f) | 0x80);
    vu >>= 7;
  }while( vu!=0 );
  q[-1] &= 0x7f;  /* turn off high bit in final byte */
  assert( q - (unsigned char *)p <= 10 );
  return (int) (q - (unsigned char *)p);
}

/*
** USAGE:  read_fts3varint BLOB VARNAME
**
** Read a varint from the start of BLOB. Set variable VARNAME to contain
** the interpreted value. Return the number of bytes of BLOB consumed.
*/
static int DBSQL_TCLAPI read_fts3varint(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  TCL_SIZE n_blob;
  unsigned char *blob;
  int64_t i_val;
  int n_val;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BLOB VARNAME");
    return TCL_ERROR;
  }
  blob = Tcl_GetByteArrayFromObj(objv[1], &n_blob);

  n_val = __get_fts3_varint((char*)blob, (int64_t *)(&i_val));
  Tcl_ObjSetVar2(interp, objv[2], 0, Tcl_NewWideIntObj(i_val), 0);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(n_val));
  return TCL_OK;
}

/*
** USAGE:  make_fts3record ARGLIST
*/
static int DBSQL_TCLAPI make_fts3record(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Obj **args = 0;
  TCL_SIZE n_arg = 0;
  unsigned char *outs = 0;
  int64_t n_out = 0;
  int64_t n_alloc = 0;
  int i;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "LIST");
    return TCL_ERROR;
  }
  if( Tcl_ListObjGetElements(interp, objv[1], &n_arg, &args) ){
    return TCL_ERROR;
  }

  for(i=0; i<(int)n_arg; i++){
    Tcl_WideInt i_val;
    if( TCL_OK==Tcl_GetWideIntFromObj(0, args[i], &i_val) ){
      if( n_out+10>n_alloc ){
        int n_new = n_alloc?n_alloc*2:128;
        unsigned char *news = dbsql_realloc(outs, n_new);
        if( news==0 ){
          dbsql_free(outs);
          return TCL_ERROR;
        }
        outs = news;
        n_alloc = n_new;
      }
      n_out += __put_fts3_varint((char*)&outs[n_out], i_val);
    }else{
      TCL_SIZE n_val = 0;
      char *val = Tcl_GetStringFromObj(args[i], &n_val);
      while( (n_out + n_val)>n_alloc ){
        int64_t n_new = n_alloc?n_alloc*2:128;
        unsigned char *news = dbsql_realloc64(outs, n_new);
        if( news==0 ){
          dbsql_free(outs);
          return TCL_ERROR;
        }
        outs = news;
        n_alloc = n_new;
      }
      memcpy(&outs[n_out], val, n_val);
      n_out += n_val;
    }
  }

  Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(outs, n_out));
  dbsql_free(outs);
  return TCL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest_hexio_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } obj_cmds[] = {
     { "hexio_read",                   hexio_read            },
     { "hexio_write",                  hexio_write           },
     { "hexio_get_int",                hexio_get_int         },
     { "hexio_render_int16",           hexio_render_int16    },
     { "hexio_render_int32",           hexio_render_int32    },
     { "utf8_to_utf8",                 utf8_to_utf8          },
     { "read_fts3varint",              read_fts3varint       },
     { "make_fts3record",              make_fts3record       },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, 0, 0);
  }
  return TCL_OK;
}
