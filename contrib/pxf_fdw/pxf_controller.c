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

#include "pxf_controller.h"

#include "pxf_header.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"

/* helper function declarations */
static void BuildUriForRead(PxfFdwScanState *pxfsstate);
static void BuildUriForWrite(PxfFdwModifyState *pxfmstate);
static size_t FillBuffer(PxfFdwScanState *pxfsstate, char *start, size_t size);

/*
 * Clean up churl related data structures from the PXF FDW modify state.
 */
void
PxfControllerCleanup(PxfFdwModifyState *pxfmstate)
{
	if (pxfmstate == NULL)
		return;

	churl_cleanup(pxfmstate->churl_handle, false);
	pxfmstate->churl_handle = NULL;

	churl_headers_cleanup(pxfmstate->churl_headers);
	pxfmstate->churl_headers = NULL;

	/* TODO: do we need to cleanup filter_str for foreign scan? */
/*	if (pxfmstate->filter_str != NULL)
	{
		pfree(pxfmstate->filter_str);
		pxfmstate->filter_str = NULL;
	}*/
}

/*
 * Sets up data before starting import
 */
void
PxfControllerImportStart(PxfFdwScanState *pxfsstate)
{
	pxfsstate->churl_headers = churl_headers_init();

	BuildUriForRead(pxfsstate);
	BuildHttpHeaders(pxfsstate->churl_headers,
					 pxfsstate->options,
					 pxfsstate->relation,
					 pxfsstate->filter_str,
					 pxfsstate->retrieved_attrs);

	pxfsstate->churl_handle = churl_init_download(pxfsstate->uri.data, pxfsstate->churl_headers);

	/* read some bytes to make sure the connection is established */
	churl_read_check_connectivity(pxfsstate->churl_handle);
}

/*
 * Sets up data before starting export
 */
void
PxfControllerExportStart(PxfFdwModifyState *pxfmstate)
{
	BuildUriForWrite(pxfmstate);
	pxfmstate->churl_headers = churl_headers_init();
	BuildHttpHeaders(pxfmstate->churl_headers,
					 pxfmstate->options,
					 pxfmstate->relation,
					 NULL,
					 NULL);
	pxfmstate->churl_handle = churl_init_upload(pxfmstate->uri.data, pxfmstate->churl_headers);
}

/*
 * Reads data from the PXF server into the given buffer of a given size
 */
int
PxfControllerRead(void *outbuf, int datasize, void *extra)
{
	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) extra;
	return (int) FillBuffer(pxfsstate, outbuf, datasize);
}

/*
 * Writes data from the given buffer of a given size to the PXF server
 */
int
PxfControllerWrite(PxfFdwModifyState *pxfmstate, char *databuf, int datalen)
{
	size_t		n = 0;

	if (datalen > 0)
	{
		n = churl_write(pxfmstate->churl_handle, databuf, datalen);
		elog(DEBUG5, "pxf PxfControllerWrite: segment %d wrote %zu bytes to %s", PXF_SEGMENT_ID, n, pxfmstate->options->resource);
	}

	return (int) n;
}

/*
 * Format the URI for reading by adding PXF service endpoint details
 */
static void
BuildUriForRead(PxfFdwScanState *pxfsstate)
{
	PxfOptions *options = pxfsstate->options;
	Assert(options->pxf_host != NULL && options->pxf_port > 0);

	resetStringInfo(&pxfsstate->uri);
	appendStringInfo(&pxfsstate->uri, "http://%s:%d/%s/%s/Controller", options->pxf_host, options->pxf_port, PXF_SERVICE_PREFIX, PXF_VERSION);
	elog(DEBUG2, "pxf_fdw: uri %s for read", pxfsstate->uri.data);
}

/*
 * Format the URI for writing by adding PXF service endpoint details
 */
static void
BuildUriForWrite(PxfFdwModifyState *pxfmstate)
{
	PxfOptions *options = pxfmstate->options;

	resetStringInfo(&pxfmstate->uri);
	appendStringInfo(&pxfmstate->uri, "http://%s/%s/%s/Writable/stream", psprintf("%s:%d", options->pxf_host, options->pxf_port), PXF_SERVICE_PREFIX, PXF_VERSION);
	elog(DEBUG2, "pxf_fdw: uri %s with file name for write: %s", pxfmstate->uri.data, options->resource);
}

/*
 * Read data from churl until the buffer is full or there is no more data to be read
 */
static size_t
FillBuffer(PxfFdwScanState *pxfsstate, char *start, size_t size)
{
	size_t		n = 0;
	char	   *ptr = start;
	char	   *end = ptr + size;

	while (ptr < end)
	{
		n = churl_read(pxfsstate->churl_handle, ptr, end - ptr);
		if (n == 0)
			break;

		ptr += n;
	}

	return ptr - start;
}
