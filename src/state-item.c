/* Counterexample Generation Search Nodes
 
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
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <gl_linked_list.h>
#include <gl_xlist.h>
#include "state-item.h"
#include "closure.h"
#include "nullable.h"
#include "getargs.h"

size_t nstate_items;
state_item_number *state_item_map;
state_item *state_items;

state_item_number *trans;
bitsetv rev_trans;
Hash_table *prods;
Hash_table *rev_prods;


// hash functions for index -> bitset hash maps
typedef struct
{
  int key;
  bitset l;
} hash_pair;

static size_t
hash_pair_hasher (const hash_pair *sl, size_t max)
{
  return sl->key % max;
}

static bool
hash_pair_comparator (const hash_pair *l, const hash_pair *r)
{
  return l->key == r->key;
}

static void
hash_pair_free (hash_pair *hp)
{
  bitset_free (hp->l);
  free (hp);
}

static bitset
hash_pair_lookup (Hash_table *tab, int key)
{
  hash_pair *l = xmalloc (sizeof (hash_pair));
  l->key = key;
  hash_pair *hp = (hash_pair *) hash_lookup (tab, l);
  if (!hp)
    return NULL;
  return hp->l;
}

static void
hash_pair_insert (Hash_table *tab, int key, bitset val)
{
  hash_pair *hp = xmalloc (sizeof (hash_pair));
  hp->key = key;
  hp->l = val;
  if (!hash_insert (tab, hp))
    xalloc_die ();
}

static void
hash_pair_remove (Hash_table *tab, int key)
{
  hash_pair *hp = xmalloc (sizeof (hash_pair));
  hp->key = key;
  hash_delete (tab, hp);
}

/* return a state_item from a state's id and the offset of the item
   within the state.
 */
state_item *
state_item_lookup (state_number s, state_item_number off)
{
  return &state_items[state_item_index_lookup (s, off)];
}

static inline void
state_item_set (state_item_number sidx, state *s, item_number off)
{
  state_item *si = state_items + sidx;
  si->state = s;
  si->item = &ritem[off];
  si->lookahead = NULL;
  trans[sidx] = -1;
}

/**
 * Initialize state_items set
 */
static void
init_state_items ()
{
  nstate_items = 0;
  bitsetv production_items = bitsetv_create (nstates, nritems, BITSET_SPARSE);
  for (int i = 0; i < nstates; ++i)
    {
      state *s = states[i];
      nstate_items += s->nitems;
      closure (s->items, s->nitems);
      for (size_t j = 0; j < nitemset; ++j)
        if (itemset[j] > 0
            && item_number_is_rule_number (ritem[itemset[j] - 1]))
          {
            bitset_set (production_items[i], itemset[j]);
            ++nstate_items;
          }
    }
  state_item_map = xnmalloc (nstates + 1, sizeof (state_item_number));
  state_items = xnmalloc (nstate_items, sizeof (state_item));
  trans = xnmalloc (nstate_items, sizeof (state_item_number));
  rev_trans = bitsetv_create (nstate_items, nstate_items, BITSET_SPARSE);
  state_item_number sidx = 0;
  for (int i = 0; i < nstates; ++i)
    {
      state_item_map[i] = sidx;
      int rule_search_idx = 0;
      state *s = states[i];
      reductions *red = s->reductions;
      for (int j = 0; j < s->nitems; ++j)
        {
          state_item_set (sidx, s, s->items[j]);
          state_item *si = state_items + sidx;
          const rule *r = item_rule (si->item);
          if (red->rules[rule_search_idx] < r)
            ++rule_search_idx;
          if (rule_search_idx < red->num && r == red->rules[rule_search_idx])
            {
              bitsetv lookahead = red->lookahead_tokens;
              if (lookahead)
                si->lookahead = lookahead[rule_search_idx];
            }
          ++sidx;
        }
      bitset_iterator biter;
      item_number off;
      BITSET_FOR_EACH (biter, production_items[i], off, 0)
        {
          state_item_set (sidx, s, off);
          if (item_number_is_rule_number (ritem[off]))
            {
              bitsetv lookahead = red->lookahead_tokens;
              if (lookahead)
                state_items[sidx].lookahead = lookahead[rule_search_idx];
              ++rule_search_idx;
            }
          ++sidx;
        }

    }
  state_item_map[nstates] = nstate_items;
}

