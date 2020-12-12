/* SLP - Pattern matcher on SLP trees
   Copyright (C) 2020 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "tree-pass.h"
#include "ssa.h"
#include "optabs-tree.h"
#include "insn-config.h"
#include "recog.h"		/* FIXME: for insn_data */
#include "fold-const.h"
#include "stor-layout.h"
#include "gimple-iterator.h"
#include "cfgloop.h"
#include "tree-vectorizer.h"
#include "langhooks.h"
#include "gimple-walk.h"
#include "dbgcnt.h"
#include "tree-vector-builder.h"
#include "vec-perm-indices.h"
#include "gimple-fold.h"
#include "internal-fn.h"

/* SLP Pattern matching mechanism.

  This extension to the SLP vectorizer allows one to transform the generated SLP
  tree based on any pattern.  The difference between this and the normal vect
  pattern matcher is that unlike the former, this matcher allows you to match
  with instructions that do not belong to the same SSA dominator graph.

  The only requirement that this pattern matcher has is that you are only
  only allowed to either match an entire group or none.

  The pattern matcher currently only allows you to perform replacements to
  internal functions.

  Once the patterns are matched it is one way, these cannot be undone.  It is
  currently not supported to match patterns recursively.

  To add a new pattern, implement the vect_pattern class and add the type to
  slp_patterns.

*/

/*******************************************************************************
 * vect_pattern class
 ******************************************************************************/

/* Default implementation of recognize that performs matching, validation and
   replacement of nodes but that can be overriden if required.  */

static bool
vect_pattern_validate_optab (internal_fn ifn, slp_tree node)
{
  tree vectype = SLP_TREE_VECTYPE (node);
  if (ifn == IFN_LAST || !vectype)
    return false;

  if (dump_enabled_p ())
    dump_printf_loc (MSG_NOTE, vect_location,
		     "Found %s pattern in SLP tree\n",
		     internal_fn_name (ifn));

  if (direct_internal_fn_supported_p (ifn, vectype, OPTIMIZE_FOR_SPEED))
    {
      if (dump_enabled_p ())
	dump_printf_loc (MSG_NOTE, vect_location,
			 "Target supports %s vectorization with mode %T\n",
			 internal_fn_name (ifn), vectype);
    }
  else
    {
      if (dump_enabled_p ())
        {
	  if (!vectype)
	    dump_printf_loc (MSG_NOTE, vect_location,
			     "Target does not support vector type for %T\n",
			     SLP_TREE_DEF_TYPE (node));
	  else
	    dump_printf_loc (MSG_NOTE, vect_location,
			     "Target does not support %s for vector type "
			     "%T\n", internal_fn_name (ifn), vectype);
	}
      return false;
    }
  return true;
}

/*******************************************************************************
 * General helper types
 ******************************************************************************/

/* The COMPLEX_OPERATION enum denotes the possible pair of operations that can
   be matched when looking for expressions that we are interested matching for
   complex numbers addition and mla.  */

typedef enum _complex_operation : unsigned {
  PLUS_PLUS,
  MINUS_PLUS,
  PLUS_MINUS,
  MULT_MULT,
  CMPLX_NONE
} complex_operation_t;

/*******************************************************************************
 * General helper functions
 ******************************************************************************/

/* Helper function of linear_loads_p that checks to see if the load permutation
   is sequential and in monotonically increasing order of loads with no gaps.
*/

