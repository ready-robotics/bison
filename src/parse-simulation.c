/* Parser simulator for unifying counterexample search
 
 Copyright (C) 2019-2020 Free Software Foundation, Inc.
 
 This file is part of Bison, the GNU Compiler Compiler.
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include <gl_linked_list.h>
#include <gl_xlist.h>
#include <stdlib.h>
#include "parse-simulation.h"
#include "derivation.h"
#include "nullable.h"
#include "lssi.h"

void
ps_chunk_prepend (ps_chunk *chunk, void *element)
{
  gl_list_add_first (chunk->contents, element);
  chunk->head_elt = element;
  ++chunk->total_size;
  if (!chunk->tail_elt)
    chunk->tail_elt = element;
}

void
ps_chunk_append (ps_chunk *chunk, void *element)
{
  gl_list_add_last (chunk->contents, element);
  chunk->tail_elt = element;
  ++chunk->total_size;
  if (!chunk->head_elt)
    chunk->head_elt = element;
}

static int allocs = 0;
static int frees = 0;

parse_state *
empty_parse_state (void)
{
  parse_state *ret = xcalloc (1, sizeof (parse_state));
  ret->state_items.contents = gl_list_create_empty (GL_LINKED_LIST, NULL,
                                                    NULL, NULL, true);
  ret->derivs.contents = gl_list_create_empty (GL_LINKED_LIST, NULL,
                                               NULL, NULL, true);
  ++allocs;
  return ret;
}

parse_state *
copy_parse_state (bool prepend, parse_state *parent)
{
  parse_state *ret = xmalloc (sizeof (parse_state));
  memcpy (ret, parent, sizeof (parse_state));
  ret->state_items.contents = gl_list_create_empty (GL_LINKED_LIST, NULL,
                                                    NULL, NULL, true);
  ret->derivs.contents = gl_list_create_empty (GL_LINKED_LIST, NULL,
                                               NULL, NULL, true);
  ret->parent = parent;
  ret->prepend = prepend;
  ret->reference_count = 0;
  ret->visited = false;
  ++parent->reference_count;
  ++allocs;
  return ret;
}

parse_state *
new_parse_state (gl_list_t sis, gl_list_t derivs, bool prepend,
                 parse_state *parent)
{
  parse_state *ret = xmalloc (sizeof (parse_state));
  memcpy (ret, parent, sizeof (parse_state));

  ret->state_items.contents = sis;
  if (sis)
    {
      size_t size = gl_list_size (sis);
      ret->state_items.total_size += size;
      if (size > 0)
        {
          if (prepend || ret->state_items.head_elt == NULL)
            ret->state_items.head_elt = gl_list_get_at (sis, 0);
          if (!prepend || ret->state_items.tail_elt == NULL)
            ret->state_items.tail_elt = gl_list_get_at (sis, size - 1);
        }
    }
  ret->derivs.contents = derivs;
  if (derivs)
    {
      size_t size = gl_list_size (derivs);
      ret->derivs.total_size += size;
      if (size > 0)
        {
          if (prepend || ret->derivs.head_elt == NULL)
            ret->derivs.head_elt = gl_list_get_at (sis, 0);
          if (!prepend || ret->derivs.tail_elt == NULL)
            ret->derivs.tail_elt = gl_list_get_at (sis, size - 1);
        }
    }
  ret->parent = parent;
  ret->prepend = prepend;
  ret->reference_count = 0;
  ret->visited = false;
  ++parent->reference_count;
  ++allocs;
  return ret;
}

void
free_parse_state (parse_state *ps)
{
  if (ps == NULL)
    return;
  --ps->reference_count;
  // need to keep the parse state around
  // for visited, but its contents can be freed
  if ((ps->reference_count == 1 && ps->visited) ||
      (ps->reference_count == 0 && !ps->visited))
    {
      if (ps->state_items.contents)
        gl_list_free (ps->state_items.contents);
      if (ps->derivs.contents)
        gl_list_free (ps->derivs.contents);
      free_parse_state (ps->parent);
    }
  if (ps->reference_count == 0)
    {
      free (ps);
      ++frees;
    }

}

// takes an array of n gl_lists and flattens them into two list
// based off of the index split
static void
list_flatten_and_split (gl_list_t *l, gl_list_t *rets, int split, int n)
{
  int ret_index = 0;
  int ret_array = 0;
  for (int i = 0; i < n; ++i)
    {
      gl_list_iterator_t it = gl_list_iterator (l[i]);
      gl_list_t l;
      while (gl_list_iterator_next (&it, (const void **) &l, NULL))
        {
          if (!l)
            continue;
          gl_list_iterator_t it2 = gl_list_iterator (l);
          void *si;
          while (gl_list_iterator_next (&it2, (const void **) &si, NULL))
            {
              if (ret_index++ == split)
                ++ret_array;
              if (rets[ret_array])
                gl_list_add_last (rets[ret_array], si);
            }
        }
    }
}

// Emulates a reduction on a parse state by popping some amount of
// derivations and state_items off of the parse_state and returning
// the result in ret. Returns the derivation of what's popped.
gl_list_t
parser_pop (parse_state *ps, int deriv_index,
            int si_index, parse_state *ret)
{
  // prepend sis, append sis, prepend derivs, append derivs
  gl_list_t chunks[4];
  for (int i = 0; i < 4; ++i)
    chunks[i] = gl_list_create_empty (GL_LINKED_LIST, NULL, NULL, NULL, 1);
  for (parse_state *pn = ps; pn != NULL; pn = pn->parent)
    {
      if (pn->prepend)
        {
          gl_list_add_last (chunks[0], pn->state_items.contents);
          gl_list_add_last (chunks[2], pn->derivs.contents);
        }
      else
        {
          gl_list_add_first (chunks[1], pn->state_items.contents);
          gl_list_add_first (chunks[3], pn->derivs.contents);
        }
    }
  gl_list_t popped_derivs =
    gl_list_create_empty (GL_LINKED_LIST, NULL, NULL, NULL, 1);
  gl_list_t ret_chunks[4] = { ret->state_items.contents, NULL,
    ret->derivs.contents, popped_derivs
  };
  list_flatten_and_split (chunks, ret_chunks, si_index, 2);
  list_flatten_and_split (chunks + 2, ret_chunks + 2, deriv_index, 2);
  size_t s_size = gl_list_size (ret->state_items.contents);
  ret->state_items.total_size = s_size;
  if (s_size > 0)
    {
      ret->state_items.tail_elt = gl_list_get_at (ret->state_items.contents,
                                                  s_size - 1);
      ret->state_items.head_elt =
        gl_list_get_at (ret->state_items.contents, 0);
    }
  else
    {
      ret->state_items.tail_elt = NULL;
      ret->state_items.head_elt = NULL;
    }
  size_t d_size = gl_list_size (ret->derivs.contents);
  ret->derivs.total_size = d_size;
  if (d_size > 0)
    {
      ret->derivs.tail_elt = gl_list_get_at (ret->derivs.contents,
                                             d_size - 1);
      ret->derivs.head_elt = gl_list_get_at (ret->derivs.contents, 0);
    }
  else
    {
      ret->derivs.tail_elt = NULL;
      ret->derivs.head_elt = NULL;
    }
  for (int i = 0; i < 4; ++i)
    gl_list_free (chunks[i]);
  return popped_derivs;
}


/**
 * Compute the parse states that result from taking a transition on
 * nullable symbols whenever possible from the given state_item.
 */
