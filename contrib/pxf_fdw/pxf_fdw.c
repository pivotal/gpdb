/*
 * pxf_fdw.c
 *		  Foreign-data wrapper for PXF (Platform Extension Framework)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_fdw.c
 */

#include "postgres.h"

#include "pxf_fdw.h"
#include "pxf_controller.h"
#include "pxf_filter.h"

#include "access/reloptions.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/pg_list.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

#define DEFAULT_PXF_FDW_STARTUP_COST   50000

typedef struct PxfFdwRelationInfo
{
	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

	/* List of attributes (columns) that we need to get */
	List	   *retrieved_attrs;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;
}			PxfFdwRelationInfo;

/*
 * Workspace for analyzing a foreign table.
 */
typedef struct PxfFdwAnalyzeState
{
	Relation	rel;			/* relcache entry for the foreign table */
	List	   *retrieved_attrs;	/* attr numbers retrieved by query */

	/* collected sample rows */
	HeapTuple  *rows;			/* array of size targrows */
	int			targrows;		/* target # of sample rows */
	int			numrows;		/* # of sample rows collected */

	/* for random sampling */
	double		samplerows;		/* # of rows fetched */
	double		rowstoskip;		/* # of rows to skip before next sample */
	double		rstate;			/* random state */

	/* working memory contexts */
	MemoryContext anl_cxt;		/* context for per-analyze lifespan data */
} PxfFdwAnalyzeState;

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Currently we store:
 *
 * 1) WHERE clause text to be sent to the remote server
 * 2) Integer list of attribute numbers retrieved by the SELECT
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the WHERE clauses:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateWhereClauses));
 */
enum FdwScanPrivateIndex
{
	/* WHERE clauses to be sent to PXF (as a String node) */
	FdwScanPrivateWhereClauses,
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs
};

extern Datum pxf_fdw_handler(PG_FUNCTION_ARGS);

/*
 * on-load initializer
 */
extern PGDLLEXPORT void _PG_init(void);

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(pxf_fdw_handler);
PG_FUNCTION_INFO_V1(pxf_fdw_validator);

/*
 * FDW functions declarations
 */
static void pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void pxfGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);

#if (PG_VERSION_NUM <= 90500)
static ForeignScan *pxfGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses);
#else
static ForeignScan *pxfGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);
#endif

static void pxfExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void pxfBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *pxfIterateForeignScan(ForeignScanState *node);
static void pxfReScanForeignScan(ForeignScanState *node);
static void pxfEndForeignScan(ForeignScanState *node);

/* Analyze */
static bool pxfAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages);

/* Foreign updates */
static void pxfBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private, int subplan_index, int eflags);
static TupleTableSlot *pxfExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot);
static void pxfEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo);
static int	pxfIsForeignRelUpdatable(Relation rel);

/*
 * Helper functions
 */
static void InitCopyState(PxfFdwScanState *pxfsstate);
static void InitCopyStateForModify(PxfFdwModifyState *pxfmstate);
static CopyState BeginCopyTo(Relation forrel, List *options);
static int PxfAcquireSampleRowsFunc(Relation relation, int elevel, HeapTuple *rows, int targrows, double *totalrows, double *totaldeadrows);
static void AnalyzeRowProcessor(TupleTableSlot *slot, PxfFdwAnalyzeState *astate);
static void PxfAbortCallback(ResourceReleasePhase phase, bool isCommit, bool isTopLevel, void *arg);

/*
 * Foreign-data wrapper handler functions:
 * returns a struct with pointers to the
 * pxf_fdw callback routines.
 */
