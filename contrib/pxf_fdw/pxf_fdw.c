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
#include "mb/pg_wchar.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "optimizer/restrictinfo.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"

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

static CopyState
BeginCopy(bool is_from, Relation rel, Node *raw_query,
          const char *queryString, List *attnamelist, List *options);

static CopyState
BeginCopyFromPxfExternal(Relation rel,
                         bool is_program,
                         copy_data_source_cb data_source_cb,
                         void *data_source_cb_extra,
                         List *attnamelist,
                         List *options,
                         List *ao_segnos);

static void
ProcessCopyOptionsInternal(CopyState cstate,
                           bool is_from,
                           List *options,
                           int num_columns,
                           bool is_copy); /* false means external table */

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
	cstate = BeginCopyFromPxfExternal(node->ss.ss_currentRelation,
	                                  false, /* is_program */
	                                  &pxf_callback,  /* data_source_cb */
	                                  context,  /* data_source_cb_extra */
	                                  NIL,   /* attnamelist */
	                                  fdw_private->options,
	                                  NIL);  /* ao_segnos */
	cstate->delim = ",";


//	/*
//	 * If this is the first call after Begin or ReScan, we need to create the
//	 * cursor on the remote side.
//	 */
//	if (!cstate->fe_eof && *cstate->raw_buf == NULL)
//	{
//		int bytes_read =
//			    gpbridge_read(fdw_private->context, cstate->raw_buf, 65536);
//
//	}

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

	PxfFdwExecutionState *pxfestate   =
		                     (PxfFdwExecutionState *) node->fdw_state;
	TupleTableSlot       *slot        = node->ss.ss_ScanTupleSlot;
	bool                 found;
	CopyState            cstate       = pxfestate->cstate;
	ForeignScan          *foreignScan = (ForeignScan *) node->ss.ps.plan;
	PxfPlanState         *fdw_private =
		                     (PxfPlanState *) foreignScan->fdw_private;

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

		ExplainPropertyText("Dummy SQL", sql, es);
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
		}
		prev = lc;
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

static CopyState
BeginCopyFromPxfExternal(Relation rel,
                         bool is_program,
                         copy_data_source_cb data_source_cb,
                         void *data_source_cb_extra,
                         List *attnamelist,
                         List *options,
                         List *ao_segnos)
{
	CopyState         cstate;
	TupleDesc         tupDesc;
	Form_pg_attribute *attr;
	AttrNumber        num_phys_attrs,
	                  num_defaults;
	FmgrInfo          *in_functions;
	Oid               *typioparams;
	int               attnum;
	Oid               in_func_oid;
	int               *defmap;
	ExprState         **defexprs;
	MemoryContext     oldcontext;
	bool              volatile_defexprs;

	cstate     = BeginCopy(true, rel, NULL, NULL, attnamelist, options);
	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/*
	 * Determine the mode
	 */
	if (Gp_role == GP_ROLE_DISPATCH && !cstate->on_segment)
		cstate->dispatch_mode = COPY_DISPATCH;
	else if (Gp_role == GP_ROLE_EXECUTE && !cstate->on_segment)
		cstate->dispatch_mode = COPY_EXECUTOR;
	else
		cstate->dispatch_mode = COPY_DIRECT;

	/* Initialize state variables */
	cstate->fe_eof      = false;
	// cstate->eol_type = EOL_UNKNOWN; /* GPDB: don't overwrite value set in ProcessCopyOptions */
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno  = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval  = NULL;

	/* Set up variables to avoid per-attribute overhead. */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->line_buf_converted = false;
	cstate->raw_buf            = (char *) palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index      = cstate->raw_buf_len = 0;

	tupDesc           = RelationGetDescr(cstate->rel);
	attr              = tupDesc->attrs;
	num_phys_attrs    = tupDesc->natts;
	num_defaults      = 0;
	volatile_defexprs = false;

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass to
	 * the input function), and info about defaults and constraints. (Which
	 * input function we use depends on text/binary format choice.)
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams  = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap       = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs     = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		/* We don't need info for dropped attributes */
		if (attr[attnum - 1]->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		if (cstate->binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
			                       &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
			                 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);

		/* TODO: is force quote array necessary for default conversion */

		// FIXME: we commented out this IF until we figure out if we need it
//		/* Get default info if needed */
//		if (!list_member_int(cstate->attnumlist, attnum))
//		{
//			/* attribute is NOT to be copied from input */
//			/* use default value if one exists */
//			Expr *defexpr = (Expr *) build_column_default(cstate->rel,
//			                                              attnum);
//
//			if (defexpr != NULL)
//			{
//				/* Run the expression through planner */
//				defexpr = expression_planner(defexpr);
//
//				/* Initialize executable expression in copycontext */
//				defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
//				defmap[num_defaults]   = attnum - 1;
//				num_defaults++;
//
//				/*
//				 * If a default expression looks at the table being loaded,
//				 * then it could give the wrong answer when using
//				 * multi-insert. Since database access can be dynamic this is
//				 * hard to test for exactly, so we use the much wider test of
//				 * whether the default expression is volatile. We allow for
//				 * the special case of when the default expression is the
//				 * nextval() of a sequence which in this specific case is
//				 * known to be safe for use with the multi-insert
//				 * optimisation. Hence we use this special case function
//				 * checker rather than the standard check for
//				 * contain_volatile_functions().
//				 */
//				if (!volatile_defexprs)
//					volatile_defexprs =
//						contain_volatile_functions_not_nextval((Node *) defexpr);
//			}
//		}
	}

	/* We keep those variables in cstate. */
	cstate->in_functions      = in_functions;
	cstate->typioparams       = typioparams;
	cstate->defmap            = defmap;
	cstate->defexprs          = defexprs;
	cstate->volatile_defexprs = volatile_defexprs;
	cstate->num_defaults      = num_defaults;
	cstate->is_program        = is_program;

