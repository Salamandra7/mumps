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

#include <stdarg.h>
#include "gtm_string.h"
#include "gdsroot.h"
#include "gdsblk.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "gdscc.h"
#include "gdskill.h"
#include "cdb_sc.h"
#include "hashtab_mname.h"	/* needed for lv_val.h */
#include "lv_val.h"
#include "jnl.h"
#include "mlkdef.h"
#include "rtnhdr.h"
#include "mv_stent.h"
#include "stack_frame.h"
#include "tp_frame.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h and cws_insert.h */
#include "tp.h"
#include "tp_timeout.h"
#include "op.h"
#include "have_crit.h"
#include "gtm_caseconv.h"
#include "gvcst_protos.h"	/* for gvcst_tp_init prototype */
#include "dpgbldir.h"
#include "longset.h"		/* needed for cws_insert.h */
#include "hashtab.h"
#include "cws_insert.h"		/* for cw_stagnate_reinitialized */

#ifdef UNICODE_SUPPORTED
#include "gtm_icu_api.h"	/* needed by *TYPEMASK* macros defined in gtm_utf8.h */
#include "gtm_utf8.h"
#endif

error_def(ERR_STACKCRIT);
error_def(ERR_STACKOFLOW);
error_def(ERR_TPMIXUP);
error_def(ERR_TPSTACKCRIT);
error_def(ERR_TPSTACKOFLOW);
error_def(ERR_TPTOODEEP);

GBLREF	jnl_fence_control	jnl_fence_ctl;
GBLREF	short			dollar_tlevel;
GBLREF	mval			dollar_zgbldir;
GBLREF	sgmnt_addrs		*cs_addrs;
GBLREF	gd_addr			*gd_header;
GBLREF	gv_key			*gv_currkey;
GBLREF	gv_namehead		*gv_target;
GBLREF	stack_frame		*frame_pointer;
GBLREF	tp_frame		*tp_pointer;
GBLREF	mv_stent		*mv_chain;
GBLREF	mlk_pvtblk		*mlk_pvt_root;
GBLREF	symval			*curr_symval;
GBLREF	unsigned char		*msp, *stacktop, *stackwarn, *tpstackbase, *tpstacktop, *tpstackwarn,
				*tp_sp, t_fail_hist[CDB_MAX_TRIES];
GBLREF  unsigned int		t_tries;
GBLREF	tp_region		*tp_reg_list, *tp_reg_free_list;
GBLREF	trans_num		local_tn;	/* transaction number for THIS PROCESS */
GBLREF	bool			is_standalone;
GBLREF 	void			(*tp_timeout_start_timer_ptr)(int4 tmout_sec);
GBLREF  sgm_info                *first_sgm_info;
GBLREF  global_tlvl_info	*global_tlvl_info_head;
GBLREF  buddy_list		*global_tlvl_info_list;
GBLREF  sgmnt_data_ptr_t	cs_data;
GBLREF  sgmnt_addrs		*cs_addrs;
GBLREF  gd_region		*gv_cur_region;
GBLREF	struct_jrec_tcom	tcom_record;
GBLREF	jnl_gbls_t		jgbl;
GBLREF	boolean_t		tp_in_use;
GBLREF	boolean_t		gtm_utf8_mode;
#ifdef VMS
GBLREF	boolean_t		tp_has_kill_t_cse; /* cse->mode of kill_t_write or kill_t_create got created in this transaction */
#endif

#ifdef DEBUG
GBLREF	uint4		donot_commit;	/* see gdsfhead.h for the purpose of this debug-only global */
#endif

#define NORESTART -1
#define ALLLOCAL  -2

/* Note gv_orig_key[i] is assigned to tp_pointer->orig_key which then tries to dereference the "begin", "end", "prev", "top"
 * 	fields like it were a gv_currkey pointer. Since these members are 2-byte fields, we need atleast 2 byte alignment.
 * We want to be safer and hence give 4-byte alignment by declaring the array as an array of integers.
 */
