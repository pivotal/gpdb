/*-------------------------------------------------------------------------
 *
 * pxf_deparse.c
 *		  Query deparser for pxf_fdw
 *
 * This file includes functions that examine query WHERE clauses to see
 * whether they're safe to send to the remote server for execution.
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_deparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pxf_fdw.h"
#include "optimizer/clauses.h"


/*
 * Global context for ForeignExprWalker's search of an expression tree.
 */
typedef struct foreign_glob_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
} foreign_glob_cxt;

/*
 * Local (per-tree-level) context for ForeignExprWalker's search.
 * This is concerned with identifying collations used in the expression.
 */
typedef enum
{
	FDW_COLLATE_NONE,			/* expression is of a noncollatable type */
	FDW_COLLATE_SAFE,			/* collation derives from a foreign Var */
	FDW_COLLATE_UNSAFE			/* collation derives from something else */
} FDWCollateState;

typedef struct foreign_loc_cxt
{
	Oid			collation;		/* OID of current collation, if any */
	FDWCollateState state;		/* state of current collation choice */
} foreign_loc_cxt;

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 */
static bool ForeignExprWalker(Node *node, foreign_glob_cxt *glob_cxt, foreign_loc_cxt *outer_cxt);
static bool IsBuiltin(Oid procid);
static bool IsForeignExpr(PlannerInfo *root, RelOptInfo *baserel, Expr *expr);

/*
 * We create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs.
 */
void
PxfDeparseTargetList(Relation rel, Bitmapset *attrs_used, List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	bool		have_wholerow;
	int			i;

	*retrieved_attrs = NIL;

	/* If there's a whole-row reference, we'll need all the columns. */
	have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used);

	if (have_wholerow)
		return;

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

		/* Ignore dropped attributes. */
		if (attr->attisdropped)
			continue;

		if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
		{
			*retrieved_attrs = lappend_int(*retrieved_attrs, i);
		}
	}
}

/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *	- remote_conds contains expressions that can be evaluated remotely
 *	- local_conds contains expressions that can't be evaluated remotely
 */
void
PxfClassifyConditions(PlannerInfo *root,
				   RelOptInfo *baserel,
				   List *input_conds,
				   List **remote_conds,
				   List **local_conds)
{
	ListCell   *lc;

	*remote_conds = NIL;
	*local_conds = NIL;

	foreach(lc, input_conds)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (IsForeignExpr(root, baserel, ri->clause))
			*remote_conds = lappend(*remote_conds, ri);
		else
			*local_conds = lappend(*local_conds, ri);
	}
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 */
static bool
IsForeignExpr(PlannerInfo *root,
              RelOptInfo *baserel,
              Expr *expr)
{
	foreign_glob_cxt glob_cxt;
	foreign_loc_cxt loc_cxt;

	/*
	 * Check that the expression consists of nodes that are safe to execute
	 * remotely.
	 */
	glob_cxt.root = root;
	glob_cxt.foreignrel = baserel;
	loc_cxt.collation = InvalidOid;
	loc_cxt.state = FDW_COLLATE_NONE;
	if (!ForeignExprWalker((Node *) expr, &glob_cxt, &loc_cxt))
		return false;

	/* Expressions examined here should be boolean, ie noncollatable */
	Assert(loc_cxt.collation == InvalidOid);
	Assert(loc_cxt.state == FDW_COLLATE_NONE);

	/*
	 * An expression which includes any mutable functions can't be sent over
	 * because its result is not stable.  For example, sending now() remote
	 * side could cause confusion from clock offsets.  Future versions might
	 * be able to make this choice with more granularity.  (We check this last
	 * because it requires a lot of expensive catalog lookups.)
	 */
	if (contain_mutable_functions((Node *) expr))
		return false;

	/* OK to evaluate on the remote server */
	return true;
}

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * In addition, *outer_cxt is updated with collation information.
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/functions/operators are safe to send (which we approximate
 * as being built-in), and that all collations used in the expression derive
 * from Vars of the foreign table.  Because of the latter, the logic is
 * pretty close to assign_collations_walker() in parse_collate.c, though we
 * can assume here that the given expression is valid.
 */
static bool
ForeignExprWalker(Node *node,
                  foreign_glob_cxt *glob_cxt,
                  foreign_loc_cxt *outer_cxt)
{
	foreign_loc_cxt inner_cxt;
	Oid			collation;
	FDWCollateState state;

	/* Need do nothing for empty subexpressions */
	if (node == NULL)
		return true;

	/* Set up inner_cxt for possible recursion to child nodes */
	inner_cxt.collation = InvalidOid;
	inner_cxt.state = FDW_COLLATE_NONE;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var *var = (Var *) node;

				if (bms_is_member(var->varno, glob_cxt->foreignrel->relids) &&
					var->varlevelsup == 0)
				{
					/* Var belongs to foreign table */
					if (var->varattno < 0 &&
						var->varattno != SelfItemPointerAttributeNumber)
						return false;
				}
			}
			break;
		case T_Const:
			{
			}
			break;
		case T_Param:
			{
			}
			break;
