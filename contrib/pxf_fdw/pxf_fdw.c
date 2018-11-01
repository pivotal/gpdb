/*
 * pxf_fdw.c
 *		  Foreign-data wrapper for the Pivotal Extension Framework (PXF)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_fdw.c
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "access/reloptions.h"
#include "nodes/pg_list.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "optimizer/restrictinfo.h"

#include "src/pxfbridge.h"
#include "src/pxffragment.h"
#include "src/pxfuriparser.h"

PG_MODULE_MAGIC;

static char *const SERVER_OPTION_PROTOCOL = "protocol";
static char *const SERVER_OPTION_LOCATION = "location";

/*
 * FDW-specific information for RelOptInfo.fdw_private.
 */
typedef struct PxfPlanState
{
	char        *protocol; /* Storage type such as S3, ADL, GS, HDFS, HBase */
	char        *location; /* data location */
	List        *options;  /* merged COPY options, excluding filename */
	BlockNumber pages;     /* estimate of file's physical size */
	double      ntuples;   /* estimate of number of rows in file */

} PxfPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct PxfFdwExecutionState
{
	List      *options; /* merged COPY options, excluding protocol and location */
	CopyState cstate;   /* state of reading file */
} PxfFdwExecutionState;

extern Datum
pxf_fdw_handler(PG_FUNCTION_ARGS);

extern Datum
pxf_fdw_validator(PG_FUNCTION_ARGS);

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(pxf_fdw_handler);
PG_FUNCTION_INFO_V1(pxf_fdw_validator);

/*
 * FDW functions declarations
 */

static void
pxfGetForeignRelSize(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid);
static void
pxfGetForeignPaths(PlannerInfo *root,
                   RelOptInfo *baserel,
                   Oid foreigntableid);
#if (PG_VERSION_NUM <= 90500)
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
                  RelOptInfo *baserel,
                  Oid foreigntableid,
                  ForeignPath *best_path,
                  List *tlist,
                  List *scan_clauses);

#else
static ForeignScan *pxfGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses,
						Plan *outer_plan);
#endif
static void
pxfBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *
pxfIterateForeignScan(ForeignScanState *node);
static void
pxfReScanForeignScan(ForeignScanState *node);
static void
pxfEndForeignScan(ForeignScanState *node);

/*
 * FDW callback routines
 */
static void
pxfAddForeignUpdateTargets(Query *parsetree,
                           RangeTblEntry *target_rte,
                           Relation target_relation);
static List *
pxfPlanForeignModify(PlannerInfo *root,
                     ModifyTable *plan,
                     Index resultRelation,
                     int subplan_index);
static void
pxfBeginForeignModify(ModifyTableState *mtstate,
                      ResultRelInfo *resultRelInfo,
                      List *fdw_private,
                      int subplan_index,
                      int eflags);
static TupleTableSlot *
pxfExecForeignInsert(EState *estate,
                     ResultRelInfo *resultRelInfo,
                     TupleTableSlot *slot,
                     TupleTableSlot *planSlot);
static TupleTableSlot *
pxfExecForeignUpdate(EState *estate,
                     ResultRelInfo *resultRelInfo,
                     TupleTableSlot *slot,
                     TupleTableSlot *planSlot);
static TupleTableSlot *
pxfExecForeignDelete(EState *estate,
                     ResultRelInfo *resultRelInfo,
                     TupleTableSlot *slot,
                     TupleTableSlot *planSlot);
static void
pxfEndForeignModify(EState *estate,
                    ResultRelInfo *resultRelInfo);
static int
pxfIsForeignRelUpdatable(Relation rel);
static void
pxfExplainForeignScan(ForeignScanState *node,
                      ExplainState *es);
static void
pxfExplainForeignModify(ModifyTableState *mtstate,
                        ResultRelInfo *rinfo,
                        List *fdw_private,
                        int subplan_index,
                        ExplainState *es);
static bool
pxfAnalyzeForeignTable(Relation relation,
                       AcquireSampleRowsFunc *func,
                       BlockNumber *totalpages);
static int
pxfAcquireSampleRowsFunc(Relation relation, int elevel,
                         HeapTuple *rows, int targrows,
                         double *totalrows,
                         double *totaldeadrows);

