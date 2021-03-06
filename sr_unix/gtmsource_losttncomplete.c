/****************************************************************
 *								*
 *	Copyright 2006 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"
#include "gtm_stdio.h"

#include <errno.h>

#include "gdsroot.h"
#include "gdsblk.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "repl_msg.h"
#include "gtmsource.h"
#include "repl_instance.h"
#include "repl_shutdcode.h"
#include "repl_sem.h"
#include "util.h"
#include "cli.h"
#include "repl_log.h"

GBLREF	jnlpool_addrs		jnlpool;
GBLREF	boolean_t		holds_sem[NUM_SEM_SETS][NUM_SRC_SEMS];

int gtmsource_losttncomplete(void)
{
	int			idx;
	gtmsource_local_ptr_t	gtmsourcelocal_ptr;

	error_def(ERR_MUPCLIERR);
	error_def(ERR_TEXT);

	assert(holds_sem[SOURCE][JNL_POOL_ACCESS_SEM]);
	/* We dont need the access control semaphore here. So release it first and avoid any potential deadlocks. */
	if (0 != rel_sem(SOURCE, JNL_POOL_ACCESS_SEM))
		rts_error(VARLSTCNT(5) ERR_TEXT, 2, RTS_ERROR_LITERAL("Error in source server losttncomplete rel_sem"),
				REPL_SEM_ERRNO);
	assert(NULL == jnlpool.gtmsource_local);
	repl_log(stderr, TRUE, TRUE, "Initiating LOSTTNCOMPLETE operation on instance [%s]\n",
		jnlpool.repl_inst_filehdr->this_instname);
	/* If this is a root primary instance, propagate this information to secondaries as well so they reset zqgblmod_seqno to 0.
	 * If propagating primary, no need to send this to tertiaries as the receiver on the tertiary cannot have started with
	 * non-zero "zqgblmod_seqno" to begin with (PRIMARYNOTROOT error would have been issued).
	 */
	if (!jnlpool.jnlpool_ctl->upd_disabled)
	{
		grab_lock(jnlpool.jnlpool_dummy_reg);
		jnlpool.jnlpool_ctl->send_losttn_complete = TRUE;
		gtmsourcelocal_ptr = jnlpool.gtmsource_local_array;
		for (idx = 0; idx < NUM_GTMSRC_LCL; idx++, gtmsourcelocal_ptr++)
		{
			if (('\0' == gtmsourcelocal_ptr->secondary_instname[0])
					&& (0 == gtmsourcelocal_ptr->read_jnl_seqno)
					&& (0 == gtmsourcelocal_ptr->connect_jnl_seqno))
				continue;
			gtmsourcelocal_ptr->send_losttn_complete = TRUE;
		}
		rel_lock(jnlpool.jnlpool_dummy_reg);
	}
	/* Reset zqgblmod_seqno and zqgblmod_tn to 0 in this instance as well */
	repl_inst_reset_zqgblmod_seqno_and_tn();
	return (NORMAL_SHUTDOWN);
}
