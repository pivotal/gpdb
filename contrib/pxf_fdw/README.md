pxf_fdw
=========
PXF foreign data wrapper for Greenplum 6+

This is a WIP PXF FDW
You can go a 

  create foreign data wrapper pxf_fdw

This allows for DDL to run without error. It does not allow for a select to
any foreign tables using this FDW. 


### Development

# Compile
make

# Install
make install

# Test
make installcheck