//	bool		pipe = (filename == NULL || cstate->dispatch_mode == COPY_EXECUTOR);

	if (cstate->on_segment && Gp_role == GP_ROLE_DISPATCH)
	{
//		/* open nothing */
//
//		if (filename == NULL)
//			ereport(ERROR,
//			        (errcode(ERRCODE_SYNTAX_ERROR),
//				        errmsg("STDIN is not supported by 'COPY ON SEGMENT'")));
	}
	else if (data_source_cb)
	{
		cstate->copy_dest            = COPY_CALLBACK;
		cstate->data_source_cb       = data_source_cb;
		cstate->data_source_cb_extra = data_source_cb_extra;
	}
//	else if (pipe)
//	{
////		Assert(!is_program || cstate->dispatch_mode == COPY_EXECUTOR);	/* the grammar does not allow this */
////		if (whereToSendOutput == DestRemote)
////			ReceiveCopyBegin(cstate);
////		else
////			cstate->copy_file = stdin;
//	}
	else
	{
//		cstate->filename = pstrdup(filename);

//		if (cstate->on_segment)
//			MangleCopyFileName(cstate);

//		if (cstate->is_program)
//		{
//			cstate->program_pipes = open_program_pipes(cstate->filename, false);
//			cstate->copy_file = fdopen(cstate->program_pipes->pipes[0], PG_BINARY_R);
//			if (cstate->copy_file == NULL)
//				ereport(ERROR,
//				        (errmsg("could not execute command \"%s\": %m",
//				                cstate->filename)));
//		}
//		else
//		{
//			struct stat st;
//			char	   *filename = cstate->filename;
//
//			cstate->copy_file = AllocateFile(filename, PG_BINARY_R);
//			if (cstate->copy_file == NULL)
//				ereport(ERROR,
//				        (errcode_for_file_access(),
//					        errmsg("could not open file \"%s\" for reading: %m",
//					               filename)));
//
//			// Increase buffer size to improve performance  (cmcdevitt)
//			setvbuf(cstate->copy_file, NULL, _IOFBF, 393216); // 384 Kbytes
//
//			fstat(fileno(cstate->copy_file), &st);
//			if (S_ISDIR(st.st_mode))
//				ereport(ERROR,
//				        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
//					        errmsg("\"%s\" is a directory", filename)));
//		}
	}

	/*
	 * Append Only Tables.
	 *
	 * If QD, build a list of all the relations (relids) that may get data
	 * inserted into them as a part of this operation. This includes
	 * the relation specified in the COPY command, plus any partitions
	 * that it may have. Then, call assignPerRelSegno to assign a segfile
	 * number to insert into each of the Append Only relations that exists
	 * in this global list. We generate the list now and save it in cstate.
	 *
	 * If QE - get the QD generated list from CopyStmt and each relation can
	 * find it's assigned segno by looking at it (during CopyFrom).
	 *
	 * Utility mode always builds a one single mapping.
	 */
