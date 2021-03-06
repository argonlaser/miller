#include <stdio.h> // For definition of EOF
#include "input/byte_readers.h"
#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"

typedef struct _string_byte_reader_state_t {
	char* backing;
	char* p;
	char* pend;
} string_byte_reader_state_t;

static int  string_byte_reader_open_func(struct _byte_reader_t* pbr, char* prepipe, char* backing);
static int  string_byte_reader_read_func(struct _byte_reader_t* pbr);
static void string_byte_reader_close_func(struct _byte_reader_t* pbr, char* prepipe);

// ----------------------------------------------------------------
byte_reader_t* string_byte_reader_alloc() {
	byte_reader_t* pbr = mlr_malloc_or_die(sizeof(byte_reader_t));

	pbr->pvstate     = NULL;
	pbr->popen_func  = string_byte_reader_open_func;
	pbr->pread_func  = string_byte_reader_read_func;
	pbr->pclose_func = string_byte_reader_close_func;

	return pbr;
}

void string_byte_reader_free(byte_reader_t* pbr) {
	free(pbr);
}

// ----------------------------------------------------------------
static int string_byte_reader_open_func(struct _byte_reader_t* pbr, char* prepipe, char* backing) {
	// popen is a stdio construct, not an mmap construct, and it can't be supported here.
	if (prepipe != NULL) {
		fprintf(stderr, "%s: coding error detected in file %s at line %d.\n",
			MLR_GLOBALS.bargv0, __FILE__, __LINE__);
		exit(1);
	}

	string_byte_reader_state_t* pstate = mlr_malloc_or_die(sizeof(string_byte_reader_state_t));
	pstate->backing = backing;
	pstate->p       = pstate->backing;
	pstate->pend    = pstate->backing + strlen(pstate->backing);
	pbr->pvstate    = pstate;
	return TRUE;
}

static int string_byte_reader_read_func(struct _byte_reader_t* pbr) {
	string_byte_reader_state_t* pstate = pbr->pvstate;
	if (pstate->p < pstate->pend) {
		return *(pstate->p++);
	} else {
		return EOF;
	}
}

static void string_byte_reader_close_func(struct _byte_reader_t* pbr, char* prepipe) {
	pbr->pvstate = NULL;
}