static inline complex_perm_kinds_t
is_linear_load_p (load_permutation_t loads)
{
  if (loads.length() == 0)
    return PERM_UNKNOWN;

  unsigned load, i;
  complex_perm_kinds_t candidates[4]
    = { PERM_EVENODD
      , PERM_ODDEVEN
      , PERM_ODDODD
      , PERM_EVENEVEN
      };

  int valid_patterns = 4;
  FOR_EACH_VEC_ELT_FROM (loads, i, load, 1)
    {
      if (candidates[0] != PERM_UNKNOWN && load != i)
	{
	  candidates[0] = PERM_UNKNOWN;
	  valid_patterns--;
	}
      if (candidates[1] != PERM_UNKNOWN
	  && load != (i % 2 == 0 ? i + 1 : i - 1))
	{
	  candidates[1] = PERM_UNKNOWN;
	  valid_patterns--;
	}
      if (candidates[2] != PERM_UNKNOWN && load != 1)
	{
	  candidates[2] = PERM_UNKNOWN;
	  valid_patterns--;
	}
      if (candidates[3] != PERM_UNKNOWN && load != 0)
	{
	  candidates[3] = PERM_UNKNOWN;
	  valid_patterns--;
	}

      if (valid_patterns == 0)
	return PERM_UNKNOWN;
    }

  for (i = 0; i < sizeof(candidates); i++)
    if (candidates[i] != PERM_UNKNOWN)
      return candidates[i];

  return PERM_UNKNOWN;
}

/* Combine complex_perm_kinds A and B into a new permute kind that describes the
   resulting operation.  */

static inline complex_perm_kinds_t
vect_merge_perms (complex_perm_kinds_t a, complex_perm_kinds_t b)
{
  if (a == b)
    return a;

  if (a == PERM_TOP)
    return b;

  if (b == PERM_TOP)
    return a;

  return PERM_UNKNOWN;
}

/* Check to see if all loads rooted in ROOT are linear.  Linearity is
   defined as having no gaps between values loaded.  */

static complex_load_perm_t
linear_loads_p (slp_tree_to_load_perm_map_t *perm_cache, slp_tree root)
{
  if (!root)
    return std::make_pair (PERM_UNKNOWN, vNULL);

  unsigned i;
  complex_load_perm_t *tmp;

  if ((tmp = perm_cache->get (root)) != NULL)
    return *tmp;

  complex_load_perm_t retval = std::make_pair (PERM_UNKNOWN, vNULL);
  perm_cache->put (root, retval);

  /* If it's a load node, then just read the load permute.  */
  if (SLP_TREE_LOAD_PERMUTATION (root).exists ())
    {
      retval.first = is_linear_load_p (SLP_TREE_LOAD_PERMUTATION (root));
      retval.second = SLP_TREE_LOAD_PERMUTATION (root);
      perm_cache->put (root, retval);
      return retval;
    }
  else if (SLP_TREE_DEF_TYPE (root) != vect_internal_def)
    {
      retval.first = PERM_TOP;
      return retval;
    }

  auto_vec<load_permutation_t> all_loads;
  complex_perm_kinds_t kind = PERM_TOP;

  slp_tree child;
  FOR_EACH_VEC_ELT (SLP_TREE_CHILDREN (root), i, child)
    {
      complex_load_perm_t res = linear_loads_p (perm_cache, child);
      kind = vect_merge_perms (kind, res.first);
      if (kind == PERM_UNKNOWN)
	return retval;
      all_loads.safe_push (res.second);
    }

  if (SLP_TREE_LANE_PERMUTATION (root).exists ())
    {
      lane_permutation_t perm = SLP_TREE_LANE_PERMUTATION (root);
      load_permutation_t nloads;
      nloads.create (SLP_TREE_LANES (root));
      nloads.quick_grow (SLP_TREE_LANES (root));
      for (i = 0; i < SLP_TREE_LANES (root); i++)
	nloads[i] = all_loads[perm[i].first][perm[i].second];

      retval.first = kind;
      retval.second = nloads;
    }
  else if (all_loads.length () == 1)
    {
      retval.first = kind;
      retval.second = all_loads[0];
    }

  perm_cache->put (root, retval);
  return retval;
}


/* This function attempts to make a node rooted in NODE is linear.  If the node
   if already linear than the node itself is returned in RESULT.

   If the node is not linear then a new VEC_PERM_EXPR node is created with a
   lane permute that when applied will make the node linear.   If such a
   permute cannot be created then FALSE is returned from the function.

   Here linearity is defined as having a sequential, monotically increasing
   load position inside the load permute generated by the loads reachable from
   NODE.  */

