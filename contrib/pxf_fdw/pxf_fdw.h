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

#ifndef PXF_FDW_H
#define PXF_FDW_H

#define PXF_SEGMENT_ID           GpIdentity.segindex
#define PXF_SEGMENT_COUNT        getgpsegmentCount()
#define PXF_FDW_DEFAULT_PROTOCOL "http"
#define PXF_FDW_DEFAULT_HOST     "localhost"
#define PXF_FDW_DEFAULT_PORT     5888

/* in pxf_deparse.c */
extern void deparseTargetList(Relation rel, Bitmapset *attrs_used, List **retrieved_attrs);
extern void classifyConditions(PlannerInfo *root, RelOptInfo *baserel, List *input_conds, List **remote_conds, List **local_conds);

#endif							/* _PXF_FDW_H_ */
