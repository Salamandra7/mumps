/****************************************************************
 *								*
 *	Copyright 2001, 2008 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include <sys/mman.h>
#include <sys/shm.h>

#include "gtm_string.h"
#include "gtm_time.h"
#include "gtm_unistd.h"	/* fsync() needs this */

#include "aswp.h"	/* for ASWP */
#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "iosp.h"		/* for SS_NORMAL */
#include "gdsbgtr.h"
#include "jnl.h"
#include "lockconst.h"		/* for LOCK_AVAILABLE */
#include "interlock.h"
#include "sleep_cnt.h"
#include "performcaslatchcheck.h"
#include "send_msg.h"
#include "gt_timer.h"
#include "is_file_identical.h"
#include "gtmmsg.h"
#include "wcs_sleep.h"
#include "wcs_flu.h"
#include "wcs_recover.h"
#include "wcs_phase2_commit_wait.h"
#include "wbox_test_init.h"
#include "wcs_mm_recover.h"
#include "memcoherency.h"

GBLREF	gd_region	*gv_cur_region;
GBLREF	uint4		process_id;
GBLREF	sgmnt_addrs	*cs_addrs;
GBLREF	volatile int4	db_fsync_in_prog;	/* for DB_FSYNC macro usage */
GBLREF 	jnl_gbls_t	jgbl;
GBLREF 	bool		in_backup;
GBLREF	boolean_t	mu_rndwn_file_dbjnl_flush;

#define	WAIT_FOR_CONCURRENT_WRITERS_TO_FINISH(fix_in_wtstart)								\
{															\
	if (WRITERS_ACTIVE(cnl))											\
	{														\
		DEBUG_ONLY(int4	in_wtstart;) 		/* temporary for debugging purposes */				\
		DEBUG_ONLY(int4	intent_wtstart;) 	/* temporary for debugging purposes */				\
		error_def(ERR_WRITERSTUCK);										\
															\
		assert(csa->now_crit);											\
		SIGNAL_WRITERS_TO_STOP(csd);		/* to stop all active writers */				\
		lcnt = 0;												\
		do													\
		{													\
			DEBUG_ONLY(in_wtstart = cnl->in_wtstart;)							\
			DEBUG_ONLY(intent_wtstart = cnl->intent_wtstart;)						\
			if (MAXGETSPACEWAIT DEBUG_ONLY( * 2) == ++lcnt)							\
			{	/* We have noticed the below assert to fail occasionally on some platforms (mostly	\
				 * AIX and Linux). We suspect it is because of waiting for another writer that is 	\
				 * in jnl_fsync (as part of flushing a global buffer) which takes more than a minute	\
				 * to finish. To avoid false failures (where the other writer finishes its job in	\
				 * a little over a minute) we wait for twice the time in the debug version.		\
				 */											\
				assert(FALSE);										\
				cnl->wcsflu_pid = 0;									\
				SIGNAL_WRITERS_TO_RESUME(csd);								\
				if (!was_crit)										\
					rel_crit(gv_cur_region);							\
				send_msg(VARLSTCNT(5) ERR_WRITERSTUCK, 3, cnl->in_wtstart, DB_LEN_STR(gv_cur_region));	\
				return FALSE;										\
			}												\
			if (-1 == shmctl(udi->shmid, IPC_STAT, &shm_buf))						\
			{												\
				save_errno = errno;									\
				if (1 == lcnt)										\
				{											\
					send_msg(VARLSTCNT(4) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region));		\
					send_msg(VARLSTCNT(8) ERR_SYSCALL, 5, RTS_ERROR_LITERAL("shmctl()"),		\
							CALLFROM, save_errno);						\
				} 											\
			} else if (1 == shm_buf.shm_nattch)								\
			{												\
				assert((FALSE == csa->in_wtstart) && (0 <= cnl->in_wtstart));				\
				cnl->in_wtstart = 0;	/* fix improper value of in_wtstart if you are standalone */	\
				fix_in_wtstart = TRUE;									\
				cnl->intent_wtstart = 0;/* fix improper value of intent_wtstart if standalone */	\
			} else												\
				wcs_sleep(lcnt);		/* wait for any in wcs_wtstart to finish */		\
		} while (WRITERS_ACTIVE(cnl));										\
		SIGNAL_WRITERS_TO_RESUME(csd);										\
	}														\
}

