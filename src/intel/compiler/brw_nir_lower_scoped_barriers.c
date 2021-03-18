/*
 * Copyright (c) 2020 Intel Corporation
 * Copyright (c) 2020 Collabora Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * Lower scoped barriers embedding a control barrier (execusion_scope != NONE)
 * to scoped_barriers-without-control-barrier + control_barrier.
 */

#include "brw_nir.h"
#include "compiler/nir/nir_builder.h"

static bool
lower_impl(nir_function_impl *impl)
{
   nir_builder b;
   nir_builder_init(&b, impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic != nir_intrinsic_scoped_barrier ||
             nir_intrinsic_execution_scope(intr) == NIR_SCOPE_NONE)
            continue;

         if (nir_intrinsic_execution_scope(intr) == NIR_SCOPE_WORKGROUP) {
            b.cursor = nir_after_instr(&intr->instr);
            nir_intrinsic_instr *cbarrier =
               nir_intrinsic_instr_create(b.shader,
                                          nir_intrinsic_control_barrier);
            nir_builder_instr_insert(&b, &cbarrier->instr);
         }

         nir_intrinsic_set_execution_scope(intr, NIR_SCOPE_NONE);
         progress = true;
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

bool
brw_nir_lower_scoped_barriers(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function(function, nir) {
      if (function->impl)
         progress |= lower_impl(function->impl);
   }

   return progress;
}
