del *.dxil
del *.slang-module
slangc .\hit.slang -report-perf-benchmark -profile lib_6_6 -target dxil -o mdl_linked_slang.dxil
