/**
 * \file dlist.h
 * Display lists management.
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2006  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */



#ifndef DLIST_H
#define DLIST_H

#include <stdio.h>

struct gl_context;

/**
 * Display list node.
 *
 * Display list instructions are stored as sequences of "nodes".  Nodes
 * are allocated in blocks.  Each block has BLOCK_SIZE nodes.  Blocks
 * are linked together with a pointer.
 *
 * Each instruction in the display list is stored as a sequence of
 * contiguous nodes in memory.
 * Each node is the union of a variety of data types.
 *
 * Note, all of these members should be 4 bytes in size or less for the
 * sake of compact display lists.  We store 8-byte pointers in a pair of
 * these nodes using the save/get_pointer() functions below.
 */
union gl_dlist_node
{
   struct {
      uint16_t opcode; /* dlist.c : enum Opcode */
      uint16_t InstSize;
   };
   GLboolean b;
   GLbitfield bf;
   GLubyte ub;
   GLshort s;
   GLushort us;
   GLint i;
   GLuint ui;
   GLenum e;
   GLfloat f;
   GLsizei si;
};

/**
 * Describes the location and size of a glBitmap image in a texture atlas.
 */
struct gl_bitmap_glyph
{
   unsigned short x, y, w, h;  /**< position and size in the texture */
   float xorig, yorig;         /**< bitmap origin */
   float xmove, ymove;         /**< rasterpos move */
};


/**
 * Describes a set of glBitmap display lists which live in a texture atlas.
 * The idea is when we see a code sequence of glListBase(b), glCallLists(n)
 * we're probably drawing bitmap font glyphs.  We try to put all the bitmap
 * glyphs into one texture map then render the glCallLists as a textured
 * quadstrip.
 */
struct gl_bitmap_atlas
{
   GLint Id;
   bool complete;     /**< Is the atlas ready to use? */
   bool incomplete;   /**< Did we fail to construct this atlas? */

   unsigned numBitmaps;
   unsigned texWidth, texHeight;
   struct gl_texture_object *texObj;
   struct gl_texture_image *texImage;

   unsigned glyphHeight;

   struct gl_bitmap_glyph *glyphs;
};

void
_mesa_delete_bitmap_atlas(struct gl_context *ctx,
                          struct gl_bitmap_atlas *atlas);

struct gl_display_list *
_mesa_lookup_list(struct gl_context *ctx, GLuint list, bool locked);

void
_mesa_compile_error(struct gl_context *ctx, GLenum error, const char *s);

void *
_mesa_dlist_alloc_vertex_list(struct gl_context *ctx,
                              bool copy_to_current);

void
_mesa_delete_list(struct gl_context *ctx, struct gl_display_list *dlist);

void
_mesa_initialize_save_table(const struct gl_context *);

void
_mesa_init_display_list(struct gl_context * ctx);

void
_mesa_install_save_vtxfmt(struct gl_context *ctx);

bool
_mesa_get_list(struct gl_context *ctx, GLuint list,
               struct gl_display_list **dlist,
               bool locked);

#endif /* DLIST_H */