static slp_tree
vect_build_swap_evenodd_node (slp_tree node)
{
  /* Attempt to linearise the permute.  */
  vec<std::pair<unsigned, unsigned> > zipped;
  zipped.create (SLP_TREE_LANES (node));

  for (unsigned x = 0; x < SLP_TREE_LANES (node); x+=2)
    {
      zipped.quick_push (std::make_pair (0, x+1));
      zipped.quick_push (std::make_pair (0, x));
    }

  /* Create the new permute node and store it instead.  */
  slp_tree vnode = vect_create_new_slp_node (1, VEC_PERM_EXPR);
  SLP_TREE_LANE_PERMUTATION (vnode) = zipped;
  SLP_TREE_VECTYPE (vnode) = SLP_TREE_VECTYPE (node);
  SLP_TREE_CHILDREN (vnode).quick_push (node);
  SLP_TREE_REF_COUNT (vnode) = 1;
  SLP_TREE_LANES (vnode) = SLP_TREE_LANES (node);
  SLP_TREE_REPRESENTATIVE (vnode) = SLP_TREE_REPRESENTATIVE (node);
  SLP_TREE_REF_COUNT (node)++;
  return vnode;
}

/* Checks to see of the expression represented by NODE is a gimple assign with
   code CODE.  */

static inline bool
vect_match_expression_p (slp_tree node, tree_code code)
{
  if (!node
      || !SLP_TREE_REPRESENTATIVE (node))
    return false;

  gimple* expr = STMT_VINFO_STMT (SLP_TREE_REPRESENTATIVE (node));
  if (!is_gimple_assign (expr)
      || gimple_assign_rhs_code (expr) != code)
    return false;

  return true;
}

/* Check if the given lane permute in PERMUTES matches an alternating sequence
   of {even odd even odd ...}.  This to account for unrolled loops.  Further
   mode there resulting permute must be linear.   */

static inline bool
vect_check_evenodd_blend (lane_permutation_t &permutes,
			 unsigned even, unsigned odd)
{
  if (permutes.length () == 0)
    return false;

  unsigned val[2] = {even, odd};
  unsigned seed = 0;
  for (unsigned i = 0; i < permutes.length (); i++)
    if (permutes[i].first != val[i % 2]
	|| permutes[i].second != seed++)
      return false;

  return true;
}

/* This function will match the two gimple expressions representing NODE1 and
   NODE2 in parallel and returns the pair operation that represents the two
   expressions in the two statements.

   If match is successful then the corresponding complex_operation is
   returned and the arguments to the two matched operations are returned in OPS.

   If TWO_OPERANDS it is expected that the LANES of the parent VEC_PERM select
   from the two nodes alternatingly.

   If unsuccessful then CMPLX_NONE is returned and OPS is untouched.

   e.g. the following gimple statements

   stmt 0 _39 = _37 + _12;
   stmt 1 _6 = _38 - _36;

   will return PLUS_MINUS along with OPS containing {_37, _12, _38, _36}.
*/

static complex_operation_t
vect_detect_pair_op (slp_tree node1, slp_tree node2, lane_permutation_t &lanes,
		     bool two_operands = true, vec<slp_tree> *ops = NULL)
{
  complex_operation_t result = CMPLX_NONE;

  if (vect_match_expression_p (node1, MINUS_EXPR)
      && vect_match_expression_p (node2, PLUS_EXPR)
      && (!two_operands || vect_check_evenodd_blend (lanes, 0, 1)))
    result = MINUS_PLUS;
  else if (vect_match_expression_p (node1, PLUS_EXPR)
	   && vect_match_expression_p (node2, MINUS_EXPR)
	   && (!two_operands || vect_check_evenodd_blend (lanes, 0, 1)))
    result = PLUS_MINUS;
  else if (vect_match_expression_p (node1, PLUS_EXPR)
	   && vect_match_expression_p (node2, PLUS_EXPR))
    result = PLUS_PLUS;
  else if (vect_match_expression_p (node1, MULT_EXPR)
	   && vect_match_expression_p (node2, MULT_EXPR))
    result = MULT_MULT;

  if (result != CMPLX_NONE && ops != NULL)
    {
      ops->create (2);
      ops->quick_push (node1);
      ops->quick_push (node2);
    }
  return result;
}