void
nullable_closure (parse_state *ps, state_item *si, gl_list_t states)
{
  parse_state *current_ps = ps;
  state_item_number prev_sin = si - state_items;
  for (state_item_number sin = trans[prev_sin];
       sin != -1; prev_sin = sin, sin = trans[sin])
    {
      state_item *psi = state_items + prev_sin;
      symbol_number sp = item_number_as_symbol_number (*psi->item);
      if (ISTOKEN (sp) || !nullable[sp - ntokens])
        break;

      state_item *nsi = state_items + sin;
      current_ps = copy_parse_state (false, current_ps);
      ps_chunk_append (&current_ps->state_items, nsi);
      ps_chunk_append (&current_ps->derivs, derivation_new (sp, NULL));
      gl_list_add_last (states, current_ps);
    }
}

gl_list_t
simulate_transition (parse_state *ps)
{
  const state_item *si = ps->state_items.tail_elt;
  symbol_number sym = item_number_as_symbol_number (*si->item);
  // Transition on the same next symbol, taking nullable
  // symbols into account.
  gl_list_t result =
    gl_list_create_empty (GL_LINKED_LIST, NULL, NULL, NULL, true);
  state_item_number si_next = trans[si - state_items];
  // check for disabled transition, shouldn't happen
  // as any state_items that lead to these should be
  // disabled.
  if (si_next < 0)
    return result;
  parse_state *next_ps = copy_parse_state (false, ps);
  ps_chunk_append (&next_ps->state_items, state_items + si_next);
  ps_chunk_append (&next_ps->derivs, derivation_new (sym, NULL));
  gl_list_add_last (result, next_ps);

  nullable_closure (next_ps, state_items + si_next, result);
  return result;
}

/**
 * Determine if the given symbols are equal or their first sets
 * intersect.
 */
