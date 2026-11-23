Sources of hakios-metal (https://github.com/oosexgang/hakios-xnu-kernel, `components/metal/plugin`, commit 62e09f8, MIT,
see LICENSE and NOTICE) that the compiler needs: the AIR -> NIR -> ir3 driver `compiler.cpp` and what it links against.
Copied unchanged except `src/compiler.cpp`: with `-DLAVENDER_GPU_ID=512` its compiler is made for the Adreno 512 (gpu id)
instead of hakios's A635 (chip id). Only `compileAir` (compute kernels) is used here.