/* Overload of vect_detect_pair_op that matches against the representative
   statements in the children of NODE.  It is expected that NODE has exactly
   two children and when TWO_OPERANDS then NODE must be a VEC_PERM.  */

static complex_operation_t
vect_detect_pair_op (slp_tree node, bool two_operands = true,
		     vec<slp_tree> *ops = NULL)
{
  if (!two_operands && SLP_TREE_CODE (node) == VEC_PERM_EXPR)
    return CMPLX_NONE;

  if (SLP_TREE_CHILDREN (node).length () != 2)
    return CMPLX_NONE;

  vec<slp_tree> children = SLP_TREE_CHILDREN (node);
  lane_permutation_t &lanes = SLP_TREE_LANE_PERMUTATION (node);

  return vect_detect_pair_op (children[0], children[1], lanes, two_operands,
			      ops);
}

/*******************************************************************************
 * complex_pattern class
 ******************************************************************************/

/* SLP Complex Numbers pattern matching.

  As an example, the following simple loop:

    double a[restrict N]; double b[restrict N]; double c[restrict N];

    for (int i=0; i < N; i+=2)
    {
      c[i] = a[i] - b[i+1];
      c[i+1] = a[i+1] + b[i];
    }

  which represents a complex addition on with a rotation of 90* around the
  argand plane. i.e. if `a` and `b` were complex numbers then this would be the
  same as `a + (b * I)`.

  Here the expressions for `c[i]` and `c[i+1]` are independent but have to be
  both recognized in order for the pattern to work.  As an SLP tree this is
  represented as

                +--------------------------------+
                |       stmt 0 *_9 = _10;        |
                |       stmt 1 *_15 = _16;       |
                +--------------------------------+
                                |
                                |
                                v
                +--------------------------------+
                |     stmt 0 _10 = _4 - _8;      |
                |    stmt 1 _16 = _12 + _14;     |
                | lane permutation { 0[0] 1[1] } |
                +--------------------------------+
                            |        |
                            |        |
                            |        |
               +-----+      |        |      +-----+
               |     |      |        |      |     |
         +-----| { } |<-----+        +----->| { } --------+
         |     |     |   +------------------|     |       |
         |     +-----+   |                  +-----+       |
         |        |      |                                |
         |        |      |                                |
         |        +------|------------------+             |
         |               |                  |             |
         v               v                  v             v
     +--------------------------+     +--------------------------------+
     |     stmt 0 _8 = *_7;     |     |        stmt 0 _4 = *_3;        |
     |    stmt 1 _14 = *_13;    |     |       stmt 1 _12 = *_11;       |
     | load permutation { 1 0 } |     |    load permutation { 0 1 }    |
     +--------------------------+     +--------------------------------+

  The pattern matcher allows you to replace both statements 0 and 1 or none at
  all.  Because this operation is a two operands operation the actual nodes
  being replaced are those in the { } nodes.  The actual scalar statements
  themselves are not replaced or used during the matching but instead the
  SLP_TREE_REPRESENTATIVE statements are inspected.  You are also allowed to
  replace and match on any number of nodes.

  Because the pattern matcher matches on the representative statement for the
  SLP node the case of two_operators it allows you to match the children of the
  node.  This is done using the method `recognize ()`.

*/

/* The complex_pattern class contains common code for pattern matchers that work
   on complex numbers.  These provide functionality to allow de-construction and
   validation of sequences depicting/transforming REAL and IMAG pairs.  */

class complex_pattern : public vect_pattern
{
  protected:
    auto_vec<slp_tree> m_workset;
    complex_pattern (slp_tree *node, vec<slp_tree> *m_ops, internal_fn ifn)
      : vect_pattern (node, m_ops, ifn)
    {
      this->m_workset.safe_push (*node);
    }