static bool
compatible (symbol_number sym1, symbol_number sym2)
{
  if (sym1 == sym2)
    return true;
  if (ISTOKEN (sym1) && ISVAR (sym2))
    return bitset_test (tfirsts[sym2 - ntokens], sym1);
  else if (ISVAR (sym1) && ISTOKEN (sym2))
    return bitset_test (tfirsts[sym1 - ntokens], sym2);
  else if (ISVAR (sym1) && ISVAR (sym2))
    return !bitset_disjoint_p (tfirsts[sym1 - ntokens],
                               tfirsts[sym2 - ntokens]);
  else
    return false;
}

gl_list_t
simulate_production (parse_state *ps, symbol_number compat_sym)
{
  gl_list_t result =
    gl_list_create_empty (GL_LINKED_LIST, NULL, NULL, NULL, true);
  state_item *si = ps->state_items.tail_elt;
  bitset prod = prods_lookup (si - state_items);
  if (prod)
    {
      bitset_iterator biter;
      state_item_number sin;
      BITSET_FOR_EACH (biter, prod, sin, 0)
        {
          // Take production step only if lhs is not nullable and
          // if first rhs symbol is compatible with compat_sym
          state_item *next = state_items + sin;
          item_number *itm1 = next->item;
          if (!compatible (*itm1, compat_sym) || !production_allowed (si, next))
            continue;
          parse_state *next_ps = copy_parse_state (false, ps);
          ps_chunk_append (&next_ps->state_items, next);
          gl_list_add_last (result, next_ps);
          if (next_ps->depth >= 0)
            ++next_ps->depth;
          nullable_closure (next_ps, next, result);
        }
    }
  return result;
}

// simulates a reduction on the given parse state, conflict_item is the
// item associated with ps's conflict. symbol_set is a lookahead set this
// reduction must be compatible with
gl_list_t
simulate_reduction (parse_state *ps, item_number *conflict_item,
                    int rule_len, bitset symbol_set)
{
  gl_list_t result =
    gl_list_create_empty (GL_LINKED_LIST, NULL, NULL, NULL, true);

  int s_size = ps->state_items.total_size;
  int d_size = ps->derivs.total_size;
  parse_state *new_root = empty_parse_state ();
  gl_list_t popped_derivs = parser_pop (ps, d_size - rule_len,
                                        s_size - rule_len - 1,
                                        new_root);

  // update derivation
  state_item *si = (state_item *) ps->state_items.tail_elt;
  const rule *r = item_rule (si->item);
  symbol_number lhs = r->lhs->number;
  derivation *deriv = derivation_new (lhs, popped_derivs);
  if (ps->depth == 0)
    {
      int dot_pos = 0;
      for (item_number *i = conflict_item - 1;
           item_number_is_symbol_number (*i) && i > ritem; --i)
        ++dot_pos;
      gl_list_add_at (deriv->children, dot_pos, derivation_dot ());
    }
  --new_root->depth;
  ps_chunk_append (&new_root->derivs, deriv);

  if (s_size != rule_len + 1)
    {
      state_item *tail = (state_item *) new_root->state_items.tail_elt;
      ps_chunk_append (&new_root->state_items,
                       state_items + trans[tail - state_items]);
      gl_list_add_last (result, new_root);
    }
  else
    {
      // The head state_item is a production item, so we need to prepend
      // with possible source state_item.
      const state_item *head = ps->state_items.head_elt;
      gl_list_t prev =
        lssi_reverse_production (head - state_items, symbol_set);
      gl_list_iterator_t it = gl_list_iterator (prev);
      gl_list_t psis;
      while (gl_list_iterator_next (&it, (const void **) &psis, NULL))
        {
          //Prepend the results from the reverse production
          parse_state *p_state = new_parse_state (psis, NULL, true, new_root);

          // Append the left hand side to the end of the parser state
          parse_state *copy = copy_parse_state (false, p_state);
          ps_chunk *sis = &copy->state_items;
          const state_item *tail = sis->tail_elt;
          ps_chunk_append (sis, state_items + trans[tail - state_items]);
          gl_list_add_last (result, copy);
          nullable_closure (copy, (state_item *) sis->tail_elt, result);
        }
      gl_list_free (prev);
    }
  return result;
}

void
print_parse_state (parse_state *ps)
{
  printf ("(size %zu depth %d rc %d)\n",
          ps->state_items.total_size, ps->depth, ps->reference_count);
  print_state_item (ps->state_items.head_elt, stdout);
  print_state_item (ps->state_items.tail_elt, stdout);
  if (ps->derivs.total_size > 0)
    derivation_print (ps->derivs.head_elt, stdout);
  putc ('\n', stdout);
}