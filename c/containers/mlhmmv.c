// ================================================================
// Array-only (open addressing) multi-level hash map, with linear probing for collisions.
// All keys, and terminal-level values, are mlrvals.
//
// Notes:
// * null key is not supported.
// * null value is not supported.
//
// See also:
// * http://en.wikipedia.org/wiki/Hash_table
// * http://docs.oracle.com/javase/6/docs/api/java/util/Map.html
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "containers/mlhmmv.h"

static mlhmmv_level_t* mlhmmv_level_alloc();
static void            mlhmmv_level_init(mlhmmv_level_t *plevel, int length);
static void            mlhmmv_level_free(mlhmmv_level_t* plevel);

static int mlhmmv_level_find_index_for_key(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pideal_index);

static void mlhmmv_level_put_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value);
static void mlhmmv_level_enlarge(mlhmmv_level_t* plevel);
static void mlhmmv_level_move(mlhmmv_level_t* plevel, mv_t* plevel_key, mlhmmv_level_value_t* plevel_value);

static mlhmmv_level_t* mlhmmv_get_or_create_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys);
static mlhmmv_level_t* mlhmmv_get_or_create_level_aux_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys);

static mlhmmv_level_entry_t* mlhmmv_get_next_level_entry(mlhmmv_level_t* pmap, mv_t* plevel_key, int* pindex);

static void mlhmmv_to_lrecs_aux(mlhmmv_level_t* plevel, mv_t* pfirstname, sllmve_t* prestnames,
	lrec_t* ptemplate, sllv_t* poutrecs);
static void mlhmmv_to_lrecs_aux_flatten(mlhmmv_level_t* plevel, char* prefix, lrec_t* ptemplate, sllv_t* poutrecs);

static void mlhmmv_level_print_stacked(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always);
static void mlhmmv_level_print_single_line(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always);

static void json_decimal_print(char* s);

static int mlhmmv_hash_func(mv_t* plevel_key);

// ----------------------------------------------------------------
// Allow compile-time override, e.g using gcc -D.

#ifndef LOAD_FACTOR
#define LOAD_FACTOR          0.7
#endif

#ifndef ENLARGEMENT_FACTOR
#define ENLARGEMENT_FACTOR   2
#endif

#define OCCUPIED             0xa4
#define DELETED              0xb8
#define EMPTY                0xce

// ----------------------------------------------------------------
mlhmmv_t* mlhmmv_alloc() {
	mlhmmv_t* pmap = mlr_malloc_or_die(sizeof(mlhmmv_t));
	pmap->proot_level = mlhmmv_level_alloc();
	return pmap;
}

static mlhmmv_level_t* mlhmmv_level_alloc() {
	mlhmmv_level_t* plevel = mlr_malloc_or_die(sizeof(mlhmmv_level_t));
	mlhmmv_level_init(plevel, MLHMMV_INITIAL_ARRAY_LENGTH);
	return plevel;
}

static void mlhmmv_level_init(mlhmmv_level_t *plevel, int length) {
	plevel->num_occupied = 0;
	plevel->num_freed    = 0;
	plevel->array_length = length;

	plevel->entries      = (mlhmmv_level_entry_t*)mlr_malloc_or_die(sizeof(mlhmmv_level_entry_t) * length);
	// Don't do mlhmmv_level_entry_clear() of all entries at init time, since this has a
	// drastic effect on the time needed to construct an empty map (and miller
	// constructs an awful lot of those). The attributes there are don't-cares
	// if the corresponding entry state is EMPTY. They are set on put, and
	// mutated on remove.

	plevel->states       = (mlhmmv_level_entry_state_t*)mlr_malloc_or_die(sizeof(mlhmmv_level_entry_state_t) * length);
	memset(plevel->states, EMPTY, length);

	plevel->phead        = NULL;
	plevel->ptail        = NULL;
}

// ----------------------------------------------------------------
void mlhmmv_free(mlhmmv_t* pmap) {
	if (pmap == NULL)
		return;
	mlhmmv_level_free(pmap->proot_level);
	free(pmap);
}

