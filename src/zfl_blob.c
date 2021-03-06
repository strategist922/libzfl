/*  =========================================================================
    zfl_blob - work with binary long objects (blobs)

    -------------------------------------------------------------------------
    Copyright (c) 1991-2011 iMatix Corporation <www.imatix.com>
    Copyright other contributors as noted in the AUTHORS file.

    This file is part of libzfl, http://libzfl.zeromq.org.

    This is free software; you can redistribute it and/or modify it under the
    terms of the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your option)
    any later version.

    This software is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABIL-
    ITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
    Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
    =========================================================================
*/

/*
@header
    Manipulates opaque binary objects including reading and writing from/to
    files.  Example use case is for loading config data from stdin or file
    for processing by zfl_config.
@discuss
@end
*/

#include <czmq.h>
#include "../include/zfl_blob.h"

//  Structure of our class

struct _zfl_blob_t {
    size_t
        size;                   //  Blob data size
    byte
        *data,                  //  Content of our blob
        *dptr;                  //  Or, pointer to data
};


//  --------------------------------------------------------------------------
//  Constructor

zfl_blob_t *
zfl_blob_new (byte *data, size_t size)
{
    zfl_blob_t
        *self;

    self = (zfl_blob_t *) zmalloc (sizeof (zfl_blob_t));
    if (data)
        zfl_blob_set_data (self, data, size);

    return self;
}

//  --------------------------------------------------------------------------
//  Destructor

void
zfl_blob_destroy (zfl_blob_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zfl_blob_t *self = *self_p;
        free (self->data);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Loads blob from file.  Always adds a binary zero to end of blob data so
//  that it can be parsed as a string if necessary.  Returns size of blob
//  data.  Idempotent, does not change current read position in file. If
//  file cannot be read, returns empty blob.

zfl_blob_t *
zfl_blob_load (FILE *file)
{
    long
        posn,                   //  Current position in file
        size;                   //  Size of file data

    zfl_blob_t *self = zfl_blob_new (NULL, 0);
    //  Get current position in file so we can come back here afterwards
    if (file)
        posn = ftell (file);
    if (file && fseek (file, 0, SEEK_END) == 0) {
        //  Now determine actual size of blob in file
        size = ftell (file);
        assert (size >= 0);

        //  Read file data, and then reset file position
        byte *buffer = (byte *) malloc (size);
        fseek (file, 0, SEEK_SET);
        size_t rc = fread (buffer, 1, size, file);
#if (defined (__UNIX__))
        assert (rc == size);
#elif (defined (__WINDOWS__))
       //  Windows will not count CRs in text files
        assert (rc <= (size_t) size);
        size = rc;
#endif
        fseek (file, posn, SEEK_SET);

        zfl_blob_set_data (self, buffer, size);
        free (buffer);
    }
    else
        zfl_blob_set_data (self, NULL, 0);

    return self;
}


//  --------------------------------------------------------------------------
//  Sets blob data as specified.  Always appends a null byte to the data.
//  Data is copied to blob. If data is null and size is zero, clears blob.
//  Use like this:
//
//      zfl_blob_set_data (blob, buffer, size);
//      zfl_blob_set_data (blob, object, sizeof (*object));
//      zfl_blob_set_data (blob, NULL, 0);

int
zfl_blob_set_data (zfl_blob_t *self, byte *data, size_t size)
{
    assert (self);

    free (self->data);
    self->dptr = NULL;          //  No data reference
    self->size = size;
    if (data) {
        self->data = (byte *) malloc (size + 1);
        memcpy (self->data, data, size);
        self->data [size] = 0;
    }
    else {
        assert (size == 0);
        self->data = NULL;
    }
    return 0;
}


//  --------------------------------------------------------------------------
//  Sets blob data as specified.  Does zero-copy, original data should not be
//  freed during lifetime of blob.
//
//      zfl_blob_set_dptr (blob, object, sizeof (*object));

int
zfl_blob_set_dptr (zfl_blob_t *self, byte *data, size_t size)
{
    assert (self);

    free (self->data);          //  Free any copied data
    self->dptr = data;          //  Hold data reference
    self->size = size;

    return 0;
}


//  --------------------------------------------------------------------------
//  Returns pointer to blob data.

byte *
zfl_blob_data (zfl_blob_t *self)
{
    assert (self);
    return self->data? self->data: self->dptr;
}


//  --------------------------------------------------------------------------
//  Returns size of blob data.

size_t
zfl_blob_size (zfl_blob_t *self)
{
    assert (self);
    return self->size;
}


//  --------------------------------------------------------------------------
//  Selftest

int
zfl_blob_test (Bool verbose)
{
    printf (" * zfl_blob: ");

    //  @selftest
    //  Try to load blob from missing file
    zfl_blob_t *blob = zfl_blob_load (NULL);
    assert (zfl_blob_size (blob) == 0);
    zfl_blob_set_data (blob, NULL, 0);
    assert (zfl_blob_size (blob) == 0);
    zfl_blob_destroy (&blob);

    //  Load blob from existing file
    FILE *file = fopen ("zfl_blob.c", "r");
    assert (file);
    blob = zfl_blob_load (file);
    assert (blob);
    fclose (file);

    char *string = "This is a string";
    assert (zfl_blob_size (blob) > 0);
    zfl_blob_set_data (blob, (byte *) string, strlen (string));
    assert (zfl_blob_size (blob) == strlen (string));
    assert (streq ((char *) (zfl_blob_data (blob)), string));

    //  Destructor should be safe to call twice
    zfl_blob_destroy (&blob);
    zfl_blob_destroy (&blob);
    assert (blob == NULL);
    //  @end

    printf ("OK\n");
    return 0;
}