Datum
pxf_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdw_routine = makeNode(FdwRoutine);

	/*
	 * foreign table scan support
	 */

	/* master - only */
	fdw_routine->GetForeignRelSize = pxfGetForeignRelSize;
	fdw_routine->GetForeignPaths = pxfGetForeignPaths;
	fdw_routine->GetForeignPlan = pxfGetForeignPlan;
	fdw_routine->ExplainForeignScan = pxfExplainForeignScan;

	/* segment - only when mpp_execute = segments */
	fdw_routine->BeginForeignScan = pxfBeginForeignScan;
	fdw_routine->IterateForeignScan = pxfIterateForeignScan;
	fdw_routine->ReScanForeignScan = pxfReScanForeignScan;
	fdw_routine->EndForeignScan = pxfEndForeignScan;

	/*
	 * foreign table insert support
	 */

	/*
	 * AddForeignUpdateTargets set to NULL, no extra target expressions are
	 * added
	 */
	fdw_routine->AddForeignUpdateTargets = NULL;

	/*
	 * PlanForeignModify set to NULL, no additional plan-time actions are
	 * taken
	 */
	fdw_routine->PlanForeignModify = NULL;
	fdw_routine->BeginForeignModify = pxfBeginForeignModify;
	fdw_routine->ExecForeignInsert = pxfExecForeignInsert;

	/*
	 * ExecForeignUpdate and ExecForeignDelete set to NULL since updates and
	 * deletes are not supported
	 */
	fdw_routine->ExecForeignUpdate = NULL;
	fdw_routine->ExecForeignDelete = NULL;
	fdw_routine->EndForeignModify = pxfEndForeignModify;
	fdw_routine->IsForeignRelUpdatable = pxfIsForeignRelUpdatable;

	/* Support functions for ANALYZE */
	fdw_routine->AnalyzeForeignTable = pxfAnalyzeForeignTable;

	PG_RETURN_POINTER(fdw_routine);
}

/*
 * _PG_init
 * 		Library load-time initialization.
 * 		Sets PxfExitHook() callback for backend shutdown.
 */
void
_PG_init(void)
{
//	if ((pgver >= 90600 && pgver <= 90608)
//		|| (pgver >= 100000 && pgver <= 100003))
//		ereport(ERROR,
//		        (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
//			        errmsg("PostgreSQL version \"%s\" not supported by pxf_fdw",
//			               GetConfigOptionByName("server_version", NULL)),
//			        errhint("You'll have to update PostgreSQL to a later minor release.")));
}

/*
 * GetForeignRelSize
 *		set relation size estimates for a foreign table
 */
static void
pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize starts on segment: %d", PXF_SEGMENT_ID);
	Relation	rel;
	ListCell   *lc;

	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) palloc(sizeof(PxfFdwRelationInfo));

	baserel->fdw_private = (void *) fpinfo;

	fpinfo->attrs_used = NULL;

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);

	rel = heap_open(rte->relid, NoLock);

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	classifyConditions(root, baserel, baserel->baserestrictinfo,
					   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server
	 */
#if (PG_VERSION_NUM <= 90500)
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid, &fpinfo->attrs_used);
#else
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid, &fpinfo->attrs_used);
#endif
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid, &fpinfo->attrs_used);
	}

	deparseTargetList(rel, fpinfo->attrs_used, &fpinfo->retrieved_attrs);

	heap_close(rel, NoLock);

	/* Use an artificial number of estimated rows */
	baserel->rows = 1000;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * GetForeignPaths
 *		create access path for a scan on the foreign table
 */
static void
pxfGetForeignPaths(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid)
{
	ForeignPath *path = NULL;
	int			total_cost = DEFAULT_PXF_FDW_STARTUP_COST;
	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) baserel->fdw_private;


	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths starts on segment: %d", PXF_SEGMENT_ID);


	path = create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
								   NULL,	/* default pathtarget */
#endif
								   baserel->rows,
								   DEFAULT_PXF_FDW_STARTUP_COST,
								   total_cost,
								   NIL, /* no pathkeys */
								   NULL,	/* no outer rel either */
#if PG_VERSION_NUM >= 90500
								   NULL,	/* no extra plan */
#endif
								   fpinfo->retrieved_attrs);



	/*
	 * Create a ForeignPath node and add it as only possible path.
	 */
	add_path(baserel, (Path *) path);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * GetForeignPlan
 *		create a ForeignScan plan node
 */
#if PG_VERSION_NUM >= 90500
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,
				  List *scan_clauses,
				  Plan *outer_plan)
#else
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,	/* target list */
				  List *scan_clauses)