/* magic */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
		FdwScanPrivateSelectSql,
	/* Integer list of attribute numbers retrieved by the SELECT */
		FdwScanPrivateRetrievedAttrs
};
/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 *    (NIL for a DELETE)
 * 3) Boolean flag showing if there's a RETURNING clause
 * 4) Integer list of attribute numbers retrieved by RETURNING, if any
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
		FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
		FdwModifyPrivateTargetAttnums,
	/* has-returning flag (as an integer Value node) */
		FdwModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING */
		FdwModifyPrivateRetrievedAttrs
};

/*
 * Helper functions
 */
static void
pxfGetOptions(Oid foreigntableid,
              char **protocol,
              char **location,
              List **extra_options);

static int
pxf_callback(void *outbuf, int datasize, void *extra);

/*
 * Foreign-data wrapper handler functions:
 * returns a struct with pointers to the
 * pxf_fdw callback routines.
 */
Datum
pxf_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdw_routine = makeNode(FdwRoutine);

	fdw_routine->GetForeignRelSize    = pxfGetForeignRelSize; // master-only
	fdw_routine->GetForeignPaths      = pxfGetForeignPaths; // master-only
	fdw_routine->GetForeignPlan       = pxfGetForeignPlan; // master-only
	fdw_routine->ExplainForeignScan   = pxfExplainForeignScan; // master-only
	fdw_routine->ExplainForeignModify = pxfExplainForeignModify; // master-only

	fdw_routine->BeginForeignScan   = pxfBeginForeignScan; // segment-only
	fdw_routine->IterateForeignScan = pxfIterateForeignScan; // segment-only
	fdw_routine->ReScanForeignScan  = pxfReScanForeignScan; // segment-only
	fdw_routine->EndForeignScan     = pxfEndForeignScan; // segment-only

	/* insert support */
	fdw_routine->AddForeignUpdateTargets = pxfAddForeignUpdateTargets;

	fdw_routine->PlanForeignModify     = pxfPlanForeignModify;
	fdw_routine->BeginForeignModify    = pxfBeginForeignModify;
	fdw_routine->ExecForeignInsert     = pxfExecForeignInsert;
	fdw_routine->ExecForeignUpdate     = pxfExecForeignUpdate;
	fdw_routine->ExecForeignDelete     = pxfExecForeignDelete;
	fdw_routine->EndForeignModify      = pxfEndForeignModify;
	fdw_routine->IsForeignRelUpdatable = pxfIsForeignRelUpdatable;

	fdw_routine->AnalyzeForeignTable = pxfAnalyzeForeignTable; // master-only

	PG_RETURN_POINTER(fdw_routine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses pxf_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
pxf_fdw_validator(PG_FUNCTION_ARGS)
{
//	List     *options_list  = untransformRelOptions(PG_GETARG_DATUM(0));
//	Oid      catalog        = PG_GETARG_OID(1);
//	char     *protocol      = NULL;
//	List     *other_options = NIL;
//	ListCell *cell;
//
//	/*
//	 * Check that only options supported by pxf_fdw, and allowed for the
//	 * current object type, are given.
//	 */
//	foreach(cell, options_list)
//	{
//		DefElem *def = (DefElem *) lfirst(cell);
//
//		/*
//		 * Separate out protocol and column-specific options
//		 */
//		if (strcmp(def->defname, SERVER_OPTION_PROTOCOL) == 0)
//		{
//			if (protocol)
//				ereport(ERROR,
//				        (errcode(ERRCODE_SYNTAX_ERROR),
//					        errmsg(
//						        "conflicting or redundant options. Protocol option should only be defined once")));
//			protocol = defGetString(def);
//		}
//		else
//			other_options = lappend(other_options, def);
//	}
//
//	/*
//	 * protocol option is required for pxf_fdw foreign tables.
//	 */
//	if (catalog == ForeignTableRelationId && protocol == NULL)
//		ereport(ERROR,
//		        (errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
//			        errmsg("protocol is required for pxf_fdw foreign tables")));

	PG_RETURN_VOID();
}

/*
 * GetForeignRelSize
 *		set relation size estimates for a foreign table
 */
static void
pxfGetForeignRelSize(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid)
{
	elog(DEBUG5, "PXF_FWD: pxfGetForeignRelSize");

	PxfPlanState *fdw_private;

	/*
	 * Fetch options.  We only need protocol at this point, but we might as
	 * well get everything and not need to re-fetch it later in planning.
	 */
	fdw_private = (PxfPlanState *) palloc(sizeof(PxfPlanState));
	pxfGetOptions(foreigntableid,
	              &fdw_private->protocol,
	              &fdw_private->location,
	              &fdw_private->options);

	elog(DEBUG2,
	     "PXF_FWD: Protocol %s, Location %s",
	     fdw_private->protocol,
	     fdw_private->location);

	baserel->fdw_private = (void *) fdw_private;
	// FIXME: populate with an estimate of the number of rows in the foreign table
	baserel->rows        = 0;
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
	Path *path;
#if (PG_VERSION_NUM < 90500)
	path = (Path *) create_foreignscan_path(root, baserel,
	                                        baserel->rows,
	                                        10,
	                                        0,
	                                        NIL,
	                                        NULL,
	                                        baserel->fdw_private);
#else
	path = (Path *) create_foreignscan_path(root, baserel,
#if PG_VERSION_NUM >= 90600
						NULL,
#endif
						baserel->rows,
						10,
						0,
						NIL,
						NULL,
						NULL,
						NIL);
#endif
	add_path(baserel, path);
}

/*
 * GetForeignPlan
 *	create a ForeignScan plan node 
 */
#if (PG_VERSION_NUM <= 90500)
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
                  RelOptInfo *baserel,
                  Oid foreigntableid,
                  ForeignPath *best_path,
                  List *tlist,
                  List *scan_clauses)
{
	Index scan_relid = baserel->relid;

	scan_clauses = extract_actual_clauses(scan_clauses, false);
	return make_foreignscan(tlist,
	                        scan_clauses,
	                        scan_relid,
	                        scan_clauses,
	                        baserel->fdw_private);
}
#else
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses,
						Plan *outer_plan)
{
	// FIXME: When 90500 is merged we need to implement this
	Index		scan_relid = baserel->relid;
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	return make_foreignscan(tlist,
			scan_clauses,
			scan_relid,
			scan_clauses,
			NIL,
			NIL,
			NIL,
			outer_plan);
}
#endif
/*
 * ExplainForeignScan
 *   no extra info explain plan
 */
