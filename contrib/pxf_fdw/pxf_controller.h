/*
 * pxf_controller.h
 *		  Functions for reading and writing data from the PXF Server.
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_controller.h
 */

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#ifndef _PXF_CONTROLLER_H_
#define _PXF_CONTROLLER_H_

#include "pxf_option.h"

#include "pxf_curl.h"

#include "cdb/cdbvars.h"
#include "commands/copy.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

/*
 * Execution state of a foreign scan using pxf_fdw.
 */
typedef struct PxfFdwScanState
{
	PXF_CURL_HEADERS curl_headers;
	PXF_CURL_HANDLE curl_handle;
	StringInfoData uri;
	Relation	relation;
	char	   *filter_str;
	List	   *quals;
	List	   *retrieved_attrs;
	PxfOptions *options;
	CopyState	cstate;
} PxfFdwScanState;

/*
 * Execution state of a foreign insert operation.
 */
typedef struct PxfFdwModifyState
{
	CopyState	cstate;			/* state of writing to PXF */

	PXF_CURL_HANDLE curl_handle;	/* curl handle */
	PXF_CURL_HEADERS curl_headers;	/* curl headers */
	StringInfoData uri;			/* rest endpoint URI for modify */
	Relation	relation;
	PxfOptions *options;		/* FDW options */

	Datum	   *values;			/* List of values exported for the row */
	bool	   *nulls;			/* List of null fields for the exported row */
} PxfFdwModifyState;

/* Clean up churl related data structures from the context */
void		PxfControllerCleanup(PxfFdwModifyState *context);

/* Sets up data before starting import */
void		PxfControllerImportStart(PxfFdwScanState *pxfsstate);

/* Sets up data before starting export */
void		PxfControllerExportStart(PxfFdwModifyState *pxfmstate);

/* Reads data from the PXF server into the given buffer of a given size */
int			PxfControllerRead(void *outbuf, int datasize, void *extra);

/* Writes data from the given buffer of a given size to the PXF server */
int			PxfControllerWrite(PxfFdwModifyState *context, char *databuf, int datalen);

#endif							/* _PXF_CONTROLLER_H_ */
