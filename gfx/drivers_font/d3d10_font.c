﻿/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2018 - Daniel De Matteis
 *  Copyright (C) 2014-2018 - Ali Bouhlel
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#define CINTERFACE

#include <string.h>
#include <malloc.h>
#include <math.h>
#include <encodings/utf.h>

#include "../font_driver.h"
#include "../common/d3d10_common.h"

#include "../../configuration.h"
#include "../../verbosity.h"

typedef struct
{
   d3d10_texture_t               texture;
   const font_renderer_driver_t* font_driver;
   void*                         font_data;
   struct font_atlas*            atlas;
} d3d10_font_t;

static void*
d3d10_font_init_font(void* data, const char* font_path, float font_size, bool is_threaded)
{
   d3d10_video_t* d3d10 = (d3d10_video_t*)data;
   d3d10_font_t*  font  = (d3d10_font_t*)calloc(1, sizeof(*font));

   if (!font)
      return NULL;

   if (!font_renderer_create_default(
             &font->font_driver, &font->font_data, font_path, font_size))
   {
      RARCH_WARN("Couldn't initialize font renderer.\n");
      free(font);
      return NULL;
   }

   font->atlas               = font->font_driver->get_atlas(font->font_data);
   font->texture.sampler     = d3d10->samplers[RARCH_FILTER_LINEAR][RARCH_WRAP_BORDER];
   font->texture.desc.Width  = font->atlas->width;
   font->texture.desc.Height = font->atlas->height;
   font->texture.desc.Format = DXGI_FORMAT_A8_UNORM;
   d3d10_release_texture(&font->texture);
   d3d10_init_texture(d3d10->device, &font->texture);
   if (font->texture.staging)
      d3d10_update_texture(
            d3d10->device,
            font->atlas->width, font->atlas->height, font->atlas->width,
            DXGI_FORMAT_A8_UNORM, font->atlas->buffer, &font->texture);
   font->atlas->dirty        = false;

   return font;
}

static void d3d10_font_free_font(void* data, bool is_threaded)
{
   d3d10_font_t* font = (d3d10_font_t*)data;

   if (!font)
      return;

   if (font->font_driver && font->font_data && font->font_driver->free)
      font->font_driver->free(font->font_data);

   Release(font->texture.handle);
   Release(font->texture.staging);
   Release(font->texture.view);
   free(font);
}

static int d3d10_font_get_message_width(void* data, const char* msg, unsigned msg_len, float scale)
{
   unsigned i;
   int      delta_x                 = 0;
   const struct font_glyph* glyph_q = NULL;
   d3d10_font_t* font               = (d3d10_font_t*)data;

   if (!font)
      return 0;

   glyph_q = font->font_driver->get_glyph(font->font_data, '?');

   for (i = 0; i < msg_len; i++)
   {
      const struct font_glyph *glyph;
      const char* msg_tmp = &msg[i];
      unsigned    code    = utf8_walk(&msg_tmp);
      unsigned    skip    = msg_tmp - &msg[i];

      if (skip > 1)
         i += skip - 1;

      /* Do something smarter here ... */
      if (!(glyph = font->font_driver->get_glyph(font->font_data, code)))
         if (!(glyph = glyph_q))
            continue;

      delta_x += glyph->advance_x;
   }

   return delta_x * scale;
}