static void mlhmmv_level_free(mlhmmv_level_t* plevel) {
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		if (!pentry->level_value.is_terminal) {
			mlhmmv_level_free(pentry->level_value.u.pnext_level);
		}
		mv_free(&pentry->level_key);
	}
	free(plevel->entries);
	free(plevel->states);
	plevel->entries      = NULL;
	plevel->num_occupied = 0;
	plevel->num_freed    = 0;
	plevel->array_length = 0;

	free(plevel);
}

// ----------------------------------------------------------------
// Used by get() and remove().
// xxx s.b. >=0? if so fix all comments (lhm*).
// Returns >0 for where the key is *or* should go (end of chain).
static int mlhmmv_level_find_index_for_key(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pideal_index) {
	int hash = mlhmmv_hash_func(plevel_key);
	int index = mlr_canonical_mod(hash, plevel->array_length);
	*pideal_index = index;
	int num_tries = 0;

	while (TRUE) {
		mlhmmv_level_entry_t* pentry = &plevel->entries[index];
		if (plevel->states[index] == OCCUPIED) {
			mv_t* ekey = &pentry->level_key;
			// Existing key found in chain.
			if (mv_equals_si(plevel_key, ekey))
				return index;
		} else if (plevel->states[index] == EMPTY) {
			return index;
		}

		// If the current entry has been freed, i.e. previously occupied,
		// the sought index may be further down the chain.  So we must
		// continue looking.
		if (++num_tries >= plevel->array_length) {
			fprintf(stderr,
				"%s: Coding error:  table full even after enlargement.\n", MLR_GLOBALS.argv0);
			exit(1);
		}

		// Linear probing.
		if (++index >= plevel->array_length)
			index = 0;
	}
	fprintf(stderr, "%s: internal coding error detected in file %s at line %d.\n",
		MLR_GLOBALS.argv0, __FILE__, __LINE__);
	exit(1);
}

// ----------------------------------------------------------------
// Example: keys = ["a", 2, "c"] and value = 4.
void mlhmmv_put(mlhmmv_t* pmap, sllmv_t* pmvkeys, mv_t* pterminal_value) {
	mlhmmv_level_put(pmap->proot_level, pmvkeys->phead, pterminal_value);
}
// Example on recursive calls:
// * level = map, rest_keys = ["a", 2, "c"] , terminal value = 4.
// * level = map["a"], rest_keys = [2, "c"] , terminal value = 4.
// * level = map["a"][2], rest_keys = ["c"] , terminal value = 4.
void mlhmmv_level_put(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value) {
	if ((plevel->num_occupied + plevel->num_freed) >= (plevel->array_length * LOAD_FACTOR))
		mlhmmv_level_enlarge(plevel);
	mlhmmv_level_put_no_enlarge(plevel, prest_keys, pterminal_value);
}

static void mlhmmv_level_put_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value) {
	mv_t* plevel_key = &prest_keys->value;
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == EMPTY) { // End of chain.
		pentry->ideal_index = ideal_index;
		pentry->level_key = mv_copy(plevel_key);

		if (prest_keys->pnext == NULL) {
			pentry->level_value.is_terminal = TRUE;
			pentry->level_value.u.mlrval = mv_copy(pterminal_value);
		} else {
			pentry->level_value.is_terminal = FALSE;
			pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
		}
		plevel->states[index] = OCCUPIED;

		if (plevel->phead == NULL) { // First entry at this level
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {                     // Subsequent entry at this level
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}

		plevel->num_occupied++;
		if (prest_keys->pnext != NULL) {
			// RECURSE
			mlhmmv_level_put(pentry->level_value.u.pnext_level, prest_keys->pnext, pterminal_value);
		}

	} else if (plevel->states[index] == OCCUPIED) { // Existing key found in chain
		if (prest_keys->pnext == NULL) { // Place the terminal at this level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
			} else {
				mlhmmv_level_free(pentry->level_value.u.pnext_level);
			}
			pentry->level_value.is_terminal = TRUE;
			pentry->level_value.u.mlrval = mv_copy(pterminal_value);

		} else { // The terminal will be placed at a deeper level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
				pentry->level_value.is_terminal = FALSE;
				pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
			}
			// RECURSE
			mlhmmv_level_put(pentry->level_value.u.pnext_level, prest_keys->pnext, pterminal_value);
		}

	} else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.argv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
