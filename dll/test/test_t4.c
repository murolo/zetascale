/*
 * Copyright (c) 2012-2013, Sandisk Corporation.  All rights reserved.
 */
#include <stdio.h>
#include "test.h"
#include "tlib.h"


/*
 * Test information.
 */
static char *Name = "t4";
static char *Desc = "test 4";


/*
 * A test.
 */
static void
test(fdf_t *fdf)
{
    int i;

    /* Initialize FDF */
    test_init(fdf, Name);

    for (i = 0; i < 16; i++) {
        char name[3] = {'C', 'a'+i, '\0'};
        fdf_ctr_t *ctr = open_ctr(fdf, name, FDF_CTNR_CREATE);
        set_obj(ctr, "indigo", name);
    }

    /* Create containers */
    fdf_ctr_t *ctr1 = open_ctr(fdf, "C0", FDF_CTNR_CREATE);
    fdf_ctr_t *ctr2 = open_ctr(fdf, "C1", FDF_CTNR_CREATE);

    /* Set some objects */
    set_obj(ctr1, "indigo", "horse");
    set_obj(ctr2, "indigo", "cow");
    set_obj(ctr1, "red",   "squirrel");
    set_obj(ctr2, "green", "alligator");

    /* Show all objects */
    show_objs(ctr1);
    show_objs(ctr2);

    /* Delete containers */
    delete_ctr(ctr1);
    delete_ctr(ctr2);

    /* Create containers */
    ctr1 = open_ctr(fdf, "C0", FDF_CTNR_CREATE);
    ctr2 = open_ctr(fdf, "C1", FDF_CTNR_CREATE);

    /* Set some objects */
    set_obj(ctr1, "indigo", "horse");
    set_obj(ctr2, "indigo", "cow");
    set_obj(ctr1, "red",   "squirrel");
    set_obj(ctr2, "green", "alligator");

    /* Set some more objects */
    set_obj(ctr1, "purple", "penguin");
    set_obj(ctr2, "purple", "porpoise");

    /* Show some objects */
    show_obj(ctr1, "indigo", "horse");
    show_obj(ctr2, "indigo", "cow");
    show_obj(ctr1, "red",   "squirrel");
    show_obj(ctr2, "green", "alligator");

    /* Show all objects */
    show_objs(ctr1);
    show_objs(ctr2);

    /* Close containers */
    fdf_ctr_close(ctr1, NULL);
    fdf_ctr_close(ctr2, NULL);

    /* Close FDF */
    fdf_done(fdf);
}


/*
 * Initialize the test.
 */
static __attribute__ ((constructor)) void
init(void)
{
    test_info(Name, Desc, NULL, NULL, test);
}