  public:
    void build (vec_info *);

    static internal_fn
    matches (complex_operation_t op, slp_tree_to_load_perm_map_t *,
	     vec<slp_tree> *);
};

/* Create a replacement pattern statement for each node in m_node and inserts
   the new statement into m_node as the new representative statement.  The old
   statement is marked as being in a pattern defined by the new statement.  The
   statement is created as call to internal function IFN with m_num_args
   arguments.

   Futhermore the new pattern is also added to the vectorization information
   structure VINFO and the old statement STMT_INFO is marked as unused while
   the new statement is marked as used and the number of SLP uses of the new
   statement is incremented.

   The newly created SLP nodes are marked as SLP only and will be dissolved
   if SLP is aborted.

   The newly created gimple call is returned and the BB remains unchanged.

   This default method is designed to only match against simple operands where
   all the input and output types are the same.
*/

void
complex_pattern::build (vec_info *vinfo)
{
  stmt_vec_info stmt_info;

  auto_vec<tree> args;
  args.create (this->m_num_args);
  args.quick_grow_cleared (this->m_num_args);
  slp_tree node;
  unsigned ix;
  stmt_vec_info call_stmt_info;
  gcall *call_stmt = NULL;

  /* Now modify the nodes themselves.  */
  FOR_EACH_VEC_ELT (this->m_workset, ix, node)
    {
      /* Calculate the location of the statement in NODE to replace.  */
      stmt_info = SLP_TREE_REPRESENTATIVE (node);
      gimple* old_stmt = STMT_VINFO_STMT (stmt_info);
      tree lhs_old_stmt = gimple_get_lhs (old_stmt);
      tree type = TREE_TYPE (lhs_old_stmt);

      /* Create the argument set for use by gimple_build_call_internal_vec.  */
      for (unsigned i = 0; i < this->m_num_args; i++)
	args[i] = lhs_old_stmt;

      /* Create the new pattern statements.  */
      call_stmt = gimple_build_call_internal_vec (this->m_ifn, args);
      tree var = make_temp_ssa_name (type, call_stmt, "slp_patt");
      gimple_call_set_lhs (call_stmt, var);
      gimple_set_location (call_stmt, gimple_location (old_stmt));
      gimple_call_set_nothrow (call_stmt, true);

      /* Adjust the book-keeping for the new and old statements for use during
	 SLP.  This is required to get the right VF and statement during SLP
	 analysis.  These changes are created after relevancy has been set for
	 the nodes as such we need to manually update them.  Any changes will be
	 undone if SLP is cancelled.  */
      call_stmt_info
	= vinfo->add_pattern_stmt (call_stmt, stmt_info);

      /* Make sure to mark the representative statement pure_slp and
	 relevant. */
      STMT_VINFO_RELEVANT (call_stmt_info) = vect_used_in_scope;
      STMT_SLP_TYPE (call_stmt_info) = pure_slp;

      /* add_pattern_stmt can't be done in vect_mark_pattern_stmts because
	 the non-SLP pattern matchers already have added the statement to VINFO
	 by the time it is called.  Some of them need to modify the returned
	 stmt_info.  vect_mark_pattern_stmts is called by recog_pattern and it
	 would increase the size of each pattern with boilerplate code to make
	 the call there.  */
      vect_mark_pattern_stmts (vinfo, stmt_info, call_stmt,
			       SLP_TREE_VECTYPE (node));
      STMT_VINFO_SLP_VECT_ONLY (call_stmt_info) = true;

      /* Since we are replacing all the statements in the group with the same
	 thing it doesn't really matter.  So just set it every time a new stmt
	 is created.  */
      SLP_TREE_REPRESENTATIVE (node) = call_stmt_info;
      SLP_TREE_LANE_PERMUTATION (node).release ();
      SLP_TREE_CODE (node) = CALL_EXPR;
    }
}

/*******************************************************************************
 * complex_add_pattern class
 ******************************************************************************/