static size_t
state_sym_hasher (const void *st, size_t max)
{
  return ((state *) st)->accessing_symbol % max;
}

static bool
state_sym_comparator (const void *s1, const void *s2)
{
  return ((state *) s1)->accessing_symbol == ((state *) s2)->accessing_symbol;
}

static state *
state_sym_lookup (symbol_number sym, Hash_table *h)
{
  state *s = xmalloc (sizeof (state *));
  s->accessing_symbol = sym;
  return hash_lookup (h, s);
}

/**
 * Initialize trans and revTrans maps.
 */
static void
init_trans ()
{
  for (state_number i = 0; i < nstates; ++i)
    {
      state *s = states[i];
      transitions *t = s->transitions;
      Hash_table *transitions
        = hash_initialize (t->num, NULL, (Hash_hasher) state_sym_hasher,
                           (Hash_comparator) state_sym_comparator, NULL);
      for (int j = 0; j < t->num; ++j)
        if (!TRANSITION_IS_DISABLED (t, j))
          if (!hash_insert (transitions, t->states[j]))
            xalloc_die ();
      for (int j = state_item_map[i]; j < state_item_map[i + 1]; ++j)
        {
          item_number *item = state_items[j].item;
          if (item_number_is_rule_number (*item))
            continue;
          state *dst = state_sym_lookup (*item, transitions);
          if (!dst)
            continue;
          for (int k = 0; k < dst->nitems; ++k)
            {
              if (item + 1 == ritem + dst->items[k])
                {
                  // We have found the target item after transition on sym
                  // from the source item.
                  state_item_number dstSI =
                    state_item_index_lookup (dst->number, k);

                  trans[j] = dstSI;
                  bitset_set (rev_trans[dstSI], j);
                  break;
                }
            }
        }
    }
}

bitset
prods_lookup (state_item_number si)
{
  return hash_pair_lookup (prods, si);
}

bitset
rev_prods_lookup (state_item_number si)
{
  return hash_pair_lookup (rev_prods, si);
}

/**
 * Initialize prods and revProds maps.
 */
static void
init_prods ()
{
  prods = hash_initialize (nstate_items,
                           NULL,
                           (Hash_hasher) hash_pair_hasher,
                           (Hash_comparator) hash_pair_comparator,
                           (Hash_data_freer) hash_pair_free);
  rev_prods = hash_initialize (nstate_items,
                               NULL,
                               (Hash_hasher) hash_pair_hasher,
                               (Hash_comparator) hash_pair_comparator,
                               (Hash_data_freer) hash_pair_free);
  for (int i = 0; i < nstates; ++i)
    {
      state *state = states[i];
      // closure_map is a hash map from nonterminals to a set
      // of the items that produce those nonterminals
      Hash_table *closure_map
        = hash_initialize (nsyms - ntokens, NULL,
                           (Hash_hasher) hash_pair_hasher,
                           (Hash_comparator) hash_pair_comparator,
                           NULL);

      // Add the nitems of state to skip to the production portion
      // of that state's state_items
      for (int j = state_item_map[i] + state->nitems;
           j < state_item_map[i + 1]; ++j)
        {
          state_item *src = state_items + j;
          item_number *item = src->item;
          symbol_number lhs = item_rule (item)->lhs->number;
          bitset itms = hash_pair_lookup (closure_map, lhs);
          if (!itms)
            {
              itms = bitset_create (nstate_items, BITSET_SPARSE);
              hash_pair_insert (closure_map, lhs, itms);
            }
          bitset_set (itms, j);
        }
      // For each item with a dot followed by a nonterminal,
      // try to create a production edge.
      for (int j = state_item_map[i]; j < state_item_map[i + 1]; ++j)
        {
          state_item *src = state_items + j;
          item_number item = *(src->item);
          // Skip reduce items and items with terminals after the dot
          if (item_number_is_rule_number (item) || ISTOKEN (item))
            continue;
          symbol_number sym = item_number_as_symbol_number (item);
          bitset lb = hash_pair_lookup (closure_map, sym);
          if (lb)
            {
              hash_pair *prod_hp = xmalloc (sizeof (hash_pair));
              prod_hp->key = j;
              prod_hp->l = lb;
              //update prods
              if (!hash_insert (prods, prod_hp))
                xalloc_die ();

              //update rev_prods
              bitset_iterator biter;
              state_item_number prod;
              BITSET_FOR_EACH (biter, lb, prod, 0)
                {
                  bitset rev_itms = rev_prods_lookup (prod);
                  if (!rev_itms)
                    {
                      rev_itms = bitset_create (nstate_items, BITSET_SPARSE);
                      hash_pair_insert (rev_prods, prod, rev_itms);
                    }
                  bitset_set (rev_itms, j);
                }
            }
        }

    }
}