// This is done only on map-level enlargement.
// Example:
// * level = map["a"], rest_keys = [2, "c"] ,   terminal_value = 4.
//                     rest_keys = ["e", "f"] , terminal_value = 7.
//                     rest_keys = [6] ,        terminal_value = "g".
//
// which is to say for the purposes of this routine
//
// * level = map["a"], level_key = 2,   level_value = non-terminal ["c"] => terminal_value = 4.
//                     level_key = "e", level_value = non-terminal ["f"] => terminal_value = 7.
//                     level_key = 6,   level_value = terminal_value = "g".

static void mlhmmv_level_move(mlhmmv_level_t* plevel, mv_t* plevel_key, mlhmmv_level_value_t* plevel_value) {
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == OCCUPIED) {
		// Existing key found in chain; put value.
		pentry->level_value = *plevel_value;

	} else if (plevel->states[index] == EMPTY) {
		// End of chain.
		pentry->ideal_index = ideal_index;
		pentry->level_key = *plevel_key;
		// For the put API, we copy data passed in. But for internal enlarges, we just need to move pointers around.
		pentry->level_value = *plevel_value;
		plevel->states[index] = OCCUPIED;

		if (plevel->phead == NULL) {
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}
		plevel->num_occupied++;
	}
	else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.argv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
// xxx use mlhmmv_get_level for most of the work. then check is_terminal.
// xxx rename to mlhmmv_get_terminal
mv_t* mlhmmv_get(mlhmmv_t* pmap, sllmv_t* pmvkeys, int* perror) {
	*perror = MLHMMV_ERROR_NONE;
	sllmve_t* prest_keys = pmvkeys->phead;
	if (prest_keys == NULL) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
		return NULL;
	}
	mlhmmv_level_t* plevel = pmap->proot_level;
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_next_level_entry(plevel, &prest_keys->value, NULL);
	while (prest_keys->pnext != NULL) {
		if (plevel_entry == NULL) {
			return NULL;
		}
		if (plevel_entry->level_value.is_terminal) {
			*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
			return NULL;
		}
		plevel = plevel_entry->level_value.u.pnext_level;
		prest_keys = prest_keys->pnext;
		plevel_entry = mlhmmv_get_next_level_entry(plevel_entry->level_value.u.pnext_level, &prest_keys->value, NULL);
	}
	if (plevel_entry == NULL) {
		return NULL;
	}
	if (!plevel_entry->level_value.is_terminal) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
		return NULL;
	}
	return &plevel_entry->level_value.u.mlrval;
}

// ----------------------------------------------------------------
// xxx remove code duplication

// Example on recursive calls:
// * level = map, rest_keys = ["a", 2, "c"]
// * level = map["a"], rest_keys = [2, "c"]
// * level = map["a"][2], rest_keys = ["c"]
mlhmmv_level_t* mlhmmv_get_or_create_level(mlhmmv_t* pmap, sllmv_t* pmvkeys) {
	return mlhmmv_get_or_create_level_aux(pmap->proot_level, pmvkeys->phead);
}
static mlhmmv_level_t* mlhmmv_get_or_create_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys) {
	if ((plevel->num_occupied + plevel->num_freed) >= (plevel->array_length * LOAD_FACTOR))
		mlhmmv_level_enlarge(plevel);
	return mlhmmv_get_or_create_level_aux_no_enlarge(plevel, prest_keys);
}

static mlhmmv_level_t* mlhmmv_get_or_create_level_aux_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys) {
	mv_t* plevel_key = &prest_keys->value;
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == EMPTY) { // End of chain.

		plevel->states[index] = OCCUPIED;
		plevel->num_occupied++;
		pentry->ideal_index = ideal_index;
		pentry->level_key = mv_copy(plevel_key);
		pentry->level_value.is_terminal = FALSE;
		pentry->level_value.u.pnext_level = mlhmmv_level_alloc();

		if (plevel->phead == NULL) { // First entry at this level
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {                     // Subsequent entry at this level
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}

		if (prest_keys->pnext != NULL) {
			// RECURSE
			return mlhmmv_get_or_create_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext);
		} else {
			return pentry->level_value.u.pnext_level;
		}

	} else if (plevel->states[index] == OCCUPIED) { // Existing key found in chain

		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
			pentry->level_value.is_terminal = FALSE;
			pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
		}
		if (prest_keys->pnext == NULL) {
			return pentry->level_value.u.pnext_level;
		} else { // RECURSE
			return mlhmmv_get_or_create_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext);
		}

	} else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.argv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