#endif
{
	Index		scan_relid = baserel->relid;
	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) baserel->fdw_private;
	List	 *fdw_private;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan starts on segment: %d", PXF_SEGMENT_ID);

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwScanPrivateIndex, above.
	 */

	/* here we serialize the WHERE clauses */
	char	   *where_clauses_str = SerializePxfFilterQuals(fpinfo->remote_conds);

	fdw_private = list_make2(makeString(where_clauses_str), fpinfo->retrieved_attrs);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan ends on segment: %d", PXF_SEGMENT_ID);

	return make_foreignscan(
							tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							fdw_private
#if PG_VERSION_NUM >= 90500
							,NIL
							,NIL
							,outer_plan
#endif
		);

}

/*
 * pxfExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
pxfExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	List *fdw_private;
	char *filter_str;
	List *retrieved_attrs;
	int retrieved_attrs_length, counter;

	if (es->verbose)
	{
		fdw_private            = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		filter_str             = strVal(list_nth(fdw_private, FdwScanPrivateWhereClauses));
		retrieved_attrs        = (List *) list_nth(fdw_private, FdwScanPrivateRetrievedAttrs);
		retrieved_attrs_length = list_length(retrieved_attrs);

		if (filter_str)
		{
			ExplainPropertyText("Serialized filter string", filter_str, es);
		}

		if (retrieved_attrs_length > 0)
		{
			StringInfoData columnProjection;
			initStringInfo(&columnProjection);

			ListCell *lc1 = NULL;
			foreach_with_count(lc1, retrieved_attrs, counter)
			{
				int attno = lfirst_int(lc1);
				appendStringInfo(&columnProjection, "%d", attno);
				if (counter < retrieved_attrs_length - 1) {
					appendStringInfo(&columnProjection, ", ");
				}
			}

			ExplainPropertyText("Column projection attributes", columnProjection.data, es);
			resetStringInfo(&columnProjection);
		}
	}

	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * BeginForeignScan
 *   called during executor startup. perform any initialization
 *   needed, but not start the actual scan.
 */
static void
pxfBeginForeignScan(ForeignScanState *node, int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	ForeignTable *rel = GetForeignTable(RelationGetRelid(node->ss.ss_currentRelation));

	/*
	 * Master does not scan when the exec location is all segments
	 */
	if (rel->exec_location == FTEXECLOCATION_ALL_SEGMENTS && Gp_role == GP_ROLE_DISPATCH)
		return;

	List	   *quals             = node->ss.ps.qual;
	Oid			foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
	PxfFdwScanState *pxfsstate    = NULL;
	Relation	relation          = node->ss.ss_currentRelation;
	ForeignScan *foreignScan      = (ForeignScan *) node->ss.ps.plan;
	PxfOptions *options           = PxfGetOptions(foreigntableid);

	/* retrieve fdw-private information from pxfGetForeignPlan() */
	char *filter_str              = strVal(list_nth(foreignScan->fdw_private, FdwScanPrivateWhereClauses));
	List *retrieved_attrs         = (List *) list_nth(foreignScan->fdw_private, FdwScanPrivateRetrievedAttrs);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	pxfsstate = (PxfFdwScanState *) palloc(sizeof(PxfFdwScanState));
	initStringInfo(&pxfsstate->uri);

	pxfsstate->filter_str = filter_str;
	pxfsstate->options = options;
	pxfsstate->quals = quals;
	pxfsstate->relation = relation;
	pxfsstate->retrieved_attrs = retrieved_attrs;

	InitCopyState(pxfsstate);
	node->fdw_state = (void *) pxfsstate;

	/*
	 * Register a callback to cleanup curl resources post-abort
	 */
	RegisterResourceReleaseCallback(PxfAbortCallback, &node);

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * IterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 *   Fetch one row from the foreign source, returning it in a tuple table slot
 *    (the node's ScanTupleSlot should be used for this purpose).
 *  Return NULL if no more rows are available.
 */
static TupleTableSlot *
pxfIterateForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfIterateForeignScan Executing on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	ErrorContextCallback errcallback;
	bool		found;

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) pxfsstate->cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file, we
	 * just skip the last step, leaving the slot empty as required.
	 *
	 * We can pass ExprContext = NULL because we read all columns from the
	 * file, so no need to evaluate default expressions.
	 *
	 * We can also pass tupleOid = NULL because we don't allow oids for
	 * foreign tables.
	 */
	ExecClearTuple(slot);

	found = NextCopyFrom(pxfsstate->cstate,
						 NULL,
						 slot_get_values(slot),
						 slot_get_isnull(slot),
						 NULL);
	if (found)
	{
		if (pxfsstate->cstate->cdbsreh)
		{
			/*
			 * If NextCopyFrom failed, the processed row count will have
			 * already been updated, but we need to update it in a successful
			 * case.
			 *
			 * GPDB_91_MERGE_FIXME: this is almost certainly not the right
			 * place for this, but row counts are currently scattered all over
			 * the place. Consolidate.
			 */
			pxfsstate->cstate->cdbsreh->processed++;
		}

		ExecStoreVirtualTuple(slot);
	}

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	return slot;
}

