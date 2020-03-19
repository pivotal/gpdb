-- start_matchsubs
-- end_matchsubs
-- ===================================================================
-- Core functionality for pxf_fdw
-- ===================================================================

-- ===================================================================
-- Create FDW objects
-- ===================================================================

DROP EXTENSION IF EXISTS pxf_fdw CASCADE;

CREATE EXTENSION pxf_fdw;

--
-- Checks the PXF version
--
SELECT pxf_fdw_version();


-- ===================================================================
-- Clean up FDW objects
-- ===================================================================

DROP EXTENSION IF EXISTS pxf_fdw CASCADE;