// xxx rename to mlhmmv_get_non_terminal
// xxx factor out a common mlhmmv_get_level?
mlhmmv_level_t* mlhmmv_get_level(mlhmmv_t* pmap, sllmv_t* pmvkeys, int* perror) {
	*perror = MLHMMV_ERROR_NONE;
	sllmve_t* prest_keys = pmvkeys->phead;
	// xxx this is awkward, with same statements inside/outside the loop. re-arrange.
	if (prest_keys == NULL) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
	}
	mlhmmv_level_t* plevel = pmap->proot_level;
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_next_level_entry(plevel, &prest_keys->value, NULL);
	while (prest_keys->pnext != NULL) {
		if (plevel_entry == NULL) {
			return NULL;
		}
		if (plevel_entry->level_value.is_terminal) {
			*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
			return NULL;
		}
		plevel = plevel_entry->level_value.u.pnext_level;
		prest_keys = prest_keys->pnext;
		plevel_entry = mlhmmv_get_next_level_entry(plevel_entry->level_value.u.pnext_level, &prest_keys->value, NULL);
	}
	if (plevel_entry->level_value.is_terminal) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
		return NULL;
	}
	return plevel_entry->level_value.u.pnext_level;
}

static mlhmmv_level_entry_t* mlhmmv_get_next_level_entry(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pindex) {
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (pindex != NULL)
		*pindex = index;

	if (plevel->states[index] == OCCUPIED)
		return pentry;
	else if (plevel->states[index] == EMPTY)
		return NULL;
	else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.argv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
// xxx cmt re remove from here on down

// xxx reg_test/run all permutations of s/t/u

// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @s      ; dump}' # ok

// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @t      ; dump}' # ok
// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @t[1]   ; dump}' # <-- not ok

// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @u      ; dump}' # ok
// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @u[1]   ; dump}' # <-- not ok
// echo a=1,b=2,x=3 | mlr put -q '@s=$x; @t[$a]=$x;@u[$a][$b]=$x; end{dump; unset @u[1][2]; dump}' # <-- not ok

// xxx make this recursive. if e.g. a=>b=>c=>4 and remove c level then all up-nodes are emptied out & should be pruned.
// so: recurse inward until end of pmvkeys list; then return to each caller whether the current level is now empty.

// echo s=a,t=b,u=c,x=9 | mlr put -q '@v[$s][$t][$u]=$x; end{dump; unset @v["a"]["b"]; dump;emit @v}'

// restkeys too long: do nothing
// restkeys just right: remove the terminal mlrval
// restkeys too short: remove the level and all below
// xxx recurse with parent level in
// xxx recurse with level-is-now-empty out
// xxx rm depth
static void mlhmmv_remove_aux(mlhmmv_level_t* plevel, sllmve_t* prestkeys, int* pemptied, int depth) {
	*pemptied = FALSE;

	if (prestkeys == NULL) // restkeys too short // xxx maybe abend
		return;

	int index = -1;
	mlhmmv_level_entry_t* pentry = mlhmmv_get_next_level_entry(plevel, &prestkeys->value, &index);
	if (pentry == NULL)
		return;

	if (prestkeys->pnext != NULL) {
		// Keep recursing until end of restkeys.
		if (pentry->level_value.is_terminal) // restkeys too long
			return;
		int descendant_emptied = FALSE;
		mlhmmv_remove_aux(pentry->level_value.u.pnext_level, prestkeys->pnext, &descendant_emptied, depth+1);

		// If the recursive call emptied the next-level slot, remove it from our level as well. This may continue all
		// the way back up. Example: the map is '{"a":{"b":{"c":4}}}' and we're asked to remove keylist ["a", "b", "c"].
		// The recursive call to the terminal will leave '{"a":{"b":{}}}' -- note the '{}'. Then we remove
		// that to leave '{"a":{}}'. Since this leaves another '{}', passing emptied==TRUE back to our caller
		// leaves empty top-level map '{}'.
		if (descendant_emptied) {
			plevel->num_occupied--;
			plevel->num_freed++;
			plevel->states[index] = DELETED;
			pentry->ideal_index = -1;
			pentry->level_key = mv_error();

			if (pentry == plevel->phead) {
				if (pentry == plevel->ptail) {
					plevel->phead = NULL;
					plevel->ptail = NULL;
					*pemptied = TRUE;
				} else {
					plevel->phead = pentry->pnext;
					pentry->pnext->pprev = NULL;
				}
			} else if (pentry == plevel->ptail) {
					plevel->ptail = pentry->pprev;
					pentry->pprev->pnext = NULL;
			} else {
				pentry->pprev->pnext = pentry->pnext;
				pentry->pnext->pprev = pentry->pprev;
			}
		}

	} else {
		// End of restkeys. Deletion & free logic goes here. Set *pemptied if the level was emptied out.

		// 1. Excise the node and its descendants from the storage tree
		if (plevel->states[index] != OCCUPIED) {
			fprintf(stderr, "%s: mlhmmv_remove: did not find end of chain.\n", MLR_GLOBALS.argv0);
			exit(1);
		}

		// xxx
		// echo 'a=1,b=2,x=9' | mlr put -q @s=$x; @t[$a]=$x; @u[$a][$b]=$x; end{dump; unset @u[1][2]; dump}
		// still not right.

		pentry->ideal_index = -1;
		plevel->states[index] = DELETED;

		if (pentry == plevel->phead) {
			if (pentry == plevel->ptail) {
				plevel->phead = NULL;
				plevel->ptail = NULL;
				*pemptied = TRUE;
			} else {
				plevel->phead = pentry->pnext;
				pentry->pnext->pprev = NULL;
			}
		} else if (pentry == plevel->ptail) {
				plevel->ptail = pentry->pprev;
				pentry->pprev->pnext = NULL;
		} else {
			pentry->pprev->pnext = pentry->pnext;
			pentry->pnext->pprev = pentry->pprev;
		}

		plevel->num_freed++;
		plevel->num_occupied--;

		// 2. Free the memory for the node and its descendants
		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
		} else {
			mlhmmv_level_free(pentry->level_value.u.pnext_level);
		}
	}

}

