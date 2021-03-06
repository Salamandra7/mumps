/****************************************************************
 *								*
 *	Copyright 2001, 2006 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/
#ifndef CMD_QLF_H_INCLUDED
#define CMD_QLF_H_INCLUDED

typedef struct
{
	uint4		qlf;
	mval		object_file;
	mval		list_file;
	mval		ceprep_file;
} command_qualifier;

typedef struct
{	unsigned short	page;		/* page number */
	unsigned short	list_line;	/* listing line number */
	unsigned short	lines_per_page;
	unsigned short	space;		/* spacing */
} list_params;

/* command qualifer bit masks */
#define CQ_LIST			(1 << 0)
#define CQ_MACHINE_CODE		(1 << 1)
#define CQ_CROSS_REFERENCE	(1 << 2)
#define CQ_DEBUG		(1 << 3)
#define CQ_OBJECT		(1 << 4)
#define CQ_WARNINGS		(1 << 5)
#define CQ_IGNORE		(1 << 6)
#define CQ_LOWER_LABELS		(1 << 7)
#define CQ_LINE_ENTRY		(1 << 8)
#define CQ_CE_PREPROCESS        (1 << 9)
#define CQ_INLINE_LITERALS	(1 << 10)
#define CQ_ALIGN_STRINGS	(1 << 11)
#define CQ_UTF8			(1 << 12)

/* TODO: add CQ_ALIGN_STRINGS to the default list below when alignment is supported */
#define CQ_DEFAULT (CQ_WARNINGS | CQ_OBJECT | CQ_IGNORE | CQ_LOWER_LABELS | CQ_LINE_ENTRY | CQ_INLINE_LITERALS)

#define LISTTAB 10
#define PG_WID 132

typedef struct src_line_type
{
	struct
	{
		struct src_line_type *fl,*bl;
	} que;
	char	*addr;
	int4	line;
} src_line_struct;

void zl_cmd_qlf(mstr *quals, command_qualifier *qualif);
void get_cmd_qlf(command_qualifier *qualif);

#endif /* CMD_QLF_H_INCLUDED */