//	bool		shouldDispatch = (Gp_role == GP_ROLE_DISPATCH &&
//		rel->rd_cdbpolicy != NULL);
//	if (shouldDispatch)
//	{
//		Oid			relid = RelationGetRelid(cstate->rel);
//		List	   *all_relids = NIL;
//
//		all_relids = lappend_oid(all_relids, relid);
//
//		if (rel_is_partitioned(relid))
//		{
//			if (cstate->on_segment && gp_enable_segment_copy_checking && !partition_policies_equal(cstate->rel->rd_cdbpolicy, RelationBuildPartitionDesc(cstate->rel, false)))
//			{
//				ereport(ERROR,
//				        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
//					        errmsg("COPY FROM ON SEGMENT doesn't support checking distribution key restriction when the distribution policy of the partition table is different from the main table"),
//					        errhint("\"SET gp_enable_segment_copy_checking=off\" can be used to disable distribution key checking.")));
//			}
//			PartitionNode *pn = RelationBuildPartitionDesc(cstate->rel, false);
//			all_relids = list_concat(all_relids, all_partition_relids(pn));
//		}
//
//		cstate->ao_segnos = assignPerRelSegno(all_relids);
//	}
//	else
//	{
//		if (ao_segnos)
//		{
//			/* We must be a QE if we received the aosegnos config */
//			Assert(Gp_role == GP_ROLE_EXECUTE);
//			cstate->ao_segnos = ao_segnos;
//		}
//		else
//		{
//			/*
//			 * utility mode (or dispatch mode for no policy table).
//			 * create a one entry map for our one and only relation
//			 */
//			if (RelationIsAoRows(cstate->rel) || RelationIsAoCols(cstate->rel))
//			{
//				SegfileMapNode *n = makeNode(SegfileMapNode);
//				n->relid = RelationGetRelid(cstate->rel);
//				n->segno = SetSegnoForWrite(cstate->rel, InvalidFileSegNumber);
//				cstate->ao_segnos = lappend(cstate->ao_segnos, n);
//			}
//		}
//	}

	if (cstate->on_segment && Gp_role == GP_ROLE_DISPATCH)
	{
		/* nothing to do */
	}
	else if (cstate->dispatch_mode == COPY_EXECUTOR &&
		cstate->copy_dest != COPY_CALLBACK)
	{
//		/* Read special header from QD */
//		static const size_t sigsize = sizeof(QDtoQESignature);
//		char		readSig[sigsize];
//		copy_from_dispatch_header header_frame;
//
//		if (CopyGetData(cstate, &readSig, sigsize) != sigsize ||
//			memcmp(readSig, QDtoQESignature, sigsize) != 0)
//			ereport(ERROR,
//			        (errcode(ERRCODE_INTERNAL_ERROR),
//				        errmsg("QD->QE COPY communication signature not recognized")));
//
//		if (CopyGetData(cstate, &header_frame, sizeof(header_frame)) != sizeof(header_frame))
//			ereport(ERROR,
//			        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//				        errmsg("invalid QD->QD COPY communication header")));
//
//		cstate->file_has_oids = header_frame.file_has_oids;
	}
	else if (!cstate->binary)
	{
		/* must rely on user to tell us... */
		cstate->file_has_oids = cstate->oids;
	}
	else
	{
//		/* Read and verify binary header */
//		char		readSig[11];
//		int32		tmp;
//
//		/* Signature */
//		if (CopyGetData(cstate, readSig, 11) != 11 ||
//			memcmp(readSig, BinarySignature, 11) != 0)
//			ereport(ERROR,
//			        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//				        errmsg("COPY file signature not recognized")));
//		/* Flags field */
//		if (!CopyGetInt32(cstate, &tmp))
//			ereport(ERROR,
//			        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//				        errmsg("invalid COPY file header (missing flags)")));
//		cstate->file_has_oids = (tmp & (1 << 16)) != 0;
//		tmp &= ~(1 << 16);
//		if ((tmp >> 16) != 0)
//			ereport(ERROR,
//			        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//				        errmsg("unrecognized critical flags in COPY file header")));
//		/* Header extension length */
//		if (!CopyGetInt32(cstate, &tmp) ||
//			tmp < 0)
//			ereport(ERROR,
//			        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//				        errmsg("invalid COPY file header (missing length)")));
//		/* Skip extension header, if present */
//		while (tmp-- > 0)
//		{
//			if (CopyGetData(cstate, readSig, 1) != 1)
//				ereport(ERROR,
//				        (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
//					        errmsg("invalid COPY file header (wrong length)")));
//		}
	}

