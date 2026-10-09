/* DO NOT EDIT: automatically built by dist/s_include. */
#ifndef	_SM_EXT_H_
#define	_SM_EXT_H_

#if defined(__cplusplus)
extern "C" {
#endif

int __sm_table_list(sm_t *, u_int32_t **, int *);
int __sm_copy_file(sm_t *, sm_t *);
void __backup_restart(dbsql_backup_t *);
void __backup_update(dbsql_backup_t *, pgno_t, const u_int8_t *);
DB_TXN *__sm_cur_txn(sm_shared_t *);
int __sm_open(dbsql_vfs_t *, const char *, DBSQL *, sm_t **, int, int);
int __sm_close(sm_t *);
int __sm_begin_trans(sm_t *, int, int *);
int __sm_open_savepoint(sm_t *, int);
int __sm_begin_stmt(sm_t *, int);
int __sm_savepoint(sm_t *, int, int);
int __sm_commit_phase_one(sm_t *, const char *);
int __sm_commit_phase_two(sm_t *, int);
int __sm_commit(sm_t *);
int __sm_rollback(sm_t *, int, int);
int __sm_txn_state(sm_t *);
int __sm_dbh_get(sm_shared_t *, u_int32_t, key_info_t *, int, sm_dbh_t **);
int __sm_table_kind(sm_shared_t *, u_int32_t, int, int *);
int __sm_create_table(sm_t *, pgno_t *, int);
int __sm_drop_table(sm_t *, int, int *);
int __sm_clear_table(sm_t *, int, int64_t *);
void __sm_get_meta(sm_t *, int, u_int32_t *);
int __sm_update_meta(sm_t *, int, u_int32_t);
int __sm_new_db(sm_t *);
void *__sm_schema(sm_t *, int, void (*)(void *));
int __sm_schema_locked(sm_t *);
int __sm_set_cache_size(sm_t *, int);
int __sm_set_spill_size(sm_t *, int);
int __sm_set_mmap_limit(sm_t *, int64_t);
int __sm_set_pager_flags(sm_t *, unsigned);
int __sm_set_page_size(sm_t *, int, int, int);
int __sm_get_page_size(sm_t *);
int __sm_get_requested_reserve(sm_t *);
int __sm_get_reserve_no_mutex(sm_t *);
pgno_t __sm_max_page_count(sm_t *, pgno_t);
pgno_t __sm_last_page(sm_t *);
int __sm_secure_delete(sm_t *, int);
int __sm_set_auto_vacuum(sm_t *, int);
int __sm_get_auto_vacuum(sm_t *);
int __sm_incr_vacuum(sm_t *);
int __sm_compact(sm_t *);
const char *__sm_get_filename(sm_t *);
const char *__sm_get_journalname(sm_t *);
int __sm_is_readonly(sm_t *);
int __sm_set_version(sm_t *, int);
int __header_size_btree(void);
int __sm_checkpoint(sm_t *, int, int *, int *);
void __sm_clear_cache(sm_t *);
int __sm_rec_valid(const void *, u_int32_t, u_int32_t *);
int __sm_rec_cmp(DB *, const DBT *, const DBT *);
void __sm_rowid_to_key(int64_t, u_int8_t *);
int64_t __sm_key_to_rowid(const u_int8_t *);
sm_cursor_t *__sm_fake_valid_cursor(void);
void __sm_cursors_save(sm_shared_t *, DB_TXN *);
void __sm_cursors_invalidate_table(sm_shared_t *, u_int32_t);
int __sm_cursor(sm_t *, pgno_t, int, struct key_info_t *, sm_cursor_t *);
int __sm_table_moveto(sm_cursor_t *, int64_t, int, int *);
int __sm_index_moveto(sm_cursor_t *, unpacked_record_t *, int *);
u_int32_t __sm_payload_size(sm_cursor_t *);
int64_t __sm_max_record_size(sm_cursor_t *);
const void *__sm_payload_fetch(sm_cursor_t *, u_int32_t *);
int __sm_payload(sm_cursor_t *, u_int32_t, u_int32_t, void *);
int __sm_payload_checked(sm_cursor_t *, u_int32_t, u_int32_t, void *);
int __sm_put_data(sm_cursor_t *, u_int32_t, u_int32_t, void *);
void __sm_incrblob_cursor(sm_cursor_t *);
int __sm_insert(sm_cursor_t *, const sm_payload_t *, int, int);
int __sm_delete(sm_cursor_t *, u_int8_t);
int __sm_transfer_row(sm_cursor_t *, sm_cursor_t *, int64_t);
int __sm_count(DBSQL *, sm_cursor_t *, int64_t *);
int64_t __sm_row_count_est(sm_cursor_t *);
int __sm_trip_all_cursors(sm_t *, int, int);
void __sm_clear_cursor(sm_cursor_t *);
int __sm_cursor_is_valid_nn(sm_cursor_t *);
int __sm_cursor_is_valid(sm_cursor_t *);
void __sm_cursor_hint_flags(sm_cursor_t *, unsigned);
int __sm_cursor_has_hint(sm_cursor_t *, unsigned int);
void __sm_cursor_pin(sm_cursor_t *);
void __sm_cursor_unpin(sm_cursor_t *);
int64_t __sm_offset(sm_cursor_t *);
int __sm_integrity_check(DBSQL *, sm_t *, pgno_t *, dbsql_value_t *, int, int, int *, char **);
int __sm_errmap(int);
int __sm_env_open_persistent(const char *, sm_env_t **, char **);
int __sm_env_open_private(int, sm_env_t **);
void __sm_env_release(sm_env_t *);
int __sm_path_split(const char *, char **, char **);
struct pager_t *__sm_pager(sm_t *);
dbsql_file_t *__sm_pager_file(pager_t *);
dbsql_file_t *__sm_pager_jrnl_file(pager_t *);
const char *__sm_pager_filename(const pager_t *, int);
dbsql_vfs_t *__sm_pager_vfs(pager_t *);
int __sm_pager_is_memdb(pager_t *);
int __sm_pager_get_journal_mode(pager_t *);
int __sm_pager_set_journal_mode(pager_t *, int);
int __sm_pager_ok_to_change_journal_mode(pager_t *);
int64_t __sm_pager_journal_size_limit(pager_t *, int64_t);
int __sm_pager_locking_mode(pager_t *, int);
int __sm_pager_exclusive_lock(pager_t *);
int __sm_pager_flush(pager_t *);
void __sm_pager_shrink(pager_t *);
int __sm_pager_mem_used(pager_t *);
void __sm_pager_cache_stat(pager_t *, int, int, u_int64_t *);
u_int32_t __sm_pager_data_version(pager_t *);
int __sm_pager_wal_system_errno(pager_t *);
int __sm_pager_wal_callback(pager_t *);
int __sm_pager_wal_supported(pager_t *);
int __sm_pager_close_wal(pager_t *, DBSQL *);
int __sm_pager_snapshot_get(pager_t *, dbsql_snapshot_t **);
int __sm_pager_snapshot_open(pager_t *, dbsql_snapshot_t *);
int __sm_pager_snapshot_recover(pager_t *);
int __sm_pager_snapshot_check(pager_t *, dbsql_snapshot_t *);
void __sm_pager_snapshot_unlock(pager_t *);
int *__sm_pager_stats(pager_t *);
int __pcache_initialize(void);
void __pcache_shutdown(void);
void __p_cache_buffer_setup(void *, int, int);
void __p_cache_set_default(void);
void __pcache_stats(int *, int *, int *, int *);
int __header_size_pcache(void);
int __header_size_pcache1(void);
dbsql_mutex_t *__pcache1_mutex(void);
int __dbstat_register(DBSQL *);

#if defined(__cplusplus)
}
#endif
#endif /* !_SM_EXT_H_ */