static void d3d10_font_render_line(
      d3d10_video_t *d3d10,
      d3d10_font_t*       font,
      const char*         msg,
      unsigned            msg_len,
      float               scale,
      const unsigned int  color,
      float               pos_x,
      float               pos_y,
      unsigned            width,
      unsigned            height,
      unsigned            text_align)
{
   int                      x, y;
   unsigned                 i, count;
   void*                    mapped_vbo;
   d3d10_sprite_t*          v;
   const struct font_glyph* glyph_q = NULL;

   if (  !d3d10                  ||
         !d3d10->sprites.enabled ||
         msg_len > (unsigned)d3d10->sprites.capacity)
      return;

   if (d3d10->sprites.offset + msg_len > (unsigned)d3d10->sprites.capacity)
      d3d10->sprites.offset = 0;

   x = roundf(pos_x * width);
   y = roundf((1.0 - pos_y) * height);

   switch (text_align)
   {
      case TEXT_ALIGN_RIGHT:
         x -= d3d10_font_get_message_width(font, msg, msg_len, scale);
         break;

      case TEXT_ALIGN_CENTER:
         x -= d3d10_font_get_message_width(font, msg, msg_len, scale) / 2;
         break;
   }

   d3d10->sprites.vbo->lpVtbl->Map(d3d10->sprites.vbo,
         D3D10_MAP_WRITE_NO_OVERWRITE, 0, (void**)&mapped_vbo);

   v       = (d3d10_sprite_t*)mapped_vbo + d3d10->sprites.offset;
   glyph_q = font->font_driver->get_glyph(font->font_data, '?');

   for (i = 0; i < msg_len; i++)
   {
      const struct font_glyph* glyph;
      const char *msg_tmp= &msg[i];
      unsigned   code    = utf8_walk(&msg_tmp);
      unsigned   skip    = msg_tmp - &msg[i];

      if (skip > 1)
         i += skip - 1;

      /* Do something smarter here ... */
      if (!(glyph = font->font_driver->get_glyph(font->font_data, code)))
         if (!(glyph = glyph_q))
            continue;

      v->pos.x           = (x + (glyph->draw_offset_x * scale)) / (float)d3d10->viewport.Width;
      v->pos.y           = (y + (glyph->draw_offset_y * scale)) / (float)d3d10->viewport.Height;
      v->pos.w           = glyph->width * scale / (float)d3d10->viewport.Width;
      v->pos.h           = glyph->height * scale / (float)d3d10->viewport.Height;

      v->coords.u        = glyph->atlas_offset_x / (float)font->texture.desc.Width;
      v->coords.v        = glyph->atlas_offset_y / (float)font->texture.desc.Height;
      v->coords.w        = glyph->width / (float)font->texture.desc.Width;
      v->coords.h        = glyph->height / (float)font->texture.desc.Height;

      v->params.scaling  = 1;
      v->params.rotation = 0;

      v->colors[0]       = color;
      v->colors[1]       = color;
      v->colors[2]       = color;
      v->colors[3]       = color;

      v++;

      x                 += glyph->advance_x * scale;
      y                 += glyph->advance_y * scale;
   }

   count = v - ((d3d10_sprite_t*)mapped_vbo + d3d10->sprites.offset);
   d3d10->sprites.vbo->lpVtbl->Unmap(d3d10->sprites.vbo);

   if (!count)
      return;

   if (font->atlas->dirty)
   {
      if (font->texture.staging)
         d3d10_update_texture(
               d3d10->device,
               font->atlas->width, font->atlas->height, font->atlas->width,
               DXGI_FORMAT_A8_UNORM, font->atlas->buffer, &font->texture);
      font->atlas->dirty = false;
   }

   d3d10_set_texture_and_sampler(d3d10->device, 0, &font->texture);
   d3d10->device->lpVtbl->OMSetBlendState(d3d10->device,
         d3d10->blend_enable,
         NULL, D3D10_DEFAULT_SAMPLE_MASK);

   d3d10->device->lpVtbl->PSSetShader(d3d10->device, d3d10->sprites.shader_font.ps);
   d3d10->device->lpVtbl->Draw(d3d10->device, count, d3d10->sprites.offset);
   d3d10->device->lpVtbl->PSSetShader(d3d10->device, d3d10->sprites.shader.ps);

   d3d10->sprites.offset += count;
}

static void d3d10_font_render_message(
      d3d10_video_t *d3d10,
      d3d10_font_t*       font,
      const char*         msg,
      float               scale,
      const unsigned int  color,
      float               pos_x,
      float               pos_y,
      unsigned            width,
      unsigned            height,
      unsigned            text_align)
{
   struct font_line_metrics *line_metrics = NULL;
   int lines                              = 0;
   float line_height;

   if (!msg || !*msg)
      return;

   /* If font line metrics are not supported just draw as usual */
   if (!font->font_driver->get_line_metrics ||
       !font->font_driver->get_line_metrics(font->font_data, &line_metrics))
   {
      d3d10_font_render_line(d3d10,
            font, msg, strlen(msg), scale, color, pos_x, pos_y,
            width, height, text_align);
      return;
   }

   line_height = line_metrics->height * scale / height;

   for (;;)
   {
      const char* delim = strchr(msg, '\n');
      unsigned msg_len  = delim ?
         (unsigned)(delim - msg) : strlen(msg);

      /* Draw the line */
      d3d10_font_render_line(d3d10,
            font, msg, msg_len, scale, color, pos_x,
            pos_y - (float)lines * line_height,
            width, height, text_align);

      if (!delim)
         break;

      msg += msg_len + 1;
      lines++;
   }
}