bool wcs_flu(bool options)
{
	bool			success, was_crit;
	boolean_t		fix_in_wtstart, flush_hdr, jnl_enabled, sync_epoch, write_epoch, need_db_fsync, in_commit;
	unsigned int		lcnt, pass;
	int			save_errno, wtstart_errno;
	jnl_buffer_ptr_t	jb;
	jnl_private_control	*jpc;
	uint4			jnl_status, to_wait, to_msg;
        unix_db_info    	*udi;
	sgmnt_addrs		*csa;
	sgmnt_data_ptr_t	csd;
	node_local_ptr_t	cnl;
	file_control		*fc;
	cache_que_head_ptr_t	crq;
        struct shmid_ds         shm_buf;
	uint4			fsync_dskaddr;

	error_def(ERR_DBFILERR);
	error_def(ERR_DBFSYNCERR);
	error_def(ERR_GBLOFLOW);
	error_def(ERR_JNLFILOPN);
	error_def(ERR_JNLFLUSH);
	error_def(ERR_OUTOFSPACE);
	error_def(ERR_SYSCALL);
	error_def(ERR_TEXT);
	error_def(ERR_WAITDSKSPACE);
	error_def(ERR_WCBLOCKED);

	jnl_status = 0;
	flush_hdr = options & WCSFLU_FLUSH_HDR;
	write_epoch = options & WCSFLU_WRITE_EPOCH;
	sync_epoch = options & WCSFLU_SYNC_EPOCH;
	need_db_fsync = options & WCSFLU_FSYNC_DB;
	/* WCSFLU_IN_COMMIT bit is set if caller is t_end or tp_tend. In that case, we should NOT invoke wcs_recover if we
	 * encounter an error. Instead we should return the error as such so they can trigger appropriate error handling.
	 * This is necessary because t_end and tp_tend could have pinned one or more cache-records (cr->in_cw_set non-zero)
	 * BEFORE invoking wcs_flu. And code AFTER the wcs_flu in them relies on the fact that those cache records stay
	 * pinned. If wcs_flu invokes wcs_recover, it will reset cr->in_cw_set to 0 for ALL cache-records so code AFTER
	 * the wcs_flu in the caller will fail because no buffer is pinned at that point.
	 */
	in_commit = options & WCSFLU_IN_COMMIT;
	udi = FILE_INFO(gv_cur_region);
	csa = &udi->s_addrs;
	csd = csa->hdr;
	cnl = csa->nl;
	assert(cnl->glob_sec_init);
	BG_TRACE_ANY(csa, total_buffer_flush);

	if (!(was_crit = csa->now_crit))	/* Caution: assignment */
		grab_crit(gv_cur_region);
	cnl->wcsflu_pid = process_id;
	if (dba_mm == csd->acc_meth)
	{
#ifndef NO_MSYNC
#if defined(UNTARGETED_MSYNC)
		if (csa->ti->last_mm_sync != csa->ti->curr_tn)
		{
			if (0 == msync((caddr_t)csa->db_addrs[0], (size_t)(csa->db_addrs[1] - csa->db_addrs[0]), MS_ASYNC))
				csa->ti->last_mm_sync = csa->ti->curr_tn;	/* Save when did last full sync */
			else
			{
				cnl->wcsflu_pid = 0;
				if (!was_crit)
					rel_crit(gv_cur_region);
				return FALSE;
			}
		}
#else
		SIGNAL_WRITERS_TO_STOP(csd);	/* to stop all active writers */
		WAIT_FOR_WRITERS_TO_STOP(cnl, lcnt, MAXGETSPACEWAIT);
		if (MAXGETSPACEWAIT == lcnt)
		{
			DEBUG_ONLY(GET_C_STACK_WTSTART_WRITERS(cnl->wtstart_pid));
			assert(FALSE);
			cnl->wcsflu_pid = 0;
			if (!was_crit)
				rel_crit(gv_cur_region);
			return FALSE;
		}
		SIGNAL_WRITERS_TO_RESUME(csd);
		/* wcs_flu() is currently also called from wcs_clean_dbsync() which is interrupt driven code. We are about to
		 * remap the database in interrupt code. Depending on where the interrupt occurred, all sorts of strange failures
		 * can occur in the mainline code after the remap in interrupt code. Thankfully, this code is currently not
		 * enabled by default (NO_MSYNC is the default) so we are fine. If ever this gets re-enabled, we need to
		 * solve this problem by changing wcs_clean_dbsync not to call wcs_flu. The assert below is a note for this.
		 */
		assert(FALSE);
		MM_DBFILEXT_REMAP_IF_NEEDED(csa, gv_cur_region);
		/* MM MM_DBFILEXT_REMAP_IF_NEEDED can remap the file so reset csd which might no long point to the file */
		csd = csa->hdr;
		while (0 != csa->acc_meth.mm.mmblk_state->mmblkq_active.fl)
		{
			wtstart_errno = wcs_wtstart(gv_cur_region, csd->n_bts);
			assert(ERR_GBLOFLOW != wtstart_errno);
		}
#endif
#endif
	}
	/* jnl_enabled is an overloaded variable. It is TRUE only if JNL_ENABLED(csd) is TRUE
	 * and if the journal file has been opened in shared memory. If the journal file hasn't
	 * been opened in shared memory, we needn't (and shouldn't) do any journal file activity.
	 */
	jnl_enabled = (JNL_ENABLED(csd) && (0 != cnl->jnl_file.u.inode));
	if (jnl_enabled)
	{
		jpc = csa->jnl;
		jb = jpc->jnl_buff;
		/* Assert that we never flush the cache in the midst of a database commit. The only exception is MUPIP RUNDOWN */
		assert((csa->ti->curr_tn == csa->ti->early_tn) || mu_rndwn_file_dbjnl_flush);
		if (!jgbl.dont_reset_gbl_jrec_time)
			SET_GBL_JREC_TIME;	/* needed before jnl_ensure_open */
		/* Before writing to jnlfile, adjust jgbl.gbl_jrec_time (if needed) to maintain time order of jnl
		 * records. This needs to be done BEFORE the jnl_ensure_open as that could write journal records
		 * (if it decides to switch to a new journal file)
		 */
		ADJUST_GBL_JREC_TIME(jgbl, jb);
		assert(csa == cs_addrs);	/* for jnl_ensure_open */
		jnl_status = jnl_ensure_open();
		if (SS_NORMAL != jnl_status)
		{
			assert(ERR_JNLFILOPN == jnl_status);
			cnl->wcsflu_pid = 0;
			if (!was_crit)
				rel_crit(gv_cur_region);
			send_msg(VARLSTCNT(6) jnl_status, 4, JNL_LEN_STR(csd), DB_LEN_STR(gv_cur_region));
			return FALSE;
		}
		fsync_dskaddr = jb->fsync_dskaddr;	/* take a local copy as it could change concurrently */
		if (fsync_dskaddr != jb->freeaddr)
		{
			assert(fsync_dskaddr <= jb->dskaddr);
			if (SS_NORMAL != (jnl_status = jnl_flush(gv_cur_region)))
			{
				assert(NOJNL == jpc->channel); /* jnl file lost */
				if (!was_crit)
					rel_crit(gv_cur_region);
				send_msg(VARLSTCNT(9) ERR_JNLFLUSH, 2, JNL_LEN_STR(csd),
					ERR_TEXT, 2, RTS_ERROR_TEXT("Error with journal flush during wcs_flu1"),
					jnl_status);
				return FALSE;
			}
			assert(jb->freeaddr == jb->dskaddr);
			jnl_fsync(gv_cur_region, jb->dskaddr);
			assert(jb->fsync_dskaddr == jb->dskaddr);
		}
	}
	if (dba_mm != csd->acc_meth)
	{
		/* If not mupip rundown, wait for ALL active phase2 commits to complete first.
		 * In case of mupip rundown, we know no one else is accessing shared memory so no point waiting.
		 */
		if (!mu_rndwn_file_dbjnl_flush && cnl->wcs_phase2_commit_pidcnt && !wcs_phase2_commit_wait(csa, NULL))
		{
			assert(FALSE);
			if (!was_crit)
				rel_crit(gv_cur_region);
			return FALSE;	/* we expect the caller to trigger cache-recovery which will fix this counter */
		}
		/* Now that all concurrent commits are complete, wait for these dirty buffers to be flushed to disk.
		 * Note that calling wcs_wtstart just once assumes that if we ask it to flush all the buffers, it will.
		 * This may not be true in case of twins. But this is Unix. So not an issue.
		 */
		wtstart_errno = wcs_wtstart(gv_cur_region, csd->n_bts);		/* Flush it all */
		/* At this point the cache should have been flushed except if some other process is in wcs_wtstart waiting
		 * to flush the dirty buffer that it has already removed from the active queue. Wait for it to finish.
		 */
		fix_in_wtstart = FALSE;		/* set to TRUE by the following macro if we needed to correct cnl->in_wtstart */
		WAIT_FOR_CONCURRENT_WRITERS_TO_FINISH(fix_in_wtstart);
		/* Ideally at this point, the cache should have been flushed. But there is a possibility that the other
		 *   process in wcs_wtstart which had already removed the dirty buffer from the active queue found (because
		 *   csr->jnl_addr > jb->dskaddr) that it needs to be reinserted and placed it back in the active queue.
		 *   In this case, issue another wcs_wtstart to flush the cache. Even if a concurrent writer picks up an
		 *   entry, he should be able to write it out since the journal is already flushed.
		 * The check for whether the cache has been flushed is two-pronged. One via "wcs_active_lvl" and the other
		 *   via the active queue head. Ideally, both are interdependent and checking on "wcs_active_lvl" should be
		 *   enough, but we don't want to take a risk in PRO (in case wcs_active_lvl is incorrect).
		 */
		crq = &csa->acc_meth.bg.cache_state->cacheq_active;
		assert(((0 <= cnl->wcs_active_lvl) && (cnl->wcs_active_lvl || 0 == crq->fl)) || (ENOSPC == wtstart_errno));
		if (cnl->wcs_active_lvl || crq->fl)
		{
			wtstart_errno = wcs_wtstart(gv_cur_region, csd->n_bts);		/* Flush it all */
			WAIT_FOR_CONCURRENT_WRITERS_TO_FINISH(fix_in_wtstart);
			if (cnl->wcs_active_lvl || crq->fl)		/* give allowance in PRO */
			{
				if (ENOSPC == wtstart_errno)
				{	/* wait for csd->wait_disk_space seconds, and give up if still not successful */
					to_wait = csd->wait_disk_space;
					to_msg = (to_wait / 8) ? (to_wait / 8) : 1; /* send message 8 times */
					while ((0 < to_wait) && (ENOSPC == wtstart_errno))
					{
						if ((to_wait == csd->wait_disk_space)
						    || (0 == to_wait % to_msg))
						{
							send_msg(VARLSTCNT(7) ERR_WAITDSKSPACE, 4,
								 process_id, to_wait, DB_LEN_STR(gv_cur_region), wtstart_errno);
							gtm_putmsg(VARLSTCNT(7) ERR_WAITDSKSPACE, 4,
								   process_id, to_wait, DB_LEN_STR(gv_cur_region), wtstart_errno);
						}
						hiber_start(1000);
						to_wait--;
						wtstart_errno = wcs_wtstart(gv_cur_region, csd->n_bts);
						if (0 == crq->fl)
							break;
					}
					if ((to_wait <= 0) && (cnl->wcs_active_lvl || crq->fl))
					{	/* not enough space became available after the wait */
						send_msg(VARLSTCNT(5) ERR_OUTOFSPACE, 3, DB_LEN_STR(gv_cur_region), process_id);
						rts_error(VARLSTCNT(5) ERR_OUTOFSPACE, 3, DB_LEN_STR(gv_cur_region), process_id);
					}
				} else
				{	/* The only case we know of currently when this is possible is if a process encountered
					 * an error in the midst of committing in phase2 and secshr_db_clnup completed the commit
					 * for it and set wc_blocked to TRUE (even though it was out of crit) causing
					 * the wcs_wtstart calls done above to do nothing. But phase2 commit errors are currently
					 * enabled only through white-box testing.  The only exception to this is if this is a
					 * crash shutdown and later mupip rundown or mupip integ is being invoked on this shared
					 * memory where it is the only process attached to the database shared memory.  Also
					 * mupip integ freezes the database so assert accordingly.
					 */
					assert(gtm_white_box_test_case_enabled || mu_rndwn_file_dbjnl_flush
								      || (csd->freeze && (csd->image_count == process_id)));
					SET_TRACEABLE_VAR(csd->wc_blocked, TRUE);
					BG_TRACE_PRO_ANY(csa, wcb_wcs_flu1);
					send_msg(VARLSTCNT(8) ERR_WCBLOCKED, 6, LEN_AND_LIT("wcb_wcs_flu1"),
						 process_id, &csa->ti->curr_tn, DB_LEN_STR(gv_cur_region));
					if (in_commit)
					{	/* We should NOT be invoking wcs_recover as otherwise the callers (t_end or tp_tend)
						 * will get confused (see explanation above where variable "in_commit" gets set).
						 */
						assert(was_crit);	/* so dont need to rel_crit */
						return FALSE;
					}
					assert(!jnl_enabled || jb->fsync_dskaddr == jb->freeaddr);
					wcs_recover(gv_cur_region);
					if (jnl_enabled)
					{
						fsync_dskaddr = jb->fsync_dskaddr;
							/* take a local copy as it could change concurrently */
						if (fsync_dskaddr != jb->freeaddr)
						{	/* an INCTN record should have been written above */
							assert(fsync_dskaddr <= jb->dskaddr);
							assert((jb->freeaddr - fsync_dskaddr) >= INCTN_RECLEN);
							/* above assert has a >= instead of == due to possible
							 * ALIGN record in between */
							if (SS_NORMAL != (jnl_status = jnl_flush(gv_cur_region)))
							{
								assert(NOJNL == jpc->channel); /* jnl file lost */
								if (!was_crit)
									rel_crit(gv_cur_region);
								send_msg(VARLSTCNT(9) ERR_JNLFLUSH, 2, JNL_LEN_STR(csd),
									ERR_TEXT, 2,
									RTS_ERROR_TEXT("Error with journal flush during wcs_flu2"),
									jnl_status);
								return FALSE;
							}
							assert(jb->freeaddr == jb->dskaddr);
							jnl_fsync(gv_cur_region, jb->dskaddr);
							/* Use jb->fsync_dskaddr (instead of "fsync_dskaddr") below as the
							 * shared memory copy is more uptodate (could have been updated by
							 * "jnl_fsync" call above).
							 */
							assert(jb->fsync_dskaddr == jb->dskaddr);
						}
					}
					wcs_wtstart(gv_cur_region, csd->n_bts);		/* Flush it all */
					WAIT_FOR_CONCURRENT_WRITERS_TO_FINISH(fix_in_wtstart);
					if (cnl->wcs_active_lvl || crq->fl)
					{
						cnl->wcsflu_pid = 0;
						if (!was_crit)
							rel_crit(gv_cur_region);
						GTMASSERT;
					}
				}
			}
		}
	}
	if (flush_hdr)
	{
		assert(memcmp(csd->label, GDS_LABEL, GDS_LABEL_SZ - 1) == 0);
		fileheader_sync(gv_cur_region);
	}
	if (jnl_enabled && write_epoch)
	{	/* If need to write an epoch,
		 *	(1) get hold of the jnl io_in_prog lock.
		 *	(2) set need_db_fsync to TRUE in the journal buffer.
		 *	(3) release the jnl io_in_prog lock.
		 *	(4) write an epoch record in the journal buffer.
		 * The next call to jnl_qio_start() will do the fsync() of the db before doing any jnl qio.
		 * The basic requirement is that we shouldn't write the epoch out until we have synced the database.
		 */
		assert(jb->fsync_dskaddr == jb->freeaddr);
		/* If jb->need_db_fsync is TRUE at this point of time, it means we already have a db_fsync waiting
		 * to happen. This means the epoch due to the earlier need_db_fsync hasn't yet been written out to
		 * the journal file. But that means we haven't yet flushed the journal buffer which leads to a
		 * contradiction. (since we have called jnl_flush earlier in this routine and also assert to the
		 * effect jb->fsync_dskaddr == jb->freeaddr a few lines above).
		 */
		assert(!jb->need_db_fsync);
		for (lcnt = 1; FALSE == (GET_SWAPLOCK(&jb->io_in_prog_latch)); lcnt++)
		{
			if (MAXJNLQIOLOCKWAIT == lcnt)	/* tried too long */
			{
				DEBUG_ONLY(GET_C_STACK_WTSTART_WRITERS(cnl->wtstart_pid));
				assert(FALSE);
				cnl->wcsflu_pid = 0;
				if (!was_crit)
					rel_crit(gv_cur_region);
				GTMASSERT;
			}
			wcs_sleep(SLEEP_JNLQIOLOCKWAIT);	/* since it is a short lock, sleep the minimum */
			performCASLatchCheck(&jb->io_in_prog_latch, lcnt);
		}
		jb->need_db_fsync = TRUE;	/* for comments on need_db_fsync, see jnl_output_sp.c */
		RELEASE_SWAPLOCK(&jb->io_in_prog_latch);
		assert(!(JNL_FILE_SWITCHED(jpc)));
		assert(jgbl.gbl_jrec_time);
		if (0 == jpc->pini_addr)
			jnl_put_jrt_pini(csa);
		jnl_write_epoch_rec(csa);
	}
	cnl->wcsflu_pid = 0;
	if (!was_crit)
		rel_crit(gv_cur_region);
	/* sync the epoch record in the journal if needed. */
	if (jnl_enabled && write_epoch && sync_epoch && (csa->ti->curr_tn == csa->ti->early_tn))
	{	/* Note that if we are in the midst of committing and came here through a bizarre
		 * stack trace (like wcs_get_space etc.) we want to defer syncing to when we go out of crit.
		 * Note that we are guaranteed to come back to wcs_wtstart since we are currently in commit-phase
		 * and will dirty atleast one block as part of the commit for a wtstart timer to be triggered.
		 */
		jnl_wait(gv_cur_region);
	}
	if (need_db_fsync && JNL_ALLOWED(csd))
	{
		if (dba_mm != csd->acc_meth)
		{
			DB_FSYNC(gv_cur_region, udi, csa, db_fsync_in_prog, save_errno);
			if (0 != save_errno)
			{
				send_msg(VARLSTCNT(5) ERR_DBFSYNCERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
				rts_error(VARLSTCNT(5) ERR_DBFSYNCERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
				assert(FALSE);	/* should not come here as the rts_error above should not return */
				return FALSE;
			}
		} else
		{
#ifndef NO_MSYNC
#ifndef TARGETED_MSYNC
			DB_FSYNC(gv_cur_region, udi, csa, db_fsync_in_prog, save_errno);
			if (0 != save_errno)
			{
				send_msg(VARLSTCNT(5) ERR_DBFSYNCERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
				rts_error(VARLSTCNT(5) ERR_DBFSYNCERR, 2, DB_LEN_STR(gv_cur_region), save_errno);
				assert(FALSE);	/* should not come here as the rts_error above should not return */
				return FALSE;
			}
#else
			if (-1 == msync((caddr_t)csa->db_addrs[0], (size_t)(csa->db_addrs[1] - csa->db_addrs[0]), MS_SYNC))
			{
				save_errno = errno;
				rts_error(VARLSTCNT(9) ERR_DBFILERR, 2, DB_LEN_STR(gv_cur_region),
					  ERR_TEXT, 2, RTS_ERROR_TEXT("Error during file msync during flush"), save_errno);
				assert(FALSE);	/* should not come here as the rts_error above should not return */
				return FALSE;
			}
#endif
#endif
		}
	}
	return TRUE;
}