/*
static void
pxfExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
}

*/
/*
 * BeginForeignScan
 *   called during executor startup. perform any initialization 
 *   needed, but not start the actual scan. 
 */

static void
pxfBeginForeignScan(ForeignScanState *node, int eflags)
{
	elog(DEBUG2, "PXF_FWD: pxfBeginForeignScan");

	Relation             relation     = node->ss.ss_currentRelation;
	ForeignScan          *foreignScan = (ForeignScan *) node->ss.ps.plan;
	PxfPlanState         *fdw_private =
		                     (PxfPlanState *) foreignScan->fdw_private;
	CopyState            cstate;
	PxfFdwExecutionState *pxfestate;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Parse the location option to a GPHDUri */
	GPHDUri *uri = parseGPHDUri(fdw_private->location);
	elog(DEBUG2,
	     "PXF_FWD: pxfBeginForeignScan with URI: %s, Profile: %s",
	     uri->uri,
	     uri->profile);

	get_fragments(uri, relation, NULL);

	/* set context */
	gphadoop_context *context = palloc0(sizeof(gphadoop_context));

	context->gphd_uri = uri;
	initStringInfo(&context->uri);
	initStringInfo(&context->write_file_name);
	context->relation  = relation;
	context->filterstr = NULL;

	gpbridge_import_start(context);
	elog(DEBUG2, "PXF_FWD: pxfBeginForeignScan is completed");

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(node->ss.ss_currentRelation,
	                       NULL,
	                       false, /* is_program */
	                       &pxf_callback,  /* data_source_cb */
	                       context,  /* data_source_cb_extra */
	                       NIL,   /* attnamelist */
	                       fdw_private->options,
	                       NIL);  /* ao_segnos */
	cstate->delim = ",";

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	pxfestate = (PxfFdwExecutionState *) palloc(sizeof(PxfFdwExecutionState));
	pxfestate->options = fdw_private->options;
	pxfestate->cstate  = cstate;

	node->fdw_state = (void *) pxfestate;
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
	elog(DEBUG5, "PXF_FWD: pxfIterateForeignScan");

	PxfFdwExecutionState *pxfestate = (PxfFdwExecutionState *) node->fdw_state;
	TupleTableSlot       *slot      = node->ss.ss_ScanTupleSlot;
	bool                 found;

	ErrorContextCallback errcallback;

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg      = (void *) pxfestate->cstate;
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
	found = NextCopyFrom(pxfestate->cstate, NULL,
	                     slot_get_values(slot), slot_get_isnull(slot),
	                     NULL);
	if (found)
		ExecStoreVirtualTuple(slot);

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
}

