del *.dxil
del *.slang-module
slangc .\common.slang -o .\common.slang-module
slangc .\material.slang -o .\material.slang-module
slangc .\runtime.slang -o .\runtime.slang-module
slangc .\types.slang -o .\types.slang-module
slangc .\lighting.slang -o .\lighting.slang-module
slangc .\hit.slang -report-perf-benchmark -profile lib_6_6 -target dxil -o mdl_linked_slang.dxil
