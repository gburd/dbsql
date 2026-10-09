/*
** 2011 March 18
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains a VFS "shim" - a layer that sits in between the
** pager and the real VFS.
**
** This particular shim enforces a multiplex system on DB files. 
** This shim shards/partitions a single DB file into smaller
** "chunks" such that the total DB file size may exceed the maximum
** file size of the underlying file system.
**
*/

#ifndef DBSQL_TEST_MULTIPLEX_H
#define DBSQL_TEST_MULTIPLEX_H

/*
** CAPI: File-control Operations Supported by Multiplex VFS
**
** Values interpreted by the file_control method of a Multiplex VFS db file-handle.
**
** MULTIPLEX_CTRL_ENABLE:
**   This file control is used to enable or disable the multiplex
**   shim.
**
** MULTIPLEX_CTRL_SET_CHUNK_SIZE:
**   This file control is used to set the maximum allowed chunk
**   size for a multiplex file set.  The chunk size should be
**   a multiple of DBSQL_MAX_PAGE_SIZE, and will be rounded up
**   if not.
**
** MULTIPLEX_CTRL_SET_MAX_CHUNKS:
**   This file control is used to set the maximum number of chunks
**   allowed to be used for a multiplex file set.
*/
#define MULTIPLEX_CTRL_ENABLE          214014
#define MULTIPLEX_CTRL_SET_CHUNK_SIZE  214015
#define MULTIPLEX_CTRL_SET_MAX_CHUNKS  214016

#ifdef __cplusplus
extern "C" {
#endif

/*
** CAPI: Initialize the multiplex VFS shim - dbsql_multiplex_initialize()
**
** Use the VFS named orig_vfs_name as the VFS that does the actual work. 
** Use the default if orig_vfs_name==NULL. 
**
** The multiplex VFS shim is named "multiplex".  It will become the default
** VFS if make_default is non-zero.
**
** An auto-extension is registered which will make the function
** multiplex_control() available to database connections.  This
** function gives access to the file_control interface of the
** multiplex VFS shim.
**
** SELECT multiplex_control(<op>,<val>);
**
**   <op>=1 MULTIPLEX_CTRL_ENABLE
**   <val>=0 disable
**   <val>=1 enable
**
**   <op>=2 MULTIPLEX_CTRL_SET_CHUNK_SIZE
**   <val> int, chunk size
**
**   <op>=3 MULTIPLEX_CTRL_SET_MAX_CHUNKS
**   <val> int, max chunks
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once
** during start-up.
*/
extern int dbsql_multiplex_initialize(const char *orig_vfs_name, int make_default);

/*
** CAPI: Shutdown the multiplex system - dbsql_multiplex_shutdown()
**
** All dbsql database connections must be closed before calling this
** routine.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once while
** shutting down in order to free all remaining multiplex groups.
*/
extern int dbsql_multiplex_shutdown(int force);

#ifdef __cplusplus
}  /* End of the 'extern "C"' block */
#endif

#endif /* DBSQL_TEST_MULTIPLEX_H */