/* Since lookaheads are only generated for reductions,
   we need to propogate lookahead sets backwards as
   the searches require each state_item to have a lookahead.
 */
static inline void
gen_lookaheads ()
{
  for (state_item_number i = 0; i < nstate_items; ++i)
    {
      state_item *si = state_items + i;
      if (item_number_is_symbol_number (*(si->item)) || !si->lookahead)
        continue;

      bitset lookahead = si->lookahead;
      gl_list_t queue =
        gl_list_create (GL_LINKED_LIST, NULL, NULL, NULL, true, 1,
                        (const void **) &si);

      // For each reduction item, traverse through all state_items
      // accessible through reverse transition steps, and set their
      // lookaheads to the reduction items lookahead
      while (gl_list_size (queue) > 0)
        {
          state_item *prev = gl_list_get_at (queue, 0);
          gl_list_remove_at (queue, 0);
          prev->lookahead = lookahead;
          bitset rsi = rev_trans[prev - state_items];
          bitset_iterator biter;
          state_item_number sin;
          BITSET_FOR_EACH (biter, rsi, sin, 0)
            gl_list_add_first (queue, &state_items[sin]);
        }
    }
}

bitsetv tfirsts = NULL;
void
init_tfirsts (void)
{
  tfirsts = bitsetv_create (nvars, ntokens, BITSET_FIXED);
  for (int i = 0; i < nrules; ++i)
    {
      rule *r = rules + i;
      if (item_number_is_symbol_number (*(r->rhs)))
        {
          symbol_number term = item_number_as_symbol_number (*(r->rhs));
          if (ISVAR (term))
            continue;
          symbol_number lhs = r->lhs->number;

          for (int j = 0; j < nvars; ++j)
            if (bitset_test (firsts[j], lhs - ntokens))
              bitset_set (tfirsts[j], term);
        }
    }
}

static inline void
disable_state_item (state_item_number sin)
{
  trans[sin] = -2;
  hash_pair *l = xmalloc (sizeof (hash_pair));
  hash_pair_remove (prods, sin);
  hash_pair_remove (rev_prods, sin);
}

/*
 To make searches more efficient, we can prune away paths that are
 caused by disabled transitions.
 */
static void
prune_disabled_paths (void)
{
  for (int i = nstate_items - 1; i >= 0; --i)
    {
      state_item *si = state_items + i;
      if (trans[i] == -1 && item_number_is_symbol_number (*si->item))
        {
          gl_list_t queue =
            gl_list_create (GL_LINKED_LIST, NULL, NULL, NULL, true, 1,
                            (const void **) &si);

          // For each disabled transition, traverse through all state_items
          // accessible through reverse transition steps, and set their
          // lookaheads to the reduction items lookahead
          while (gl_list_size (queue) > 0)
            {
              state_item *prev = gl_list_get_at (queue, 0);
              gl_list_remove_at (queue, 0);
              state_item_number prev_num = prev - state_items;
              disable_state_item (prev_num);

              bitset rsi = rev_trans[prev_num];
              bitset_iterator biter;
              state_item_number sin;
              BITSET_FOR_EACH (biter, rsi, sin, 0)
                gl_list_add_first (queue, &state_items[sin]);
            }
        }
    }
}

void
print_state_item (state_item *si, FILE *out)
{
  fprintf (out, "%d:", si->state->number);
  item_print (si->item, NULL, out);
  if (si->lookahead)
    {
      fputs (" {", out);
      bitset_iterator biter;
      symbol_number sin;
      int i = 0;
      BITSET_FOR_EACH (biter, si->lookahead, sin, 0)
        {
          symbol_print (symbols[sin], out);
          fputs (", ", out);
          ++i;
          if (i >= 3)
            {
              fputs ("...", out);
              goto biter_exit;
            }
        }
    biter_exit:
      putc ('}', out);
    }
  putc ('\n', out);
}