#if PG_VERSION_NUM < 120000
		case T_ArrayRef:
			{
				ArrayRef   *ar = (ArrayRef *) node;;

				/* Assignment should not be in restrictions. */
				if (ar->refassgnexpr != NULL)
					return false;

				/*
				 * Recurse to remaining subexpressions.  Since the array
				 * subscripts must yield (noncollatable) integers, they won't
				 * affect the inner_cxt state.
				 */
				if (!ForeignExprWalker((Node *) ar->refupperindexpr,
				                       glob_cxt, &inner_cxt))
					return false;
				if (!ForeignExprWalker((Node *) ar->reflowerindexpr,
				                       glob_cxt, &inner_cxt))
					return false;
				if (!ForeignExprWalker((Node *) ar->refexpr,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
#else
		case T_SubscriptingRef:
			{
				SubscriptingRef   *sbref = (SubscriptingRef *) node;;

				/* Assignment should not be in restrictions. */
				if (sbref->refassgnexpr != NULL)
					return false;

				/*
				 * Recurse to remaining subexpressions.  Since the array
				 * subscripts must yield (noncollatable) integers, they won't
				 * affect the inner_cxt state.
				 */
				if (!foreign_expr_walker((Node *) sbref->refupperindexpr,
										 glob_cxt, &inner_cxt))
					return false;
				if (!foreign_expr_walker((Node *) sbref->reflowerindexpr,
										 glob_cxt, &inner_cxt))
					return false;
				if (!ForeignExprWalker((Node *) sbref->refexpr,
										 glob_cxt, &inner_cxt))
					return false;
			}
			break;
#endif
		case T_FuncExpr:
			{
				FuncExpr   *fe = (FuncExpr *) node;

				/*
				 * If function used by the expression is not built-in, it
				 * can't be sent to remote because it might have incompatible
				 * semantics on remote side.
				 */
				if (!IsBuiltin(fe->funcid))
					return false;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) fe->args,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
			{
				OpExpr	   *oe = (OpExpr *) node;

				/*
				 * Similarly, only built-in operators can be sent to remote.
				 * (If the operator is, surely its underlying function is
				 * too.)
				 */
				if (!IsBuiltin(oe->opno))
					return false;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) oe->args,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *oe = (ScalarArrayOpExpr *) node;

				/*
				 * Again, only built-in operators can be sent to remote.
				 */
				if (!IsBuiltin(oe->opno))
					return false;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) oe->args,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) node;

				/*
				 * Recurse to input subexpression.
				 */
				if (!ForeignExprWalker((Node *) r->arg,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) node;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) b->args,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) nt->arg,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *a = (ArrayExpr *) node;

				/*
				 * Recurse to input subexpressions.
				 */
				if (!ForeignExprWalker((Node *) a->elements,
				                       glob_cxt, &inner_cxt))
					return false;
			}
			break;
		case T_List:
			{
				List	   *l = (List *) node;
				ListCell   *lc;

				/*
				 * Recurse to component subexpressions.
				 */
				foreach(lc, l)
				{
					if (!ForeignExprWalker((Node *) lfirst(lc),
					                       glob_cxt, &inner_cxt))
						return false;
				}
			}
			break;
		default:

			/*
			 * If it's anything else, assume it's unsafe.  This list can be
			 * expanded later, but don't forget to add deparse support below.
			 */
			return false;
	}

	/* It looks OK */
	return true;
}

/*
 * Return true if given object is one of PostgreSQL's built-in objects.
 *
 * We use FirstBootstrapObjectId as the cutoff, so that we only consider
 * objects with hand-assigned OIDs to be "built in", not for instance any
 * function or type defined in the information_schema.
 *
 * Our constraints for dealing with types are tighter than they are for
 * functions or operators: we want to accept only types that are in pg_catalog,
 * else format_type might incorrectly fail to schema-qualify their names.
 * (This could be fixed with some changes to format_type, but for now there's
 * no need.)  Thus we must exclude information_schema types.
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time.  Something that is built-in to us might not
 * be known to the remote server, if it's of an older version.  But keeping
 * track of that would be a huge exercise.
 */
static bool
IsBuiltin(Oid procid)
{
	return (procid < FirstBootstrapObjectId);
}