/*
 * ReScanForeignScan
 *		Restart the scan from the beginning
 */
static void
pxfReScanForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	EndCopyFrom(pxfsstate->cstate);
	InitCopyState(pxfsstate);

	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * EndForeignScan
 *		End the scan and release resources.
 */
static void
pxfEndForeignScan(ForeignScanState *node)
{
	elog(DEBUG1, "pxf_fdw: pxfEndForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	ForeignScan *foreignScan = (ForeignScan *) node->ss.ps.plan;
	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	/* Release resources */
	if (foreignScan->fdw_private)
	{
		elog(DEBUG5, "Freeing fdw_private");
		pfree(foreignScan->fdw_private);
	}

	/* if pxfsstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfsstate)
	{
		EndCopyFrom(pxfsstate->cstate);

		if (pxfsstate->churl_handle)
		{
			churl_cleanup(pxfsstate->churl_handle, false);
			pxfsstate->churl_handle = NULL;
		}

		if (pxfsstate->curl_headers)
		{
			churl_headers_cleanup(pxfsstate->curl_headers);
			pxfsstate->curl_headers = NULL;
		}
	}

	elog(DEBUG1, "pxf_fdw: pxfEndForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 */
static void
pxfBeginForeignModify(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo,
					  List *fdw_private,
					  int subplan_index,
					  int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfOptions *options = NULL;
	PxfFdwModifyState *pxfmstate = NULL;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupDesc;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	tupDesc = RelationGetDescr(relation);
	options = PxfGetOptions(RelationGetRelid(relation));
	pxfmstate = palloc(sizeof(PxfFdwModifyState));

	initStringInfo(&pxfmstate->uri);
	pxfmstate->relation = relation;
	pxfmstate->options = options;
	pxfmstate->values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	pxfmstate->nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	InitCopyStateForModify(pxfmstate);

	resultRelInfo->ri_FdwState = pxfmstate;

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfExecForeignInsert
 *		Insert one row into a foreign table
 */
static TupleTableSlot *
pxfExecForeignInsert(EState *estate,
					 ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;
	CopyState	cstate = pxfmstate->cstate;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupDesc = RelationGetDescr(relation);
	HeapTuple	tuple = ExecMaterializeSlot(slot);
	Datum	   *values = pxfmstate->values;
	bool	   *nulls = pxfmstate->nulls;

	/* TEXT or CSV */
	heap_deform_tuple(tuple, tupDesc, values, nulls);
	CopyOneRowTo(cstate, HeapTupleGetOid(tuple), values, nulls);
	CopySendEndOfRow(cstate);

	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	int			bytes_written = PxfControllerWrite(pxfmstate, fe_msgbuf->data, fe_msgbuf->len);

	if (bytes_written == -1)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to foreign resource: %m")));
	}

	elog(DEBUG3, "pxf_fdw %d bytes written", bytes_written);

	/* Reset our buffer to start clean next round */
	cstate->fe_msgbuf->len = 0;
	cstate->fe_msgbuf->data[0] = '\0';

	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert ends on segment: %d", PXF_SEGMENT_ID);
	return slot;
}

/*
 * pxfEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
pxfEndForeignModify(EState *estate,
					ResultRelInfo *resultRelInfo)
{
	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If pxfmstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfmstate == NULL)
		return;

	EndCopyFrom(pxfmstate->cstate);
	PxfControllerCleanup(pxfmstate);

	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfIsForeignRelUpdatable
 *  Assume table is updatable regardless of settings.
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 */
static int
pxfIsForeignRelUpdatable(Relation rel)
{
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable starts on segment: %d", PXF_SEGMENT_ID);
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable ends on segment: %d", PXF_SEGMENT_ID);
	/* Only INSERT is allowed at the moment */
	return 1u << (unsigned int) CMD_INSERT | 0u << (unsigned int) CMD_UPDATE | 0u << (unsigned int) CMD_DELETE;
}

/*
 * pxfAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
pxfAnalyzeForeignTable(Relation relation,
					   AcquireSampleRowsFunc *func,
					   BlockNumber *totalpages)
{
	elog(DEBUG5, "pxf_fdw: pxfAnalyzeForeignTable starts on segment: %d", PXF_SEGMENT_ID);
	ForeignTable *table;
	UserMapping *user;
	long		total_size = 0;

	/* Return the row-analysis function pointer */
	*func = PxfAcquireSampleRowsFunc;

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);

	/*
	 * Ask PXF to calculate the total size of the dataset on the external
	 * system, this can be exact (in the case of HCFS) or inexact (in the case
	 * of JDBC)
	 */
	/* TODO: get the total_size from PXF */
	/* total_size = PxfAnalyzeRetrieveTotalSize(table, user); */
	total_size = 0;

	/*
	 * The total number of pages equals to the total size of the dataset,
	 * divided by Postgres's block size
	 */
	*totalpages = total_size / BLCKSZ;

	/*
	 * Must return at least 1 so that we can tell later on that
	 * pg_class.relpages is not default.
	 */
	if (*totalpages < 1)
		*totalpages = 1;

	elog(DEBUG5, "pxf_fdw: pxfAnalyzeForeignTable ends on segment: %d", PXF_SEGMENT_ID);
	return true;
}