/**
 * Report set counts and the state_item graph if trace is enabled
 */
void
state_items_report (FILE *out)
{
  state_items_init (out);
  fprintf (out, "# state items: %zu\n", nstate_items);

  size_t count = 0;
  for (state_item_number si = 0; si < nstate_items; ++si)
    if (trans[si] != -1)
      ++count;

  fprintf (out, "# transitions: %zu\n", count);

  count = 0;
  for (hash_pair *hp = (hash_pair *) hash_get_first (prods);
       hp != NULL; hp = hash_get_next (prods, hp))
    count += bitset_count (hp->l);
  fprintf (out, "# productions: %zu\n", count);

  count = 0;
  for (state_item_number si = 0; si < nstate_items; ++si)
    count += bitset_count (rev_trans[si]);
  fprintf (out, "# reverse transitions: %zu\n", count);

  count = 0;
  for (hash_pair *hp = (hash_pair *) hash_get_first (rev_prods);
       hp != NULL; hp = hash_get_next (rev_prods, hp))
    count += bitset_count (hp->l);
  fprintf (out, "# reverse productions: %zu\n", count);

  // Graph printing
  if (trace_flag & trace_cex)
    {
      for (state_number i = 0; i < nstates; ++i)
        {
          fprintf (out, "State %d:\n", i);
          for (int j = state_item_map[i]; j < state_item_map[i + 1]; ++j)
            {
              item_print (state_items[j].item, NULL, out);
              putc ('\n', out);
              if (trans[j] >= 0)
                {
                  fputs ("    -> ", out);
                  print_state_item (state_items + trans[j], out);
                }

              bitset sets[3] =
                { rev_trans[j], prods_lookup (j), rev_prods_lookup (j) };
              const char *txt[3] = { "    <- ", "    => ", "    <= " };
              for (int seti = 0; seti < 3; ++seti)
                {
                  bitset b = sets[seti];
                  if (b)
                    {
                      bitset_iterator biter;
                      state_item_number sin;
                      BITSET_FOR_EACH (biter, b, sin, 0)
                        {
                          fputs (txt[seti], out);
                          print_state_item (state_items + sin, out);
                        }
                    }
                }
              putc ('\n', out);
            }
        }
      fprintf (out, "TOKEN FIRSTS\n");
      for (symbol_number i = ntokens; i < nsyms; ++i)
        {
          fprintf (out, "  %s firsts\n", symbols[i]->tag);
          bitset_iterator iter;
          symbol_number j;
          BITSET_FOR_EACH (iter, tfirsts[i - ntokens], j, 0)
            fprintf (out, "    %s\n", symbols[j]->tag);
        }
      fprintf (out, "\n\n");
    }
}


static bool s_initialized = false;

void
state_items_init (FILE *report)
{
  if (!s_initialized)
    {
      s_initialized = true;
      time_t start = time (NULL);
      init_state_items ();
      init_trans ();
      init_prods ();
      gen_lookaheads ();
      init_tfirsts ();
      prune_disabled_paths ();
      if (report)
        {
          fprintf (report, "init: %f\n", difftime (time (NULL), start));
          state_items_report (report);
        }
    }
}

void
state_items_free (void)
{
  if (s_initialized)
    {
      hash_free (rev_prods);
      hash_free (prods);
      bitsetv_free (rev_trans);
      free (trans);
      free (state_items);
      bitsetv_free (tfirsts);
    }
}

/**
 * Determine, using precedence and associativity, whether the next
 * production is allowed from the current production.
 */
bool
production_allowed (state_item *si, state_item *next)
{
  sym_content *s1 = item_rule (si->item)->lhs;
  sym_content *s2 = item_rule (next->item)->lhs;
  int prec1 = s1->prec;
  int prec2 = s2->prec;
  if (prec1 >= 0 && prec2 >= 0)
    {
      // Do not expand if lower precedence.
      if (prec1 > prec2)
        return false;
      // Do not expand if same precedence, but left-associative.
      if (prec1 == prec2 && s1->assoc == left_assoc)
        return false;
    }
    return true;
}