static void d3d10_font_render_msg(
      void *userdata,
      void* data,
      const char* msg,
      const struct font_params *params)
{
   float                     x, y, scale, drop_mod, drop_alpha;
   int                       drop_x, drop_y;
   enum text_alignment       text_align;
   unsigned                  color, r, g, b, alpha;
   d3d10_font_t*             font   = (d3d10_font_t*)data;
   d3d10_video_t*           d3d10   = (d3d10_video_t*)userdata;
   unsigned                  width  = d3d10->vp.full_width;
   unsigned                  height = d3d10->vp.full_height;

   if (!font || !msg || !*msg)
      return;

   if (params)
   {
      x          = params->x;
      y          = params->y;
      scale      = params->scale;
      text_align = params->text_align;
      drop_x     = params->drop_x;
      drop_y     = params->drop_y;
      drop_mod   = params->drop_mod;
      drop_alpha = params->drop_alpha;

      r          = FONT_COLOR_GET_RED(params->color);
      g          = FONT_COLOR_GET_GREEN(params->color);
      b          = FONT_COLOR_GET_BLUE(params->color);
      alpha      = FONT_COLOR_GET_ALPHA(params->color);

      color      = DXGI_COLOR_RGBA(r, g, b, alpha);
   }
   else
   {
      settings_t *settings     = config_get_ptr();
      float video_msg_pos_x    = settings->floats.video_msg_pos_x;
      float video_msg_pos_y    = settings->floats.video_msg_pos_y;
      float video_msg_color_r  = settings->floats.video_msg_color_r;
      float video_msg_color_g  = settings->floats.video_msg_color_g;
      float video_msg_color_b  = settings->floats.video_msg_color_b;
      x                        = video_msg_pos_x;
      y                        = video_msg_pos_y;
      scale                    = 1.0f;
      text_align               = TEXT_ALIGN_LEFT;

      r                        = (video_msg_color_r * 255);
      g                        = (video_msg_color_g * 255);
      b                        = (video_msg_color_b * 255);
      alpha                    = 255;
      color                    = DXGI_COLOR_RGBA(r, g, b, alpha);

      drop_x                   = -2;
      drop_y                   = -2;
      drop_mod                 = 0.3f;
      drop_alpha               = 1.0f;
   }

   if (drop_x || drop_y)
   {
      unsigned r_dark          = r * drop_mod;
      unsigned g_dark          = g * drop_mod;
      unsigned b_dark          = b * drop_mod;
      unsigned alpha_dark      = alpha * drop_alpha;
      unsigned color_dark      = DXGI_COLOR_RGBA(r_dark, g_dark, b_dark, alpha_dark);

      d3d10_font_render_message(d3d10,
            font, msg, scale, color_dark,
            x + scale * drop_x / width,
            y + scale * drop_y / height,
            width, height, text_align);
   }

   d3d10_font_render_message(d3d10, font, msg, scale,
         color, x, y,
         width, height, text_align);
}

static const struct font_glyph* d3d10_font_get_glyph(void *data, uint32_t code)
{
   d3d10_font_t* font = (d3d10_font_t*)data;
   if (font && font->font_driver && font->font_driver->ident)
      return font->font_driver->get_glyph((void*)font->font_driver, code);
   return NULL;
}

static bool d3d10_font_get_line_metrics(void* data, struct font_line_metrics **metrics)
{
   d3d10_font_t* font = (d3d10_font_t*)data;
   if (font && font->font_driver && font->font_data)
      return font->font_driver->get_line_metrics(font->font_data, metrics);
   return -1;
}

font_renderer_t d3d10_font = {
   d3d10_font_init_font,
   d3d10_font_free_font,
   d3d10_font_render_msg,
   "d3d10font",
   d3d10_font_get_glyph,
   NULL, /* bind_block */
   NULL, /* flush */
   d3d10_font_get_message_width,
   d3d10_font_get_line_metrics
};