/*
 * Acquire a random sample of rows from foreign table managed by pxf_fdw.
 *
 * We fetch the whole table from the remote side and pick out some sample rows.
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the table and return it into
 * *totalrows.  Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the table.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int
PxfAcquireSampleRowsFunc(Relation relation, int elevel,
                         HeapTuple *rows, int targrows,
                         double *totalrows,
                         double *totaldeadrows)
{
	elog(DEBUG2, "pxf_fdw: PxfAcquireSampleRowsFunc starts on segment: %d. Sample %d rows", PXF_SEGMENT_ID, targrows);
	PxfFdwAnalyzeState astate;
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;

	/* Initialize workspace state */
	astate.rel = relation;
	astate.rows = rows;
	astate.targrows = targrows;
	astate.numrows = 0;
	astate.samplerows = 0;
	astate.rowstoskip = -1;		/* -1 means not set yet */
	astate.rstate = anl_init_selection_state(targrows);

	/* Remember ANALYZE context */
	astate.anl_cxt = CurrentMemoryContext;

	/*
	 *
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);

//	*totalRows = PxfAnalyzeRetrieveTotalRows(table, user);
//
//	if (*totalpages < targrows)
//	{
//		// PXF Scan the entire table as the user that owns the table
//	}
//	else
//	{
//		// Sample rows
//	}

	// TODO: read targrows rows
	// TODO: read total row count on the external system

//	/* In what follows, do not risk leaking any PGresults. */
//	PG_TRY();
//			{
//				char		fetch_sql[64];
//				int			fetch_size;
//				ListCell   *lc;
//
//				res = pgfdw_exec_query(conn, sql.data);
//				if (PQresultStatus(res) != PGRES_COMMAND_OK)
//					pgfdw_report_error(ERROR, res, conn, false, sql.data);
//				PQclear(res);
//				res = NULL;
//
//				/*
//				 * Determine the fetch size.  The default is arbitrary, but shouldn't
//				 * be enormous.
//				 */
//				fetch_size = 100;
//				foreach(lc, server->options)
//				{
//					DefElem    *def = (DefElem *) lfirst(lc);
//
//					if (strcmp(def->defname, "fetch_size") == 0)
//					{
//						fetch_size = strtol(defGetString(def), NULL, 10);
//						break;
//					}
//				}
//				foreach(lc, table->options)
//				{
//					DefElem    *def = (DefElem *) lfirst(lc);
//
//					if (strcmp(def->defname, "fetch_size") == 0)
//					{
//						fetch_size = strtol(defGetString(def), NULL, 10);
//						break;
//					}
//				}
//
//				/* Construct command to fetch rows from remote. */
//				snprintf(fetch_sql, sizeof(fetch_sql), "FETCH %d FROM c%u",
//				         fetch_size, cursor_number);
//
//				/* Retrieve and process rows a batch at a time. */
//				for (;;)
//				{
//					int			numrows;
//					int			i;
//
//					/* Allow users to cancel long query */
//					CHECK_FOR_INTERRUPTS();
//
//					/*
//					 * XXX possible future improvement: if rowstoskip is large, we
//					 * could issue a MOVE rather than physically fetching the rows,
//					 * then just adjust rowstoskip and samplerows appropriately.
//					 */
//
//					/* Fetch some rows */
//					res = pgfdw_exec_query(conn, fetch_sql);
//					/* On error, report the original query, not the FETCH. */
//					if (PQresultStatus(res) != PGRES_TUPLES_OK)
//						pgfdw_report_error(ERROR, res, conn, false, sql.data);
//
//					/* Process whatever we got. */
//					numrows = PQntuples(res);
//					for (i = 0; i < numrows; i++)
//						AnalyzeRowProcessor(res, i, &astate);
//
//					PQclear(res);
//					res = NULL;
//
//					/* Must be EOF if we didn't get all the rows requested. */
//					if (numrows < fetch_size)
//						break;
//				}
//			}
//		PG_CATCH();
//			{
//				if (res)
//					PQclear(res);
//				PG_RE_THROW();
//			}
//	PG_END_TRY();

	/* We assume that we have no dead tuples. */
	*totaldeadrows = 0.0;

	/* We've retrieved all living tuples from foreign server. */
	*totalrows = astate.samplerows;

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": table contains %.0f rows, %d rows in sample",
					RelationGetRelationName(relation),
					astate.samplerows, astate.numrows)));

	elog(DEBUG5, "pxf_fdw: PxfAcquireSampleRowsFunc ends on segment: %d", PXF_SEGMENT_ID);
	return astate.numrows;
}

