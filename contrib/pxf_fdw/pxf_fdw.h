/*
 * pxf_fdw.h
 *		  Foreign-data wrapper for PXF (Platform Extension Framework)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_fdw.h
 */

#include "postgres.h"

#include "access/formatter.h"
#include "commands/copy.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "utils/rel.h"

#ifndef _PXF_FDW_H_
#define _PXF_FDW_H_

#define PXF_SEGMENT_ID           GpIdentity.segindex
#define PXF_SEGMENT_COUNT        getgpsegmentCount()

/* in pxf_deparse.c */
extern void PxfDeparseTargetList(Relation rel, Bitmapset *attrs_used, List **retrieved_attrs);
extern void PxfClassifyConditions(PlannerInfo *root, RelOptInfo *baserel, List *input_conds, List **remote_conds, List **local_conds);

#endif							/* _PXF_FDW_H_ */