/*
 *EndForeignScan
 *	End the scan and release resources. 
 */
static void
pxfEndForeignScan(ForeignScanState *node)
{
}

/*
 * postgresAddForeignUpdateTargets
 *    Add resjunk column(s) needed for update/delete on a foreign table
 */
static void
pxfAddForeignUpdateTargets(Query *parsetree,
                           RangeTblEntry *target_rte,
                           Relation target_relation)
{
	Var         *var;
	const char  *attrname;
	TargetEntry *tle;

/*
 * In postgres_fdw, what we need is the ctid, same as for a regular table.
 */

	/* Make a Var representing the desired value */
	var = makeVar(parsetree->resultRelation,
	              SelfItemPointerAttributeNumber,
	              TIDOID,
	              -1,
	              InvalidOid,
	              0);

	/* Wrap it in a resjunk TLE with the right name ... */
	attrname = "ctid";

	tle = makeTargetEntry((Expr *) var,
	                      list_length(parsetree->targetList) + 1,
	                      pstrdup(attrname),
	                      true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * pxfPlanForeignModify
 *		Plan an insert/update/delete operation on a foreign table
 *
 * Note: currently, the plan tree generated for UPDATE/DELETE will always
 * include a ForeignScan that retrieves ctids (using SELECT FOR UPDATE)
 * and then the ModifyTable node will have to execute individual remote
 * UPDATE/DELETE commands.  If there are no local conditions or joins
 * needed, it'd be better to let the scan node do UPDATE/DELETE RETURNING
 * and then do nothing at ModifyTable.  Room for future optimization ...
 */
static List *
pxfPlanForeignModify(PlannerInfo *root,
                     ModifyTable *plan,
                     Index resultRelation,
                     int subplan_index)
{
/*
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
*/
	List *targetAttrs     = NIL;
	List *returningList   = NIL;
	List *retrieved_attrs = NIL;

	StringInfoData sql;
	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
//	rel = heap_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, we transmit only columns that were explicitly
	 * targets of the UPDATE, so as to avoid unnecessary data transmission.
	 * (We can't do that for INSERT since we would miss sending default values
	 * for columns not listed in the source statement.)
	 */
/*
	if (operation == CMD_INSERT)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		Bitmapset  *tmpset = bms_copy(rte->modifiedCols);
		AttrNumber	col;

		while ((col = bms_first_member(tmpset)) >= 0)
		{
			col += FirstLowInvalidHeapAttributeNumber;
			if (col <= InvalidAttrNumber)		// shouldn't happen 
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, col);
		}
	}
*/

	/*
	 * Extract the relevant RETURNING list if any.
	 */
/*
	if (plan->returningLists)
		returningList = (List *) list_nth(plan->returningLists, subplan_index);
*/

	/*
	 * Construct the SQL command string.
	 */
/*
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, root, resultRelation, rel,
							 targetAttrs, returningList,
							 &retrieved_attrs);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, root, resultRelation, rel,
							 targetAttrs, returningList,
							 &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, root, resultRelation, rel,
							 returningList,
							 &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	heap_close(rel, NoLock);
*/

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
	return list_make4(makeString(sql.data),
	                  targetAttrs,
	                  makeInteger((returningList != NIL)),
	                  retrieved_attrs);
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
	return;
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
	return NULL;
}

/*
 * pxfExecForeignUpdate
 *		Update one row in a foreign table
 */
static TupleTableSlot *
pxfExecForeignUpdate(EState *estate,
                     ResultRelInfo *resultRelInfo,
                     TupleTableSlot *slot,
                     TupleTableSlot *planSlot)
{
	return NULL;
}

/*
 * pxfExecForeignDelete
 *		Delete one row from a foreign table
 */
static TupleTableSlot *
pxfExecForeignDelete(EState *estate,
                     ResultRelInfo *resultRelInfo,
                     TupleTableSlot *slot,
                     TupleTableSlot *planSlot)
{
	return NULL;
}

/*
 * pxfEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
pxfEndForeignModify(EState *estate,
                    ResultRelInfo *resultRelInfo)
{
	return;
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
	/* updatable is INSERT, UPDATE and DELETE.
	 */
	return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
}

/*
 * pxfExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
pxfExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
/*
	List	   *fdw_private;
	char	   *sql;

	if (es->verbose)
	{
		fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
		ExplainPropertyText("Dummy SQL", sql, es);
	}
*/

}