//	if (cstate->file_has_oids && cstate->binary)
//	{
//		getTypeBinaryInputInfo(OIDOID,
//		                       &in_func_oid, &cstate->oid_typioparam);
//		fmgr_info(in_func_oid, &cstate->oid_in_function);
//	}

	/* create workspace for CopyReadAttributes results */
	if (!cstate->binary)
	{
		AttrNumber attr_count = list_length(cstate->attnumlist);
		int        nfields    =
			           cstate->file_has_oids ? (attr_count + 1) : attr_count;

		cstate->max_fields = nfields;
		cstate->raw_fields = (char **) palloc(nfields * sizeof(char *));
	}

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * Common setup routines used by BeginCopyFrom and BeginCopyTo.
 *
 * Iff <binary>, unload or reload in the binary format, as opposed to the
 * more wasteful but more robust and portable text format.
 *
 * Iff <oids>, unload or reload the format that includes OID information.
 * On input, we accept OIDs whether or not the table has an OID column,
 * but silently drop them if it does not.  On output, we report an error
 * if the user asks for OIDs in a table that has none (not providing an
 * OID column might seem friendlier, but could seriously confuse programs).
 *
 * If in the text format, delimit columns with delimiter <delim> and print
 * NULL values as <null_print>.
 */
static CopyState
BeginCopy(bool is_from,
          Relation rel,
          Node *raw_query,
          const char *queryString,
          List *attnamelist,
          List *options)
{
	CopyState     cstate;
	TupleDesc     tupDesc = NULL;
	int           num_phys_attrs;
	MemoryContext oldcontext;

	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/*
	 * We allocate everything used by a cstate in a new memory context. This
	 * avoids memory leaks during repeated use of COPY in a query.
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
	                                            "COPY",
	                                            ALLOCSET_DEFAULT_MINSIZE,
	                                            ALLOCSET_DEFAULT_INITSIZE,
	                                            ALLOCSET_DEFAULT_MAXSIZE);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* Extract options from the statement node tree */
	ProcessCopyOptionsInternal(cstate, is_from, options,
	                           0, /* pass correct value when COPY supports no delim */
	                           true);

	/* Process the source/target relation or query */
	if (rel)
	{
		Assert(!raw_query);

		cstate->rel = rel;

		tupDesc = RelationGetDescr(cstate->rel);

		/* Don't allow COPY w/ OIDs to or from a table without them */
		if (cstate->oids && !cstate->rel->rd_rel->relhasoids)
			ereport(ERROR,
			        (errcode(ERRCODE_UNDEFINED_COLUMN),
				        errmsg("table \"%s\" does not have OIDs",
				               RelationGetRelationName(cstate->rel))));
	}
	else
	{
//		List	   *rewritten;
//		Query	   *query;
//		PlannedStmt *plan;
//		DestReceiver *dest;
//
//		Assert(!is_from);
//		cstate->rel = NULL;
//
//		/* Don't allow COPY w/ OIDs from a select */
//		if (cstate->oids)
//			ereport(ERROR,
//			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
//				        errmsg("COPY (SELECT) WITH OIDS is not supported")));
//
//		/*
//		 * Run parse analysis and rewrite.  Note this also acquires sufficient
//		 * locks on the source table(s).
//		 *
//		 * Because the parser and planner tend to scribble on their input, we
//		 * make a preliminary copy of the source querytree.  This prevents
//		 * problems in the case that the COPY is in a portal or plpgsql
//		 * function and is executed repeatedly.  (See also the same hack in
//		 * DECLARE CURSOR and PREPARE.)  XXX FIXME someday.
//		 */
//		rewritten = pg_analyze_and_rewrite((Node *) copyObject(raw_query),
//		                                   queryString, NULL, 0);
//
//		/* We don't expect more or less than one result query */
//		if (list_length(rewritten) != 1)
//			elog(ERROR, "unexpected rewrite result");
//
//		query = (Query *) linitial(rewritten);
//
//		/* Query mustn't use INTO, either */
//		if (query->utilityStmt != NULL &&
//			IsA(query->utilityStmt, CreateTableAsStmt))
//			ereport(ERROR,
//			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
//				        errmsg("COPY (SELECT INTO) is not supported")));
//
//		Assert(query->commandType == CMD_SELECT);
//		Assert(query->utilityStmt == NULL);
//
//		/* plan the query */
//		plan = planner(query, 0, NULL);
//
//		/*
//		 * Use a snapshot with an updated command ID to ensure this query sees
//		 * results of any previously executed queries.
//		 */
//		PushCopiedSnapshot(GetActiveSnapshot());
//		UpdateActiveSnapshotCommandId();
//
//		/* Create dest receiver for COPY OUT */
//		dest = CreateDestReceiver(DestCopyOut);
//		((DR_copy *) dest)->cstate = cstate;
//
//		/* Create a QueryDesc requesting no output */
//		cstate->queryDesc = CreateQueryDesc(plan, queryString,
//		                                    GetActiveSnapshot(),
//		                                    InvalidSnapshot,
//		                                    dest, NULL,
//		                                    GP_INSTRUMENT_OPTS);
//
//		if (gp_enable_gpperfmon && Gp_role == GP_ROLE_DISPATCH)
//		{
//			Assert(queryString);
//			gpmon_qlog_query_submit(cstate->queryDesc->gpmon_pkt);
//			gpmon_qlog_query_text(cstate->queryDesc->gpmon_pkt,
//			                      queryString,
//			                      application_name,
//			                      GetResqueueName(GetResQueueId()),
//			                      GetResqueuePriority(GetResQueueId()));
//		}
//
//		/* GPDB hook for collecting query info */
//		if (query_info_collect_hook)
//			(*query_info_collect_hook)(METRICS_QUERY_SUBMIT, cstate->queryDesc);
//
//		/*
//		 * Call ExecutorStart to prepare the plan for execution.
//		 *
//		 * ExecutorStart computes a result tupdesc for us
//		 */
//		ExecutorStart(cstate->queryDesc, 0);
//
//		tupDesc = cstate->queryDesc->tupDesc;
	}

	cstate->attnamelist = attnamelist;
	/* Generate or convert list of attributes to process */
	cstate->attnumlist  = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* Convert FORCE QUOTE name list to per-column flags, check validity */
	cstate->force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->force_quote_all)
	{
		int i;

		for (i = 0; i < num_phys_attrs; i++)
			cstate->force_quote_flags[i] = true;
	}
	else if (cstate->force_quote)
	{
		List     *attnums;
		ListCell *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->force_quote);

		foreach(cur, attnums)
		{
			int attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
				        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					        errmsg(
						        "FORCE QUOTE column \"%s\" not referenced by COPY",
						        NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_quote_flags[attnum - 1] = true;
		}
	}

	/* Convert FORCE NOT NULL name list to per-column flags, check validity */
	cstate->force_notnull_flags =
		(bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->force_notnull)
	{
		List     *attnums;
		ListCell *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->force_notnull);

		foreach(cur, attnums)
		{
			int attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
				        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					        errmsg(
						        "FORCE NOT NULL column \"%s\" not referenced by COPY",
						        NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_notnull_flags[attnum - 1] = true;
		}
	}

	/* Convert FORCE NULL name list to per-column flags, check validity */
	cstate->force_null_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->force_null)
	{
		List     *attnums;
		ListCell *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->force_null);

		foreach(cur, attnums)
		{
			int attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
				        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					        errmsg(
						        "FORCE NULL column \"%s\" not referenced by COPY",
						        NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_null_flags[attnum - 1] = true;
		}
	}

	/* Convert convert_selectively name list to per-column flags */
	if (cstate->convert_selectively)
	{
		List     *attnums;
		ListCell *cur;

		cstate->convert_select_flags =
			(bool *) palloc0(num_phys_attrs * sizeof(bool));

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->convert_select);

		foreach(cur, attnums)
		{
			int attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
				        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					        errmsg_internal(
						        "selected column \"%s\" not referenced by COPY",
						        NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->convert_select_flags[attnum - 1] = true;
		}
	}

	/* Use client encoding when ENCODING option is not specified. */
	if (cstate->file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();

	/*
	 * Set up encoding conversion info.  Even if the file and server encodings
	 * are the same, we must apply pg_any_to_server() to validate data in
	 * multibyte encodings.
	 *
	 * In COPY_EXECUTE mode, the dispatcher has already done the conversion.
	 */
	if (cstate->dispatch_mode != COPY_DISPATCH)
	{
		cstate->need_transcoding =
			((cstate->file_encoding != GetDatabaseEncoding() ||
				pg_database_encoding_max_length() > 1));
		/* See Multibyte encoding comment above */
		cstate->encoding_embeds_ascii =
			PG_ENCODING_IS_CLIENT_ONLY(cstate->file_encoding);
		setEncodingConversionProc(cstate, cstate->file_encoding, !is_from);
	}
	else
	{
		cstate->need_transcoding = false;
		cstate->encoding_embeds_ascii =
			PG_ENCODING_IS_CLIENT_ONLY(cstate->file_encoding);
	}

	/*
	 * some greenplum db specific vars
	 */
	cstate->is_copy_in = (is_from ? true : false);
	if (is_from)
	{
		cstate->error_on_executor = false;
		initStringInfo(&(cstate->executor_err_context));
	}

	cstate->copy_dest = COPY_CALLBACK;

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

static void
ProcessCopyOptionsInternal(CopyState cstate,
                           bool is_from,
                           List *options,
                           int num_columns,
                           bool is_copy) /* false means external table */
{
	bool     format_specified = false;
	ListCell *option;

	/* Support external use for option sanity checking */
	if (cstate == NULL)
		cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	cstate->escape_off    = false;
	cstate->file_encoding = -1;

	bool delim_off = false;

	if (cstate->delim && pg_strcasecmp(cstate->delim, "off") == 0)
		delim_off = true;

	/*
	 * Check for incompatible options (must do these two before inserting
	 * defaults)
	 */
	if (cstate->binary && cstate->delim)
		ereport(ERROR,
		        (errcode(ERRCODE_SYNTAX_ERROR),
			        errmsg("cannot specify DELIMITER in BINARY mode")));

	if (cstate->binary && cstate->null_print)
		ereport(ERROR,
		        (errcode(ERRCODE_SYNTAX_ERROR),
			        errmsg("cannot specify NULL in BINARY mode")));

	cstate->eol_type = EOL_UNKNOWN;

	/* Set defaults for omitted options */
	if (!cstate->delim)
		cstate->delim = cstate->csv_mode ? "," : "\t";

	if (!cstate->null_print)
		cstate->null_print = cstate->csv_mode ? "" : "\\N";
	cstate->null_print_len = strlen(cstate->null_print);

	if (cstate->csv_mode)
	{
		if (!cstate->quote)
			cstate->quote  = "\"";
		if (!cstate->escape)
			cstate->escape = cstate->quote;
	}

	if (!cstate->csv_mode && !cstate->escape)
		cstate->escape = "\\";            /* default escape for text mode */

	/* Only single-byte delimiter strings are supported. */
	if (strlen(cstate->delim) != 1)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("COPY delimiter must be a single one-byte character")));

	/* Disallow end-of-line characters */
	if (strchr(cstate->delim, '\r') != NULL ||
		strchr(cstate->delim, '\n') != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg("COPY delimiter cannot be newline or carriage return")));

	if (strchr(cstate->null_print, '\r') != NULL ||
		strchr(cstate->null_print, '\n') != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg(
				        "COPY null representation cannot use newline or carriage return")));

	/*
	 * Disallow unsafe delimiter characters in non-CSV mode.  We can't allow
	 * backslash because it would be ambiguous.  We can't allow the other
	 * cases because data characters matching the delimiter must be
	 * backslashed, and certain backslash combinations are interpreted
	 * non-literally by COPY IN.  Disallowing all lower case ASCII letters is
	 * more than strictly necessary, but seems best for consistency and
	 * future-proofing.  Likewise we disallow all digits though only octal
	 * digits are actually dangerous.
	 */
	if (!cstate->csv_mode && !delim_off &&
		strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
		       cstate->delim[0]) != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg("delimiter cannot be \"%s\"", cstate->delim)));

	/* Check header */
	/*
	 * In PostgreSQL, HEADER is not allowed in text mode either, but in GPDB,
	 * only forbid it with BINARY.
	 */
	if (cstate->binary && cstate->header_line)
		ereport(ERROR,
		        (errcode(ERRCODE_SYNTAX_ERROR),
			        errmsg("cannot specify HEADER in BINARY mode")));

	/* Check quote */
	if (!cstate->csv_mode && cstate->quote != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("quote available only in CSV mode")));

	if (cstate->csv_mode && strlen(cstate->quote) != 1)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("quote must be a single one-byte character")));

	if (cstate->csv_mode && cstate->delim[0] == cstate->quote[0])
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg("delimiter and quote must be different")));

	/* Check escape */
	if (cstate->csv_mode && cstate->escape != NULL &&
		strlen(cstate->escape) != 1)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("escape in CSV format must be a single character")));

	if (!cstate->csv_mode && cstate->escape != NULL &&
		(strchr(cstate->escape, '\r') != NULL ||
			strchr(cstate->escape, '\n') != NULL))
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg(
				        "escape representation in text format cannot use newline or carriage return")));

	if (!cstate->csv_mode && cstate->escape != NULL &&
		strlen(cstate->escape) != 1)
	{
		if (pg_strcasecmp(cstate->escape, "off") != 0)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				        errmsg(
					        "escape must be a single character, or [OFF/off] to disable escapes")));
	}

	/* Check force_quote */
	if (!cstate->csv_mode && (cstate->force_quote || cstate->force_quote_all))
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("force quote available only in CSV mode")));
	if ((cstate->force_quote != NIL || cstate->force_quote_all) && is_from)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg(
				        "force quote only available for data unloading, not loading")));

	/* Check force_notnull */
	if (!cstate->csv_mode && cstate->force_notnull != NIL)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("force not null available only in CSV mode")));
	if (cstate->force_notnull != NIL && !is_from)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg(
				        "force not null only available for data loading, not unloading")));

	/* Check force_null */
	if (!cstate->csv_mode && cstate->force_null != NIL)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("COPY force null available only in CSV mode")));

	if (cstate->force_null != NIL && !is_from)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("COPY force null only available using COPY FROM")));

	/* Don't allow the delimiter to appear in the null string. */
	if (strchr(cstate->null_print, cstate->delim[0]) != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg(
				        "COPY delimiter must not appear in the NULL specification")));

	/* Don't allow the CSV quote char to appear in the null string. */
	if (cstate->csv_mode &&
		strchr(cstate->null_print, cstate->quote[0]) != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg(
				        "CSV quote character must not appear in the NULL specification")));

	/*
	 * DELIMITER
	 *
	 * Only single-byte delimiter strings are supported. In addition, if the
	 * server encoding is a multibyte character encoding we only allow the
	 * delimiter to be an ASCII character (like postgresql. For more info
	 * on this see discussion and comments in MPP-3756).
	 */
	if (pg_database_encoding_max_length() == 1)
	{
		/* single byte encoding such as ascii, latinx and other */
		if (strlen(cstate->delim) != 1 && !delim_off)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				        errmsg(
					        "delimiter must be a single one-byte character, or \'off\'")));
	}
	else
	{
		/* multi byte encoding such as utf8 */
		if ((strlen(cstate->delim) != 1 || IS_HIGHBIT_SET(cstate->delim[0])) &&
			!delim_off)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				        errmsg(
					        "delimiter must be a single one-byte character, or \'off\'")));
	}

	/* Disallow end-of-line characters */
	if (strchr(cstate->delim, '\r') != NULL ||
		strchr(cstate->delim, '\n') != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg("delimiter cannot be newline or carriage return")));

	if (strchr(cstate->null_print, '\r') != NULL ||
		strchr(cstate->null_print, '\n') != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg(
				        "null representation cannot use newline or carriage return")));

	if (!cstate->csv_mode && strchr(cstate->delim, '\\') != NULL)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			        errmsg("delimiter cannot be backslash")));

	if (strchr(cstate->null_print, cstate->delim[0]) != NULL && !delim_off)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg("delimiter must not appear in the NULL specification")));

	if (delim_off)
	{

		/*
		 * We don't support delimiter 'off' for COPY because the QD COPY
		 * sometimes internally adds columns to the data that it sends to
		 * the QE COPY modules, and it uses the delimiter for it. There
		 * are ways to work around this but for now it's not important and
		 * we simply don't support it.
		 */
		if (is_copy)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				        errmsg(
					        "Using no delimiter is only supported for external tables")));

		if (num_columns != 1)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				        errmsg(
					        "Using no delimiter is only possible for a single column table")));

	}


	/* Check header */
	if (cstate->header_line)
	{
		if (!is_copy && Gp_role == GP_ROLE_DISPATCH)
		{
			/* (exttab) */
			if (is_from)
			{
				/* RET */
				ereport(NOTICE,
				        (errmsg(
					        "HEADER means that each one of the data files has a header row.")));
			}
			else
			{
				/* WET */
				ereport(ERROR,
				        (errcode(ERRCODE_GP_FEATURE_NOT_YET),
					        errmsg(
						        "HEADER is not yet supported for writable external tables")));
			}
		}
	}

	if (cstate->fill_missing && !is_from)
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			        errmsg(
				        "fill missing fields only available for data loading, not unloading")));

	/*
	 * NEWLINE
	 */
	if (cstate->eol_str)
	{
		if (!is_from)
		{
			ereport(ERROR,
			        (errcode(ERRCODE_GP_FEATURE_NOT_YET),
				        errmsg(
					        "newline currently available for data loading only, not unloading")));
		}
		else
		{
			if (pg_strcasecmp(cstate->eol_str, "lf") != 0 &&
				pg_strcasecmp(cstate->eol_str, "cr") != 0 &&
				pg_strcasecmp(cstate->eol_str, "crlf") != 0)
				ereport(ERROR,
				        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					        errmsg("invalid value for NEWLINE (%s)",
					               cstate->eol_str),
					        errhint("valid options are: 'LF', 'CRLF', 'CR'")));
		}
	}

	if (cstate->escape != NULL && pg_strcasecmp(cstate->escape, "off") == 0)
	{
		cstate->escape_off = true;
	}

	/* set end of line type if NEWLINE keyword was specified */
	if (cstate->eol_str)
		CopyEolStrToType(cstate);
}

static int
pxf_callback(void *outbuf, int datasize, void *extra)
{
	return gpbridge_read(extra, outbuf, datasize);
}