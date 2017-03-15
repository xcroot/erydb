/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

// The purpose of this test is to test the error recovery of the extractor.  We inject errors into the extractor and
// verify that the extractor error state is set.

#define DONT_DEPRECATE_MALLOC
#define DONT_DEPRECATE_WRITES
#include "test.h"
#include "loader/loader.h"
#include "loader/loader-internal.h"
#include "ftloader-error-injector.h"
#include "memory.h"
#include <portability/toku_path.h>


static int generate(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    (void) dest_db; (void) src_db; (void) src_key; (void) src_val;
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);

    copy_dbt(&dest_keys->dbts[0], src_key);
    copy_dbt(&dest_vals->dbts[0], src_val);
    
    return 0;
}

static int qsort_compare_ints (const void *a, const void *b) {
    int avalue = *(int*)a;
    int bvalue = *(int*)b;
    if (avalue<bvalue) return -1;
    if (avalue>bvalue) return +1;
    return 0;
}

static int compare_int(DB *desc, const DBT *akey, const DBT *bkey) {
    assert(desc == NULL);
    assert(akey->size == sizeof (int));
    assert(bkey->size == sizeof (int));
    return qsort_compare_ints(akey->data, bkey->data);
}

static void populate_rowset(struct rowset *rowset, int seq, int nrows, int keys[]) {
    for (int i = 0; i < nrows; i++) {
        int k = keys[i];
        int v = seq * nrows + i;
        DBT key;
        toku_fill_dbt(&key, &k, sizeof k);
        DBT val;
        toku_fill_dbt(&val, &v, sizeof v);
        add_row(rowset, &key, &val);
    }
}

static void shuffle(int a[], int n) {
    for (int i = 0; i < n; i++) {
        int r = random() % n;
        int t = a[i]; a[i] = a[r]; a[r] = t;
    }
}

static int ascending_keys = 0;
static int descending_keys = 0;
static int random_keys = 0;

static void test_extractor(int nrows, int nrowsets, bool expect_fail, const char *testdir) {
    if (verbose) printf("%s %d %d %s\n", __FUNCTION__, nrows, nrowsets, testdir);

    int r;

    int nkeys = nrows * nrowsets;
    int *XMALLOC_N(nkeys, keys);
    for (int i = 0; i < nkeys; i++)
        keys[i] = ascending_keys ? i : nkeys - i;
    if (random_keys)
        shuffle(keys, nkeys);

    // open the ft_loader. this runs the extractor.
    const int N = 1;
    FT_HANDLE fts[N];
    DB* dbs[N];
    const char *fnames[N];
    ft_compare_func compares[N];
    for (int i = 0; i < N; i++) {
        fts[i] = NULL;
        dbs[i] = NULL;
        fnames[i] = "";
        compares[i] = compare_int;
    }

    char temp[strlen(testdir) + 1 + strlen("tempXXXXXX") + 1];
    sprintf(temp, "%s/%s", testdir, "tempXXXXXX");

    FTLOADER loader;
    r = toku_ft_loader_open(&loader, NULL, generate, NULL, N, fts, dbs, fnames, compares, "tempXXXXXX", ZERO_LSN, nullptr, true, 0, false, true);
    assert(r == 0);

    struct rowset *rowset[nrowsets];
    for (int i = 0 ; i < nrowsets; i++) {
        rowset[i] = (struct rowset *) toku_malloc(sizeof (struct rowset));
        assert(rowset[i]);
        init_rowset(rowset[i], toku_ft_loader_get_rowset_budget_for_testing());
        populate_rowset(rowset[i], i, nrows, &keys[i*nrows]);
    }

    // setup error injection
    toku_set_func_malloc(my_malloc);
    toku_set_func_realloc(my_realloc);
    ft_loader_set_os_fwrite(bad_fwrite);
    toku_set_func_write(bad_write);
    toku_set_func_pwrite(bad_pwrite);
    ft_loader_set_poll_function(&loader->poll_callback, loader_poll_callback, NULL);

    // feed rowsets to the extractor
    for (int i = 0; i < nrowsets; i++) {
        r = toku_queue_enq(loader->primary_rowset_queue, rowset[i], 1, NULL);
        assert(r == 0);
    }

    r = toku_ft_loader_finish_extractor(loader);
    assert(r == 0);

    toku_set_func_malloc(NULL);
    toku_set_func_realloc(NULL);
    ft_loader_set_os_fwrite(NULL);
    toku_set_func_write(NULL);
    toku_set_func_pwrite(NULL);

    int error;
    r = toku_ft_loader_get_error(loader, &error);
    assert(r == 0);
    assert(expect_fail ? error != 0 : error == 0);

    // verify the temp files

    // abort the ft_loader.  this ends the test
    r = toku_ft_loader_abort(loader, true);
    assert(r == 0);

    toku_free(keys);
}
static int nrows = 1;
static int nrowsets = 2;