/*
 * pxfExplainForeignModify
 *		Produce extra output for EXPLAIN of a ModifyTable on a foreign table
 */
static void
pxfExplainForeignModify(ModifyTableState *mtstate,
                        ResultRelInfo *rinfo,
                        List *fdw_private,
                        int subplan_index,
                        ExplainState *es)
{
	if (es->verbose)
	{
		char *sql = strVal(list_nth(fdw_private,
		                            FdwModifyPrivateUpdateSql));

		ExplainPropertyText("PXF Query", sql, es);
	}
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
	*func = pxfAcquireSampleRowsFunc;
	return false;
}

/*
 * Acquire a random sample of rows
 */
static int
pxfAcquireSampleRowsFunc(Relation relation, int elevel,
                         HeapTuple *rows, int targrows,
                         double *totalrows,
                         double *totaldeadrows)
{

	totalrows     = 0;
	totaldeadrows = 0;
	return 0;
}

/*
 * Fetch the options for a pxf_fdw foreign table.
 *
 * We need to extract out the "protocol"
 * from other options to be able to determine
 * the correct accessor that we want to access.
 */
static void
pxfGetOptions(Oid foreigntableid,
              char **protocol,
              char **location,
              List **extra_options)
{
	UserMapping        *user;
	ForeignTable       *table;
	ForeignServer      *server;
	ForeignDataWrapper *wrapper;
	List               *options;
	ListCell           *lc,
	                   *prev;

	/*
	 * Extract options from FDW objects.  We ignore user mappings because
	 * file_fdw doesn't have any options that can be specified there.
	 *
	 * (XXX Actually, given the current contents of valid_options[], there's
	 * no point in examining anything except the foreign table's own options.
	 * Simplify?)
	 */
	table   = GetForeignTable(foreigntableid);
	server  = GetForeignServer(table->serverid);
	user    = GetUserMapping(GetUserId(), server->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);
	options = list_concat(options, user->options);
	// FIXME: look at the behaviour of get_file_fdw_attribute_options
	//        in file_fdw and determine if we need to do something
	//        for pxf_fwd
//	options =
//		list_concat(options, get_file_fdw_attribute_options(foreigntableid));

	/*
	 * Separate out the filename.
	 */
	*protocol = NULL;
	prev = NULL;
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		elog(DEBUG2, "PXF_FWD: pxfGetOptions Found option %s", def->defname);

		if (strcmp(def->defname, SERVER_OPTION_PROTOCOL) == 0)
		{
			*protocol = defGetString(def);
			options = list_delete_cell(options, lc, prev);
		}
		else if (strcmp(def->defname, SERVER_OPTION_LOCATION) == 0)
		{
			*location = defGetString(def);
			options = list_delete_cell(options, lc, prev);
		}
		else if (strcmp(def->defname, "fs.s3a.awsAccessKeyId") == 0 ||
			strcmp(def->defname, "fs.s3a.awsSecretAccessKey") == 0)
		{
			options = list_delete_cell(options, lc, prev);
		}
		else
		{
			prev = lc;
		}
	}

	/*
	 * The validator should have checked that a protocol was included in the
	 * options, but check again, just in case.
	 */
	if (*protocol == NULL)
		elog(ERROR, "protocol is required for pxf_fdw foreign tables");

	if (*location == NULL)
		elog(ERROR, "location is required for pxf_fdw foreign tables");

	*extra_options = options;
}

static int
pxf_callback(void *outbuf, int datasize, void *extra)
{
	return gpbridge_read(extra, outbuf, datasize);
}
