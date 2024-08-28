del *.dxil
del *.slang-module
slangc .\common.slang -embed-dxil -o .\common.slang-module
slangc .\material.slang -embed-dxil -o .\material.slang-module
slangc .\mdl_renderer_runtime.slang -embed-dxil -o .\mdl_renderer_runtime.slang-module
slangc .\mdl_target_code_types.slang -embed-dxil -o .\mdl_target_code_types.slang-module
slangc.exe .\mdl_hit_programs.slang -report-perf-benchmark -profile lib_6_6 -target dxil -o mdl_linked_slang.dxil