void mlhmmv_remove(mlhmmv_t* pmap, sllmv_t* prestkeys) {
	if (prestkeys == NULL)
		return;

	if (prestkeys->phead == NULL) {
		mlhmmv_level_free(pmap->proot_level);
		pmap->proot_level = mlhmmv_level_alloc();
		return;
	}

	int unused = FALSE;
	mlhmmv_remove_aux(pmap->proot_level, prestkeys->phead, &unused, 0);
}

// ----------------------------------------------------------------
static void mlhmmv_level_enlarge(mlhmmv_level_t* plevel) {
	mlhmmv_level_entry_t*       old_entries = plevel->entries;
	mlhmmv_level_entry_state_t* old_states  = plevel->states;
	mlhmmv_level_entry_t*       old_head    = plevel->phead;

	mlhmmv_level_init(plevel, plevel->array_length*ENLARGEMENT_FACTOR);

	for (mlhmmv_level_entry_t* pentry = old_head; pentry != NULL; pentry = pentry->pnext) {
		mlhmmv_level_move(plevel, &pentry->level_key, &pentry->level_value);
	}
	free(old_entries);
	free(old_states);
}

// ----------------------------------------------------------------
// xxx temp
#define TEMP_FLATTEN_SEP ":"

// xxx comment copiously @ .h, and interleaved here
void mlhmmv_to_lrecs(mlhmmv_t* pmap, sllmv_t* pnames, sllv_t* poutrecs) {
	if (pnames->phead == NULL) {
		// xxx make a separate entry point
		// emit the entire map as lrecs
		for (mlhmmv_level_entry_t* pentry = pmap->proot_level->phead; pentry != NULL; pentry = pentry->pnext) {
			sllmv_t* pname = sllmv_single_no_free(&pentry->level_key);
			mlhmmv_to_lrecs(pmap, pname, poutrecs);
			sllmv_free(pname);
		}
		return;
	}

	mv_t* pfirstname = &pnames->phead->value;

	mlhmmv_level_entry_t* ptop_entry = mlhmmv_get_next_level_entry(pmap->proot_level, pfirstname, NULL);
	if (ptop_entry == NULL) {
		// xxx temp
	} else if (ptop_entry->level_value.is_terminal) {
		// xxx temp
		lrec_t* poutrec = lrec_unbacked_alloc();
		lrec_put(poutrec, mv_alloc_format_val(pfirstname),
			mv_alloc_format_val(&ptop_entry->level_value.u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
		sllv_append(poutrecs, poutrec);
	} else {
		lrec_t* ptemplate = lrec_unbacked_alloc();
		mlhmmv_to_lrecs_aux(ptop_entry->level_value.u.pnext_level, pfirstname, pnames->phead->pnext,
			ptemplate, poutrecs);
		lrec_free(ptemplate);
	}
}

static void mlhmmv_to_lrecs_aux(
	mlhmmv_level_t* plevel,
	mv_t*           pfirstname,
	sllmve_t*       prestnames,
	lrec_t*         ptemplate,
	sllv_t*         poutrecs)
{
	char* oosvar_name = mv_alloc_format_val(pfirstname);
	if (prestnames == NULL) {
		mlhmmv_to_lrecs_aux_flatten(plevel, oosvar_name, ptemplate, poutrecs);
	} else {
		for (mlhmmv_level_entry_t* pe = plevel->phead; pe != NULL; pe = pe->pnext) {
			lrec_t* pnextrec = lrec_copy(ptemplate);
			lrec_put(pnextrec, mv_alloc_format_val(&prestnames->value),
				mv_alloc_format_val(&pe->level_key), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
			mlhmmv_level_value_t* plevel_value = &pe->level_value;
			if (plevel_value->is_terminal) {
				lrec_put(pnextrec, mlr_strdup_or_die(oosvar_name),
					mv_alloc_format_val(&plevel_value->u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
				sllv_append(poutrecs, pnextrec);
			} else {
				mlhmmv_to_lrecs_aux(pe->level_value.u.pnext_level,
					pfirstname, prestnames->pnext, pnextrec, poutrecs);
				lrec_free(pnextrec);
			}
		}
	}
	free(oosvar_name);
}

static void mlhmmv_to_lrecs_aux_flatten(
	mlhmmv_level_t* plevel,
	char*           prefix,
	lrec_t*         ptemplate,
	sllv_t*         poutrecs)
{
	for (mlhmmv_level_entry_t* pe = plevel->phead; pe != NULL; pe = pe->pnext) {
		mlhmmv_level_value_t* plevel_value = &pe->level_value;
		lrec_t* pnextrec = lrec_copy(ptemplate);
		// xxx rename
		char* foo = mv_alloc_format_val(&pe->level_key);
		char* name = mlr_paste_3_strings(prefix, TEMP_FLATTEN_SEP, foo);
		free(foo);
		if (plevel_value->is_terminal) {
			lrec_put(pnextrec, name,
				mv_alloc_format_val(&plevel_value->u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
			sllv_append(poutrecs, pnextrec);
		} else {
			mlhmmv_to_lrecs_aux_flatten(plevel_value->u.pnext_level, name, pnextrec, poutrecs);
			lrec_free(pnextrec);
		}
	}
}

// ----------------------------------------------------------------
// This is simply JSON. Example output:
// {
//   "0":  {
//     "fghij":  {
//       "0":  17
//     }
//   },
//   "3":  4,
//   "abcde":  {
//     "-6":  7
//   }
// }

void mlhmmv_print_json_stacked(mlhmmv_t* pmap, int quote_values_always) {
	mlhmmv_level_print_stacked(pmap->proot_level, 0, FALSE, quote_values_always);
}

static void mlhmmv_level_print_stacked(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always)
{
	static char* leader = "  ";
	// Top-level opening brace goes on a line by itself; subsequents on the same line after the level key.
	if (depth == 0)
		printf("{\n");
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		for (int i = 0; i <= depth; i++)
			printf("%s", leader);
		char* level_key_string = mv_alloc_format_val(&pentry->level_key);
			printf("\"%s\": ", level_key_string);
		free(level_key_string);

		if (pentry->level_value.is_terminal) {
			char* level_value_string = mv_alloc_format_val(&pentry->level_value.u.mlrval);

			if (quote_values_always) {
				printf("\"%s\"", level_value_string);
			} else if (pentry->level_value.u.mlrval.type == MT_STRING) {
				double unused;
				if (mlr_try_float_from_string(level_value_string, &unused))
					json_decimal_print(level_value_string);
				else if (streq(level_value_string, "true") || streq(level_value_string, "false"))
					printf("%s", level_value_string);
				else
					printf("\"%s\"", level_value_string);
			} else {
				printf("%s", level_value_string);
			}

			free(level_value_string);
			if (pentry->pnext != NULL)
				printf(",\n");
			else
				printf("\n");
		} else {
			printf("{\n");
			mlhmmv_level_print_stacked(pentry->level_value.u.pnext_level, depth + 1,
				pentry->pnext != NULL, quote_values_always);
		}
	}
	for (int i = 0; i < depth; i++)
		printf("%s", leader);
	if (do_final_comma)
		printf("},\n");
	else
		printf("}\n");
}

// ----------------------------------------------------------------
void mlhmmv_print_json_single_line(mlhmmv_t* pmap, int quote_values_always) {
	mlhmmv_level_print_single_line(pmap->proot_level, 0, FALSE, quote_values_always);
	printf("\n");
}

static void mlhmmv_level_print_single_line(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always)
{
	// Top-level opening brace goes on a line by itself; subsequents on the same line after the level key.
	if (depth == 0)
		printf("{ ");
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		char* level_key_string = mv_alloc_format_val(&pentry->level_key);
			printf("\"%s\": ", level_key_string);
		free(level_key_string);

		if (pentry->level_value.is_terminal) {
			char* level_value_string = mv_alloc_format_val(&pentry->level_value.u.mlrval);

			if (quote_values_always) {
				printf("\"%s\"", level_value_string);
			} else if (pentry->level_value.u.mlrval.type == MT_STRING) {
				double unused;
				if (mlr_try_float_from_string(level_value_string, &unused))
					json_decimal_print(level_value_string);
				else if (streq(level_value_string, "true") || streq(level_value_string, "false"))
					printf("%s", level_value_string);
				else
					printf("\"%s\"", level_value_string);
			} else {
				printf("%s", level_value_string);
			}

			free(level_value_string);
			if (pentry->pnext != NULL)
				printf(", ");
		} else {
			printf("{");
			mlhmmv_level_print_single_line(pentry->level_value.u.pnext_level, depth + 1,
				pentry->pnext != NULL, quote_values_always);
		}
	}
	if (do_final_comma)
		printf(" },");
	else
		printf(" }");
}

// ----------------------------------------------------------------
// 0.123 is valid JSON; .123 is not. Meanwhile is a format-converter tool so if there is
// perfectly legitimate CSV/DKVP/etc. data to be JSON-formatted, we make it JSON-compliant.
//
// Precondition: the caller has already checked that the string represents a number.
static void json_decimal_print(char* s) {
	if (s[0] == '.') {
		printf("0%s", s);
	} else if (s[0] == '-' && s[1] == '.') {
		printf("-0.%s", &s[2]);
	} else {
		printf("%s", s);
	}
}

// ----------------------------------------------------------------
typedef int mlhmmv_typed_hash_func(mv_t* pa);

static int mlhmmv_string_hash_func(mv_t* pa) {
	return mlr_string_hash_func(pa->u.strv);
}
static int mlhmmv_int_hash_func(mv_t* pa) {
	return pa->u.intv;
}
static int mlhmmv_other_hash_func(mv_t* pa) {
	fprintf(stderr, "%s: @-variable keys must be of type %s or %s; got %s.\n",
		MLR_GLOBALS.argv0,
		mt_describe_type(MT_STRING),
		mt_describe_type(MT_INT),
		mt_describe_type(pa->type));
	exit(1);
}
static mlhmmv_typed_hash_func* mlhmmv_hash_func_dispositions[MT_DIM] = {
	/*ERROR*/  mlhmmv_other_hash_func,
	/*ABSENT*/ mlhmmv_other_hash_func,
	/*UNINIT*/ mlhmmv_other_hash_func,
	/*VOID*/   mlhmmv_other_hash_func,
	/*STRING*/ mlhmmv_string_hash_func,
	/*INT*/    mlhmmv_int_hash_func,
	/*FLOAT*/  mlhmmv_other_hash_func,
	/*BOOL*/   mlhmmv_other_hash_func,
};

static int mlhmmv_hash_func(mv_t* pa) {
	return mlhmmv_hash_func_dispositions[pa->type](pa);
}