static int usage(const char *progname) {
    fprintf(stderr, "Usage: %s [options] directory\n", progname);
    fprintf(stderr, "[-v] turn on verbose\n");
    fprintf(stderr, "[-q] turn off verbose\n");
    fprintf(stderr, "[-r %d] set the number of rows\n", nrows);
    fprintf(stderr, "[--rowsets %d] set the number of rowsets\n", nrowsets);
    fprintf(stderr, "[-s] set the small loader size factor\n");
    fprintf(stderr, "[-m] inject big malloc and realloc errors\n");
    fprintf(stderr, "[--malloc_limit %u] set the threshold for failing malloc and realloc\n", (unsigned) my_big_malloc_limit);
    fprintf(stderr, "[-w] inject write errors\n");
    fprintf(stderr, "[-u] inject user errors\n");
    return 1;
}

int test_main (int argc, const char *argv[]) {
    const char *progname=argv[0];
    int max_error_limit = -1;
    argc--; argv++;
    while (argc>0) {
        if (strcmp(argv[0],"-h")==0) {
            return usage(progname);
        } else if (strcmp(argv[0],"-v")==0) {
	    verbose=1;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose=0;
        } else if (strcmp(argv[0],"-r") == 0 && argc >= 1) {
            argc--; argv++;
            nrows = atoi(argv[0]);
        } else if (strcmp(argv[0],"--rowsets") == 0 && argc >= 1) {
            argc--; argv++;
            nrowsets = atoi(argv[0]);
        } else if (strcmp(argv[0],"-s") == 0) {
            toku_ft_loader_set_size_factor(1);
        } else if (strcmp(argv[0],"-w") == 0) {
            do_write_errors = 1;
        } else if (strcmp(argv[0],"-m") == 0) {
            do_malloc_errors = 1;
        } else if (strcmp(argv[0],"-u") == 0) {
            do_user_errors = 1;
        } else if (strcmp(argv[0],"--malloc_limit") == 0 && argc > 1) {
            argc--; argv++;
            my_big_malloc_limit = atoi(argv[0]);
        } else if (strcmp(argv[0],"--max_error_limit") == 0 && argc >= 1) {
            argc--; argv++;
            max_error_limit = atoi(argv[0]);
        } else if (strcmp(argv[0],"--asc") == 0) {
            ascending_keys = 1;
        } else if (strcmp(argv[0],"--dsc") == 0) {
            descending_keys = 1;
        } else if (strcmp(argv[0],"--random") == 0) {
            random_keys = 1;
	} else if (argc!=1) {
            return usage(progname);
	    exit(1);
	}
        else {
            break;
        }
	argc--; argv++;
    }

    const char *testdir = TOKU_TEST_FILENAME;

    if (ascending_keys + descending_keys + random_keys == 0)
        ascending_keys = 1;

    // callibrate
    test_extractor(nrows, nrowsets, false, testdir);

    // run tests
    int error_limit = event_count;
    if (verbose) printf("error_limit=%d\n", error_limit);

    if (max_error_limit != -1 && error_limit > max_error_limit)
        error_limit = max_error_limit;
    for (int i = 1; i <= error_limit; i++) {
        reset_event_counts();
        reset_my_malloc_counts();
        event_count_trigger = i;
        test_extractor(nrows, nrowsets, true, testdir);
    }

    return 0;
}
