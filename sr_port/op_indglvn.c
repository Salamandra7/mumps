/****************************************************************
 *								*
 *	Copyright 2001, 2004 Sanchez Computer Associates, Inc.	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"
#include "hashtab_mname.h"
#include "lv_val.h"
#include "compiler.h"
#include "opcode.h"
#include "indir_enum.h"
#include "toktyp.h"
#include "cache.h"
#include "hashtab_objcode.h"
#include "op.h"
#include "rtnhdr.h"
#include "valid_mname.h"
#include "hashtab.h"

GBLREF	bool			undef_inhibit;
GBLREF	symval			*curr_symval;
GBLREF	mval			**ind_result_sp, **ind_result_top;
LITREF	mval			literal_null;

void	op_indglvn(mval *v,mval *dst)
{
	bool		rval;
	mstr		*obj, object;
	oprtype		x;
	lv_val 		*a;
	icode_str	indir_src;
	lv_val		*lv;
	var_tabent	targ_key;
	ht_ent_mname	*tabent;

	error_def(ERR_INDMAXNEST);
	error_def(ERR_UNDEF);

	MV_FORCE_STR(v);
	indir_src.str = v->str;
	indir_src.code = indir_glvn;
	if (NULL == (obj = cache_get(&indir_src)))
	{
		if (valid_mname(&v->str))
		{
			targ_key.var_name = v->str;
			COMPUTE_HASH_MNAME(&targ_key);
			tabent = lookup_hashtab_mname(&curr_symval->h_symtab, &targ_key);
			assert(NULL == tabent ||  NULL != tabent->value);
			if (!tabent || !MV_DEFINED(&((lv_val *)tabent->value)->v))
			{
				if (undef_inhibit)
				{
					*dst = literal_null;
					return;
				}
				else
					rts_error(VARLSTCNT(4) ERR_UNDEF, 2, v->str.len, v->str.addr);
			}
			a = (lv_val *)tabent->value;
			*dst = a->v;
			return;
		}
		comp_init(&v->str);
		rval = glvn(&x);
		if (comp_fini(rval, &object, OC_IRETMVAL, &x, v->str.len))
		{
			indir_src.str.addr = v->str.addr;
			cache_put(&indir_src, &object);
			*ind_result_sp++ = dst;
			if (ind_result_sp >= ind_result_top)
				rts_error(VARLSTCNT(1) ERR_INDMAXNEST);
			comp_indr(&object);
		}
	}
	else
	{
		*ind_result_sp++ = dst;
		if (ind_result_sp >= ind_result_top)
			rts_error(VARLSTCNT(1) ERR_INDMAXNEST);
		comp_indr(obj);
	}
}