class complex_add_pattern : public complex_pattern
{
  protected:
    complex_add_pattern (slp_tree *node, vec<slp_tree> *m_ops, internal_fn ifn)
      : complex_pattern (node, m_ops, ifn)
    {
      this->m_num_args = 2;
    }

  public:
    void build (vec_info *);
    static internal_fn
    matches (complex_operation_t op, slp_tree_to_load_perm_map_t *,
	     vec<slp_tree> *);

    static vect_pattern*
    recognize (slp_tree_to_load_perm_map_t *, slp_tree *);
};

/* Perform a replacement of the detected complex add pattern with the new
   instruction sequences.  */

void
complex_add_pattern::build (vec_info *vinfo)
{
  auto_vec<slp_tree> nodes;
  slp_tree node = this->m_ops[0];
  vec<slp_tree> children = SLP_TREE_CHILDREN (node);

  /* First re-arrange the children.  */
  nodes.create (children.length ());
  nodes.quick_push (children[0]);
  nodes.quick_push (vect_build_swap_evenodd_node (children[1]));

  SLP_TREE_CHILDREN (*this->m_node).truncate (0);
  SLP_TREE_CHILDREN (*this->m_node).safe_splice (nodes);

  complex_pattern::build (vinfo);
}

/* Pattern matcher for trying to match complex addition pattern in SLP tree.

   If no match is found then IFN is set to IFN_LAST.
   This function matches the patterns shaped as:

   c[i] = a[i] - b[i+1];
   c[i+1] = a[i+1] + b[i];

   If a match occurred then TRUE is returned, else FALSE.  The initial match is
   expected to be in OP1 and the initial match operands in args0.  */

internal_fn
complex_add_pattern::matches (complex_operation_t op,
			      slp_tree_to_load_perm_map_t *perm_cache,
			      vec<slp_tree> *ops)
{
  internal_fn ifn = IFN_LAST;

  /* Find the two components.  Rotation in the complex plane will modify
     the operations:

      * Rotation  0: + +
      * Rotation 90: - +
      * Rotation 180: - -
      * Rotation 270: + -

      Rotation 0 and 180 can be handled by normal SIMD code, so we don't need
      to care about them here.  */
  if (op == MINUS_PLUS)
    ifn = IFN_COMPLEX_ADD_ROT90;
  else if (op == PLUS_MINUS)
    ifn = IFN_COMPLEX_ADD_ROT270;
  else
    return ifn;

  /* verify that there is a permute, otherwise this isn't a pattern we
     we support.  */
  gcc_assert (ops->length () == 2);

  vec<slp_tree> children = SLP_TREE_CHILDREN ((*ops)[0]);

  /* First node must be unpermuted.  */
  if (linear_loads_p (perm_cache, children[0]).first != PERM_EVENODD)
    return IFN_LAST;

  /* Second node must be permuted.  */
  if (linear_loads_p (perm_cache, children[1]).first != PERM_ODDEVEN)
    return IFN_LAST;

  return ifn;
}

/* Attempt to recognize a complex add pattern.  */

vect_pattern*
complex_add_pattern::recognize (slp_tree_to_load_perm_map_t *perm_cache,
				slp_tree *node)
{
  auto_vec<slp_tree> ops;
  complex_operation_t op
    = vect_detect_pair_op (*node, true, &ops);
  internal_fn ifn = complex_add_pattern::matches (op, perm_cache, &ops);
  if (!vect_pattern_validate_optab (ifn, *node))
    return NULL;

  return new complex_add_pattern (node, &ops, ifn);
}

/*******************************************************************************
 * Pattern matching definitions
 ******************************************************************************/

#define SLP_PATTERN(x) &x::recognize
vect_pattern_decl_t slp_patterns[]
{
  /* For least amount of back-tracking and more efficient matching
     order patterns from the largest to the smallest.  Especially if they
     overlap in what they can detect.  */

  SLP_PATTERN (complex_add_pattern),
};
#undef SLP_PATTERN

/* Set the number of SLP pattern matchers available.  */
size_t num__slp_patterns = sizeof(slp_patterns)/sizeof(vect_pattern_decl_t);