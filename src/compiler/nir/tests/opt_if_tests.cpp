/*
 * Copyright © 2020 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <gtest/gtest.h>
#include "nir.h"
#include "nir_builder.h"

class nir_opt_if_test : public ::testing::Test {
protected:
   nir_opt_if_test();
   ~nir_opt_if_test();

   nir_builder bld;

   nir_ssa_def *in_def;
   nir_variable *out_var;
};

nir_opt_if_test::nir_opt_if_test()
{
   glsl_type_singleton_init_or_ref();

   static const nir_shader_compiler_options options = { };
   nir_builder_init_simple_shader(&bld, NULL, MESA_SHADER_VERTEX, &options);

   nir_variable *var = nir_variable_create(bld.shader, nir_var_shader_in, glsl_int_type(), "in");
   in_def = nir_load_var(&bld, var);

   out_var = nir_variable_create(bld.shader, nir_var_shader_out, glsl_int_type(), "out");
}

nir_opt_if_test::~nir_opt_if_test()
{
   ralloc_free(bld.shader);
   glsl_type_singleton_decref();
}

TEST_F(nir_opt_if_test, opt_if_simplification)
{
   /* Tests that opt_if_simplification correctly optimizes a simple case:
    *
    * vec1 1 ssa_2 = ieq ssa_0, ssa_1
    * if ssa_2 {
    *    block block_2:
    * } else {
    *    block block_3:
    *    do_work()
    * }
    */

   nir_ssa_def *one = nir_imm_int(&bld, 1);

   nir_ssa_def *cmp_result = nir_ieq(&bld, in_def, one);
   nir_if *nif = nir_push_if(&bld, cmp_result);

   nir_push_else(&bld, NULL);

   // do_work
   nir_store_var(&bld, out_var, one, 1);

   nir_pop_if(&bld, NULL);

   ASSERT_TRUE(nir_opt_if(bld.shader, false));

   nir_validate_shader(bld.shader, NULL);

   ASSERT_TRUE(!exec_list_is_empty((&nir_if_first_then_block(nif)->instr_list)));
   ASSERT_TRUE(exec_list_is_empty((&nir_if_first_else_block(nif)->instr_list)));
}

TEST_F(nir_opt_if_test, opt_if_simplification_single_source_phi_after_if)
{
   /* Tests that opt_if_simplification correctly handles single-source
    * phis after the if.
    *
    * vec1 1 ssa_2 = ieq ssa_0, ssa_1
    * if ssa_2 {
    *    block block_2:
    * } else {
    *    block block_3:
    *    do_work()
    *    return
    * }
    * block block_4:
    * vec1 32 ssa_3 = phi block_2: ssa_0
    */

   nir_ssa_def *one = nir_imm_int(&bld, 1);

   nir_ssa_def *cmp_result = nir_ieq(&bld, in_def, one);
   nir_if *nif = nir_push_if(&bld, cmp_result);

   nir_push_else(&bld, NULL);

   // do_work
   nir_store_var(&bld, out_var, one, 1);

   nir_jump_instr *jump = nir_jump_instr_create(bld.shader, nir_jump_return);
   nir_builder_instr_insert(&bld, &jump->instr);

   nir_pop_if(&bld, NULL);

   nir_block *then_block = nir_if_last_then_block(nif);

   nir_phi_instr *const phi = nir_phi_instr_create(bld.shader);

   nir_phi_src *phi_src;
   phi_src = ralloc(phi, nir_phi_src);
   phi_src->pred = then_block;
   phi_src->src = nir_src_for_ssa(one);
   exec_list_push_tail(&phi->srcs, &phi_src->node);

   nir_ssa_dest_init(&phi->instr, &phi->dest,
                     one->num_components, one->bit_size, NULL);

   nir_builder_instr_insert(&bld, &phi->instr);

   ASSERT_TRUE(nir_opt_if(bld.shader, false));

   nir_validate_shader(bld.shader, NULL);

   ASSERT_TRUE(nir_block_ends_in_jump(nir_if_last_then_block(nif)));
   ASSERT_TRUE(exec_list_is_empty((&nir_if_first_else_block(nif)->instr_list)));
}