struct gv_orig_key_struct
{
	int4	gv_orig_key[TP_MAX_NEST + 1][DIVIDE_ROUND_UP((sizeof(gv_key) + MAX_KEY_SZ + 1), sizeof(int4))];
};
static  struct gv_orig_key_struct *gv_orig_key_ptr;

/*** temporary ***/
GBLREF int4	    		dollar_zmaxtptime;

void	op_tstart(int dollar_t, ...) /* value of $T when TSTART */
{
	bool			serial;			/* whether SERIAL keyword was present */
	int			prescnt,		/* number of names to save, -1 = no restart, -2 = preserve all */
				pres;
	lv_val			*lv, *var;
	mlk_pvtblk		*pre_lock;
	mlk_tp			*lck_tp;
	mval			*preserve,		/* list of names to save */
				*tid,			/* transaction id */
				*mvname;
	mv_stent		*mv_st_ent, *mvst_tmp, *mvst_prev;
	stack_frame		*fp, *fp_fix;
	tp_frame		*tf;
	tp_var			*restore_ent;
	unsigned char		*old_sp, *top, *tstack_ptr, *ptrstart, *ptrend, *ptrinvalidbegin;
	va_list			varlst, lvname;
	tp_region		*tr, *tr_next;
	sgm_info		*si;
	int4			tmout_sec;
	tlevel_info		*tli, *new_tli, *prev_tli;
	global_tlvl_info	*gtli, *new_gtli, *prev_gtli;
	kill_set		*ks, *prev_ks;
	jnl_format_buffer 	*jfb, *prev_jfb;
	gd_region		*r_top, *r_local;
	gd_addr			*addr_ptr;
	unsigned char		tp_bat[TP_BATCH_LEN];
	mname_entry		tpvent;
	ht_ent_mname		*tabent, *curent, *topent;
	sgmnt_addrs		*csa;
	int4			shift_size;

	/* If we haven't done any TP until now, turn the flag on to tell gvcst_init to
	   initialize it in any regions it opens from now on and initialize it in any
	   regions that are already open.
	*/
	if (!tp_in_use)
	{
		tp_in_use = TRUE;
		for (addr_ptr = get_next_gdr(NULL); addr_ptr; addr_ptr = get_next_gdr(addr_ptr))
		{
			for (r_local = addr_ptr->regions, r_top = r_local + addr_ptr->n_regions; r_local < r_top; r_local++)
			{
				if (r_local->open && !r_local->was_open &&
				    (dba_bg == r_local->dyn.addr->acc_meth || dba_mm == r_local->dyn.addr->acc_meth))
				{	/* Let's initialize those regions but only if it came through gvcst_init_sysops
					   (being a bg or mm region)
					*/
					gvcst_tp_init(r_local);
				}
			}
		}
	}

	if (0 != jnl_fence_ctl.level)
		rts_error(VARLSTCNT(4) ERR_TPMIXUP, 2, "An M", "a fenced logical");
	if (dollar_tlevel + 1 >= TP_MAX_NEST)
		rts_error(VARLSTCNT(1) ERR_TPTOODEEP);
	va_start(varlst, dollar_t);	/* no argument count first */
	serial = va_arg(varlst, int);
	tid = va_arg(varlst, mval *);
	prescnt = va_arg(varlst, int);
	MV_FORCE_STR(tid);
	if (0 == dollar_tlevel)
	{
		jnl_fence_ctl.fence_list = JNL_FENCE_LIST_END;
		jgbl.cumul_jnl_rec_len = 0;
		DEBUG_ONLY(jgbl.cumul_index = jgbl.cu_jnl_index = 0;)
		t_tries = (FALSE == is_standalone) ? 0 : CDB_STAGNATE;
		t_fail_hist[t_tries] = cdb_sc_normal;
		/* ensure that we don't have crit on any region at the beginning of a TP transaction (be it GT.M or MUPIP) */
		assert(0 == have_crit(CRIT_HAVE_ANY_REG));
		for (tr = tp_reg_list; NULL != tr; tr = tr_next)
		{	/* start with empty list, place all existing entries on free list */
			tp_reg_list = tr_next = tr->fPtr;	/* Remove from queue */
			tr->fPtr = tp_reg_free_list;
			tp_reg_free_list = tr; 			/* Place on free queue */
		}
		++local_tn;					/* Begin new local transaction */
		jgbl.wait_for_jnl_hard = TRUE;
		memset(tcom_record.jnl_tid, 0, TID_STR_SIZE);
		if (0 != tid->str.len)
		{
			if (!gtm_utf8_mode)
			{
				if (tid->str.len > TID_STR_SIZE)
					tid->str.len = TID_STR_SIZE;
			}
			UNICODE_ONLY(
			else
			{	/* In UTF8 mode, take only as many valid multi-byte characters as can fit in TID_STR_SIZE */
				if (gtm_utf8_mode)
				{
					MV_FORCE_LEN(tid); /* issues BADCHAR error if appropriate */
					if (tid->str.len > TID_STR_SIZE)
					{
						ptrstart = (unsigned char *)tid->str.addr;
						ptrend = ptrstart + TID_STR_SIZE;
						UTF8_LEADING_BYTE(ptrend, ptrstart, ptrinvalidbegin)
						tid->str.len = INTCAST(ptrinvalidbegin - ptrstart);
					}
				}
			}
			)
			assert(TID_STR_SIZE >= tid->str.len);
			memcpy(tcom_record.jnl_tid, (char *)tid->str.addr, tid->str.len);
			if ((TP_BATCH_SHRT == tid->str.len) || (TP_BATCH_LEN == tid->str.len))
			{
				lower_to_upper(tp_bat, (uchar_ptr_t)tid->str.addr, (int)tid->str.len);
				if (0 == memcmp(TP_BATCH_ID, tp_bat, tid->str.len))
					jgbl.wait_for_jnl_hard = FALSE;
			}
		}
		VMS_ONLY(tp_has_kill_t_cse = FALSE;)
		assert(!donot_commit);
	}
	assert((0 == cw_stagnate.size) || cw_stagnate_reinitialized);
		/* either cw_stagnate has not been initialized at all or previous-non-TP or tp_hist should have done CWS_RESET */
	if (prescnt > 0)
	{
		VAR_COPY(lvname, varlst);
		for (pres = 0;  pres < prescnt;  ++pres)
		{
			preserve = va_arg(lvname, mval *);
			MV_FORCE_STR(preserve);
		}
		va_end(lvname);
	}
	if (NULL == gd_header)
		gvinit();
	assert(NULL != gd_header);
	if (frame_pointer->old_frame_pointer->type & SFT_DM)
	{	/* Put a TPHOLD underneath dmode frame */
		assert(frame_pointer->old_frame_pointer->old_frame_pointer);
		fp = frame_pointer->old_frame_pointer->old_frame_pointer;
		top = (unsigned char *)(frame_pointer->old_frame_pointer + 1);
		old_sp = msp;
		shift_size = mvs_size[MVST_TPHOLD];
		msp -= shift_size;
		if (msp <= stackwarn)
		{
			va_end(varlst);
			if (msp <= stacktop)
			{
				msp = old_sp;
				rts_error(VARLSTCNT(1) ERR_STACKOFLOW);
			} else
				rts_error(VARLSTCNT(1) ERR_STACKCRIT);
		}
		memcpy(msp, old_sp, top - (unsigned char *)old_sp);
		mv_st_ent = (mv_stent *)(top - shift_size);
		mv_st_ent->mv_st_type = MVST_TPHOLD;
		ADJUST_FRAME_POINTER(frame_pointer, shift_size);
		for (fp_fix = frame_pointer;  fp_fix != fp;  fp_fix = fp_fix->old_frame_pointer)
		{
			if ((unsigned char *)fp_fix->l_symtab < top  &&  (unsigned char *)fp_fix->l_symtab > stacktop)
				fp_fix->l_symtab = (mval **)((char *)fp_fix->l_symtab - shift_size);
			if (fp_fix->temps_ptr < top  &&  fp_fix->temps_ptr > stacktop)
				fp_fix->temps_ptr -= shift_size;
			if (fp_fix->vartab_ptr < (char *)top  &&  fp_fix->vartab_ptr > (char *)stacktop)
				fp_fix->vartab_ptr -= shift_size;
			if ((unsigned char *)fp_fix->old_frame_pointer < top  &&
			   (char *)fp_fix->old_frame_pointer > (char *)stacktop)
			{
				ADJUST_FRAME_POINTER(fp_fix->old_frame_pointer, shift_size);
			}
		}
		if ((unsigned char *)mv_chain >= top)
		{
			mv_st_ent->mv_st_next = (unsigned int)((char *)mv_chain - (char *)mv_st_ent);
			mv_chain = mv_st_ent;
		} else
		{
			top -= shift_size + sizeof(stack_frame);
			mv_chain = (mv_stent *)((char *)mv_chain - shift_size);
			mvst_tmp = mv_chain;
			mvst_prev = (mv_stent *)((char *)mvst_tmp + mvst_tmp->mv_st_next);
			while (mvst_prev < (mv_stent *)top)
			{
				mvst_tmp = mvst_prev;
				mvst_prev = (mv_stent *)((char *)mvst_tmp + mvst_tmp->mv_st_next);
			}
			mvst_tmp->mv_st_next = (unsigned int)((char *)mv_st_ent - (char *)mvst_tmp);
			mv_st_ent->mv_st_next = (unsigned int)((char *)mvst_prev - (char *)mv_st_ent + shift_size);

		}
	} else
	{
		PUSH_MV_STENT(MVST_TPHOLD);
		mv_st_ent = mv_chain;
		fp = frame_pointer;
	}
	mv_st_ent->mv_st_cont.mvs_tp_holder = dollar_tlevel;
	if (NULL == tpstackbase)
	{
		tstack_ptr = (unsigned char *)malloc(32768);
		tp_sp = tpstackbase = tstack_ptr + 32768;
		tpstacktop = tstack_ptr;
		tpstackwarn = tpstacktop + 1024;
		tp_pointer = NULL;
	}

	/* Add a new tp_frame in the TP stack */
	/* Pre V5, tf allocation was
	 * tf = (tp_frame *)(tp_sp -= sizeof(tp_frame) + sizeof(mident) * pres);
	 * The (sizeof(mident) * pres) area *may* have been for saving the names of preserved variables. We say *may*
	 * because we could not locate such logic in the history of op_tstart(). We don't need the name of a local
	 * variable to save and restore it (refer to the logic maintaining tf->vars). To avoid wasting space,
	 * we no longer allocate the extra "sizeof(mident) * pres" effective V5. */
	tf = (tp_frame *)(tp_sp -= sizeof(tp_frame));
	if (tp_sp < tpstackwarn)
	{
		va_end(varlst);
		rts_error(VARLSTCNT(1) tp_sp < tpstacktop ? ERR_TPSTACKOFLOW : ERR_TPSTACKCRIT);
	}
	tf->dlr_t = dollar_t;
	tf->restart_pc = fp->mpc;
	tf->restart_ctxt = fp->ctxt;
	tf->fp = fp;
	tf->serial = serial;
	tf->trans_id = *tid;
	tf->restartable = (NORESTART != prescnt);
	tf->old_locks = (NULL != mlk_pvt_root);
	tf->orig_gv_target = gv_target;
	DBG_CHECK_GVTARGET_CSADDRS_IN_SYNC;
#	ifdef DEBUG
	if (!jgbl.forw_phase_recovery)
	{	/* In case of forward phase of journal recovery, gv_currkey is set by caller (mur_forward) only
		 * after the call to op_tstart so avoid doing gv_currkey check.
		 */
		DBG_CHECK_GVTARGET_GVCURRKEY_IN_SYNC;
	}
#	endif
	/* If the TP structures have not yet been initialized, do that now. */
	if (NULL == gv_orig_key_ptr)
	{	/* This need only be set once */
		gv_orig_key_ptr = (struct gv_orig_key_struct *)malloc(sizeof(struct gv_orig_key_struct));
		memset(gv_orig_key_ptr, 0, sizeof(struct gv_orig_key_struct));
	}
	tf->orig_key = (gv_key *)&(gv_orig_key_ptr->gv_orig_key[dollar_tlevel][0]);
	assert(NULL != gv_currkey);
	memcpy(tf->orig_key, gv_currkey, sizeof(gv_key) + gv_currkey->end);
	tf->gd_header = gd_header;
	tf->gd_reg = gv_cur_region;
	tf->zgbldir = dollar_zgbldir;
	tf->sym = (symval *)NULL;
	tf->vars = (tp_var *)NULL;
	tf->old_tp_frame = tp_pointer;
	tp_pointer = tf;
	if (prescnt > 0)
	{
		VAR_COPY(lvname, varlst);
		for (pres = 0;  pres < prescnt;  ++pres)
		{
			preserve = va_arg(lvname, mval *);
			/* Note: the assumption (according to the comment below) is that this mval points into the literal table
			   and thus could not possibly be undefined. In that case, I do not understand why the earlier loop to
			   do MV_FORCE_STR on these variables. Future todo -- verify if that loop is needed. On the assumption
			   that it is not, the below assert will verify that the mval is defined to catch any NOUNDEF case.
			*/
			assert(MV_DEFINED(preserve));
			/* The incoming 'preserve' is the pointer to a literal mval table entry. For the indirect code
			 * (eg. Direct Mode), since the literal table is no longer on the M stack, we should not shift
			 * the incoming va_arg pointer (C9D01-002205) */
			mvname = preserve;
			if (0 != mvname->str.len)
			{	/* Convert mval to mident and see if it's in the symbol table */
				tpvent.var_name.len = mvname->str.len;
				tpvent.var_name.addr = mvname->str.addr;
				COMPUTE_HASH_MNAME(&tpvent);
				if (add_hashtab_mname(&curr_symval->h_symtab, &tpvent, NULL, &tabent))
					lv_newname(tabent, curr_symval);
				lv = (lv_val *)tabent->value;
				/* In order to allow restart of a sub-transaction, this should chain rather than back stop,
				   with appropriate changes to tp_var_clone and tp_unwind */
				if (NULL == lv->tp_var)
				{
					/* Suggested enhancement: create space for restore_ent in the TP frame instead of
					 * malloc'ing for each preserved variable. This will improve memory efficiency by
					 * eliminating need for linked list. This may also assist in implementing restarting
					 * of sub-transactions where we can limit the store/restore logic to locals for the
					 * sub-transaction. With linked list, there is no straight-forward way to tell if
					 * an lv was specified in a sub-transaction, and if so, which sub-transaction. */
					var = lv_getslot(lv->ptrs.val_ent.parent.sym);
					restore_ent = (tp_var *)malloc(sizeof(tp_var));
					restore_ent->current_value = lv;
					restore_ent->save_value = var;
					restore_ent->next = tf->vars;
					lv->tp_var = var;
					*var = *lv;
					tf->vars = restore_ent;
				}
			}
		}
		va_end(lvname);
	}
	else if (ALLLOCAL == prescnt)
	{	/* Preserve all variables */
		tf->sym = curr_symval;
		++curr_symval->tp_save_all;
		for (curent = curr_symval->h_symtab.base, topent = curr_symval->h_symtab.top;  curent < topent;  curent++)
		{
			if (HTENT_VALID_MNAME(curent, lv_val, lv))
			{
				if (NULL == lv->tp_var)
				{
					var = lv_getslot(lv->ptrs.val_ent.parent.sym);
					restore_ent = (tp_var *)malloc(sizeof(tp_var));
					restore_ent->current_value = lv;
					restore_ent->save_value = var;
					restore_ent->next = tf->vars;
					lv->tp_var = var;
					*var = *lv;
					tf->vars = restore_ent;
				}
			}
		}
	}
	va_end(varlst);
	/* Store existing state of locks */
	for (pre_lock = mlk_pvt_root;  NULL != pre_lock;  pre_lock = pre_lock->next)
	{
		if ((NULL == pre_lock->tp) || (pre_lock->tp->level != pre_lock->level)
			|| (pre_lock->tp->zalloc != pre_lock->zalloc))
		{	/* Only stack locks that have changed since last TSTART */
			lck_tp = (mlk_tp *)malloc(sizeof(mlk_tp));
			lck_tp->tplevel = dollar_tlevel;
			lck_tp->level = pre_lock->level;
			lck_tp->zalloc = pre_lock->zalloc;
			lck_tp->next = pre_lock->tp;
			pre_lock->tp = lck_tp;
		}
	}
	++dollar_tlevel;
	/* Store the global (across all segments) dollar_tlevel specific information. Curently, it holds only jnl related info. */
	if (JNL_FENCE_LIST_END != jnl_fence_ctl.fence_list)
	{
		for (prev_gtli = NULL, gtli = global_tlvl_info_head; gtli; gtli = gtli->next_global_tlvl_info)
			prev_gtli = gtli;
		new_gtli = (global_tlvl_info *)get_new_element(global_tlvl_info_list, 1);
		new_gtli->global_tlvl_fence_info = jnl_fence_ctl.fence_list;
		new_gtli->t_level = dollar_tlevel;
		new_gtli->next_global_tlvl_info = NULL;
		if (prev_gtli)
		{
			assert(prev_gtli->t_level + 1 == dollar_tlevel);
			prev_gtli->next_global_tlvl_info = new_gtli;
		} else
			global_tlvl_info_head = new_gtli;
	}
	/* Store the dollar_tlevel specific information */
	for (si = first_sgm_info; NULL != si; si = si->next_sgm_info)
	{
		for (prev_tli = NULL, tli = si->tlvl_info_head; (NULL != tli); tli = tli->next_tlevel_info)
			prev_tli = tli;
		new_tli = (tlevel_info *)get_new_element(si->tlvl_info_list, 1);
		new_tli->t_level = dollar_tlevel;
		for (prev_ks = NULL, ks = si->kill_set_head; ks; ks = ks->next_kill_set)
			prev_ks = ks;
		new_tli->tlvl_kill_set = prev_ks;
		if (prev_ks)
			new_tli->tlvl_kill_used = prev_ks->used;
		else
			new_tli->tlvl_kill_used = 0;
		new_tli->tlvl_tp_hist_info = si->last_tp_hist;
		/* Prepare for journaling logical records if journaling is enabled on this region OR if replication was
		 * allowed on this region (this is a case where replication was ON originally but later transitioned
		 * into WAS_ON state and journaling got turned OFF.
		 */
		csa = si->tp_csa;
		if (JNL_WRITE_LOGICAL_RECS(csa))
		{
			for (prev_jfb = NULL, jfb = si->jnl_head; jfb; jfb = jfb->next)
				prev_jfb = jfb;
			new_tli->tlvl_jfb_info = prev_jfb;
			new_tli->tlvl_cumul_jrec_len = jgbl.cumul_jnl_rec_len;
			DEBUG_ONLY(new_tli->tlvl_cumul_index = jgbl.cumul_index;)
		}
		new_tli->next_tlevel_info = NULL;
		new_tli->update_trans = si->update_trans;
		if (prev_tli)
			prev_tli->next_tlevel_info = new_tli;
		else
			si->tlvl_info_head = new_tli;
	}
	/* ------------------------------------------------------------------
	 * Start TP timer if set to non-default value
	 * ------------------------------------------------------------------
	 */
	tmout_sec = dollar_zmaxtptime;
	assert(0 <= tmout_sec);
	if (0 < tmout_sec)
	{
		/* ------------------------------------------------------------------
		 * Only start timer at outer level
		 * ------------------------------------------------------------------
		 */
		if (1 == dollar_tlevel)
		{
			(*tp_timeout_start_timer_ptr)(tmout_sec);
		} else
		{
			/* Confirm that timeout already pending, else warning */
		}
	}
}