/*
 * Collect sample rows from the result of query.
 *	 - Use all tuples in sample until target # of samples are collected.
 *	 - Subsequently, replace already-sampled tuples randomly.
 */
static void
AnalyzeRowProcessor(TupleTableSlot *slot, PxfFdwAnalyzeState *astate)
{
	int			targrows = astate->targrows;
	int			pos;			/* array index to store tuple in */
	MemoryContext oldcontext;

	/* Always increment sample row counter. */
	astate->samplerows += 1;

	/*
	 * Determine the slot where this sample row should be stored.  Set pos to
	 * negative value to indicate the row should be skipped.
	 */
	if (astate->numrows < targrows)
	{
		/* First targrows rows are always included into the sample */
		pos = astate->numrows++;
	}
	else
	{
		/*
		 * Now we start replacing tuples in the sample until we reach the end
		 * of the relation.  Same algorithm as in acquire_sample_rows in
		 * analyze.c; see Jeff Vitter's paper.
		 */
		if (astate->rowstoskip < 0)
			astate->rowstoskip = anl_get_next_S(astate->samplerows, targrows,
												&astate->rstate);

		if (astate->rowstoskip <= 0)
		{
			/* Choose a random reservoir element to replace. */
			pos = (int) (targrows * anl_random_fract());
			Assert(pos >= 0 && pos < targrows);
			heap_freetuple(astate->rows[pos]);
		}
		else
		{
			/* Skip this tuple. */
			pos = -1;
		}

		astate->rowstoskip -= 1;
	}

	if (pos >= 0)
	{
		/*
		 * Create sample tuple from current result row, and store it in the
		 * position determined above.  The tuple has to be created in anl_cxt.
		 */
		oldcontext = MemoryContextSwitchTo(astate->anl_cxt);

		astate->rows[pos] = ExecCopySlotHeapTuple(slot);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Initiates a copy state for pxfBeginForeignScan() and pxfReScanForeignScan()
 */
static void
InitCopyState(PxfFdwScanState *pxfsstate)
{
	CopyState	cstate;

	PxfControllerImportStart(pxfsstate);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(pxfsstate->relation,
						   NULL,
						   false,	/* is_program */
						   &PxfControllerRead,	/* data_source_cb */
						   pxfsstate,	/* data_source_cb_extra */
						   NIL, /* attnamelist */
						   pxfsstate->options->copy_options,	/* copy options */
						   NIL);	/* ao_segnos */


	if (pxfsstate->options->reject_limit == -1)
	{
		/* Default error handling - "all-or-nothing" */
		cstate->cdbsreh = NULL; /* no SREH */
		cstate->errMode = ALL_OR_NOTHING;
	}
	else
	{
		/* no error log by default */
		cstate->errMode = SREH_IGNORE;

		/* select the SREH mode */
		if (pxfsstate->options->log_errors)
			cstate->errMode = SREH_LOG; /* errors into file */

		cstate->cdbsreh = makeCdbSreh(pxfsstate->options->reject_limit,
									  pxfsstate->options->is_reject_limit_rows,
									  pxfsstate->options->resource,
									  (char *) cstate->cur_relname,
									  pxfsstate->options->log_errors);

		cstate->cdbsreh->relid = RelationGetRelid(pxfsstate->relation);
	}

	/* and 'fe_mgbuf' */
	cstate->fe_msgbuf = makeStringInfo();

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input or output routines, and should be faster than retail
	 * pfree's anyway.
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "PxfFdwMemCxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	pxfsstate->cstate = cstate;
}

/*
 * Initiates a copy state for pxfBeginForeignModify()
 */
static void
InitCopyStateForModify(PxfFdwModifyState *pxfmstate)
{
	List	   *copy_options;
	CopyState	cstate;

	copy_options = pxfmstate->options->copy_options;

	PxfControllerExportStart(pxfmstate);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyTo(pxfmstate->relation, copy_options);

	/* Initialize 'out_functions', like CopyTo() would. */

	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	Form_pg_attribute *attr = tupDesc->attrs;
	int			num_phys_attrs = tupDesc->natts;

	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	ListCell   *cur;

	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;
		bool		isvarlena;

		getTypeOutputInfo(attr[attnum - 1]->atttypid,
						  &out_func_oid,
						  &isvarlena);
		fmgr_info(out_func_oid, &cstate->out_functions[attnum - 1]);
	}

	/* and 'fe_mgbuf' */
	cstate->fe_msgbuf = makeStringInfo();

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input or output routines, and should be faster than retail
	 * pfree's anyway.
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "PxfFdwMemCxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	pxfmstate->cstate = cstate;
}

/*
 * Set up CopyState for writing to an foreign table.
 */
static CopyState
BeginCopyTo(Relation forrel, List *options)
{
	CopyState	cstate;

	Assert(RelationIsForeign(forrel));

#if (PG_VERSION_NUM <= 90500)
	cstate = BeginCopy(false, forrel, NULL, NULL, NIL, options, NULL);
#else
	cstate = BeginCopy(false, forrel, NULL, NULL, forrel->rd_id, NIL, options, NULL);
#endif
	cstate->dispatch_mode = COPY_DIRECT;

	/*
	 * We use COPY_CALLBACK to mean that the each line should be left in
	 * fe_msgbuf. There is no actual callback!
	 */
	cstate->copy_dest = COPY_CALLBACK;

	/*
	 * Some more initialization, that in the normal COPY TO codepath, is done
	 * in CopyTo() itself.
	 */
	cstate->null_print_client = cstate->null_print; /* default */
	if (cstate->need_transcoding)
		cstate->null_print_client = pg_server_to_custom(cstate->null_print,
														cstate->null_print_len,
														cstate->file_encoding,
														cstate->enc_conversion_proc);

	return cstate;
}

static void
PxfAbortCallback(ResourceReleasePhase phase,
				 bool isCommit,
				 bool isTopLevel,
				 void *arg)
{
	if (isCommit || phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	pxfEndForeignScan(arg);
}